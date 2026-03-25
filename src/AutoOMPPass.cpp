#include "AutoOMPPass.h"
#include "LoopCandidate.h"
#include "LoopCodegen.h"

#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Frontend/OpenMP/OMPIRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

using namespace llvm;

#define DEBUG_TYPE "auto-omp"

static bool checkStructure(Loop *L, ScalarEvolution &SE, LoopCandidate &LC, auto &Remark) {
    unsigned TC = SE.getSmallConstantTripCount(L);
    if (TC != 0 && TC <= 64) {
        Remark("❌ trip count too small to benefit from parallelization");
        return false;
    }

    LC.Preheader = L->getLoopPreheader();
    LC.Header = L->getHeader();
    LC.Latch = L->getLoopLatch();
    LC.ExitBlock = L->getExitBlock();
    LC.ExitingBlock = L->getExitingBlock();

    if (!LC.Preheader || !LC.Header || !LC.Latch || !LC.ExitBlock || !LC.ExitingBlock) {
        Remark("❌ loop lacks canonical structure");
        return false;
    }

    auto MaybeBounds = L->getBounds(SE);
    if (!MaybeBounds) {
        Remark("❌ cannot determine loop bounds");
        return false;
    }

    auto &Bounds = *MaybeBounds;
    LC.Start = &Bounds.getInitialIVValue();
    LC.Stop = &Bounds.getFinalIVValue();
    LC.Step = Bounds.getStepValue();
    LC.IV = L->getInductionVariable(SE);

    if (!LC.Step || !LC.IV) {
        Remark("❌ no canonical induction variable");
        return false;
    }

    auto Pred = Bounds.getCanonicalPredicate();
    if (Pred == ICmpInst::BAD_ICMP_PREDICATE) {
        Remark("❌ bad comparison predicate");
        return false;
    }

    LC.IsSigned = ICmpInst::isSigned(Pred);
    LC.Inclusive = (Pred == ICmpInst::ICMP_SLE || Pred == ICmpInst::ICMP_ULE);

    return true;
}

static bool checkSafety(Loop *L, Function &F, LoopCandidate &LC, const LoopAccessInfo &LAI,
                         auto &Remark) {
    // Validate reductions using RecurrenceDescriptor.
    for (PHINode &PN : LC.Header->phis()) {
        if (&PN == LC.IV) {
            continue;
        }
        RecurrenceDescriptor RedDes;
        if (!RecurrenceDescriptor::isReductionPHI(&PN, L, RedDes)) {
            Remark("❌ unsupported loop-carried dependency");
            return false;
        }
        Value *InitVal = PN.getIncomingValueForBlock(L->getLoopPreheader());
        auto *Update = cast<Instruction>(PN.getIncomingValueForBlock(L->getLoopLatch()));
        LC.Reductions.push_back({&PN, InitVal, Update});
    }

    // Reject reductions whose accumulator is stored to non-local memory
    // (e.g. prefix sum: arr[i] = running_total).
    if (!LC.Reductions.empty()) {
        SmallPtrSet<Value *, 4> RedVals;
        for (auto &R : LC.Reductions) {
            RedVals.insert(R.Phi);
            RedVals.insert(R.Update);
        }
        for (BasicBlock *BB : L->blocks()) {
            for (Instruction &I : *BB) {
                auto *SI = dyn_cast<StoreInst>(&I);
                if (SI && RedVals.count(SI->getValueOperand())) {
                    Remark("❌ reduction value stored to memory");
                    return false;
                }
            }
        }
    }

    for (Instruction &AI : F.getEntryBlock()) {
        auto *Alloca = dyn_cast<AllocaInst>(&AI);
        if (!Alloca || Alloca->user_empty()) {
            continue;
        }
        bool AllInLoop = llvm::all_of(Alloca->users(), [&](User *U) {
            auto *UI = dyn_cast<Instruction>(U);
            return UI && L->contains(UI->getParent());
        });
        if (AllInLoop) {
            LC.LoopLocalAllocas.insert(Alloca);
        }
    }

    // Reject atomics and any call that accesses memory.
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            if (I.isAtomic()) {
                Remark("❌ loop contains atomic instructions");
                return false;
            }
            auto *CB = dyn_cast<CallBase>(&I);
            if (!CB) {
                continue;
            }
            if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
                if (II->isLifetimeStartOrEnd() || II->isDebugOrPseudoInst()) {
                    continue;
                }
            }
            if (CB->doesNotAccessMemory() || CB->getMemoryEffects().onlyAccessesInaccessibleMem()) {
                continue;
            }
            Remark("❌ call with side effects");
            return false;
        }
    }

    // Memory dependence checking via LoopAccessInfo.
    if (!LAI.canVectorizeMemory() || LAI.getNumRuntimePointerChecks() > 0) {
        Remark("❌ unsafe memory dependence");
        return false;
    }

    // Reject if non-reduction loop-variant values escape via LCSSA PHIs.
    {
        SmallPtrSet<Value *, 4> RedVals;
        for (auto &R : LC.Reductions) {
            RedVals.insert(R.Phi);
            RedVals.insert(R.Update);
        }
        for (PHINode &PN : LC.ExitBlock->phis()) {
            for (unsigned i = 0; i < PN.getNumIncomingValues(); ++i) {
                auto *I = dyn_cast<Instruction>(PN.getIncomingValue(i));
                if (I && L->contains(I->getParent()) && !RedVals.count(I)) {
                    Remark("❌ non-reduction value escapes loop");
                    return false;
                }
            }
        }
    }

    return true;
}

static bool parallelizeLoop(Loop *L, OpenMPIRBuilder &OMPBuilder, ScalarEvolution &SE,
                            const LoopAccessInfo &LAI, Function &F, auto &Remark) {
    LoopCandidate LC;
    if (!checkStructure(L, SE, LC, Remark)) {
        return false;
    }
    if (!checkSafety(L, F, LC, LAI, Remark)) {
        return false;
    }

    if (LC.Reductions.empty()) {
        Remark("✅ #omp parallel for simd");
    } else {
        Remark("✅ #omp parallel for simd reduction");
    }

    return emitParallelLoop(L, LC, OMPBuilder, F);
}

PreservedAnalyses AutoOMPPass::run(Function &F, FunctionAnalysisManager &AM) {
    bool hadRemark = F.hasFnAttribute("__skip_auto_omp");
    F.addFnAttr("__skip_auto_omp", "true");

    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
    auto &LAM = AM.getResult<LoopAccessAnalysis>(F);

    OpenMPIRBuilder OMPBuilder(*F.getParent());
    OMPBuilder.Config.IsTargetDevice = false;
    OMPBuilder.initialize();

    SmallVector<Loop *, 8> Worklist;
    for (auto *TopLoop : LI) {
        for (auto *L : depth_first(TopLoop)) {
            if (L->isInnermost()) {
                Worklist.push_back(L);
            }
        }
    }
    bool Changed = false;

    for (Loop *L : Worklist) {
        auto Remark = [&](StringRef Msg) {
            if (hadRemark) {
                return;
            }
            ORE.emit([&]() {
                return OptimizationRemarkAnalysis(DEBUG_TYPE, "LoopParallelize", L->getStartLoc(),
                                                  L->getHeader())
                       << Msg;
            });
        };
        const LoopAccessInfo &LAI = LAM.getInfo(*L);
        Changed |= parallelizeLoop(L, OMPBuilder, SE, LAI, F, Remark);
    }

    if (Changed) {
        OMPBuilder.finalize();
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
