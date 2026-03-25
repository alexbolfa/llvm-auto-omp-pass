#include "AutoOMPPass.h"
#include "LoopCandidate.h"
#include "LoopCodegen.h"

#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Frontend/OpenMP/OMPIRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

using namespace llvm;

#define DEBUG_TYPE "auto-omp"

static Value *getMemPointer(Instruction &I) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
        return LI->getPointerOperand();
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
        return SI->getPointerOperand();
    }
    return nullptr;
}

static std::optional<ReductionInfo> detectReduction(PHINode &PN, Loop *L) {
    Value *InitVal = PN.getIncomingValueForBlock(L->getLoopPreheader());
    Value *LatchVal = PN.getIncomingValueForBlock(L->getLoopLatch());
    if (!InitVal || !LatchVal) {
        return std::nullopt;
    }

    auto *Update = dyn_cast<Instruction>(LatchVal);
    // Note: FP requires -ffast-math
    if (!Update || !Update->isBinaryOp() ||
        (Update->getOperand(0) != &PN && Update->getOperand(1) != &PN) ||
        !Update->isAssociative() || !Update->isCommutative()) {
        return std::nullopt;
    }

    return ReductionInfo{&PN, InitVal, Update};
}

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

static bool checkSafety(Loop *L, DependenceInfo &DI, Function &F, LoopCandidate &LC, auto &Remark) {
    for (PHINode &PN : LC.Header->phis()) {
        if (&PN == LC.IV) {
            continue;
        }
        auto R = detectReduction(PN, L);
        if (!R) {
            Remark("❌ unsupported loop-carried dependency");
            return false;
        }
        LC.Reductions.push_back(*R);
    }

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

    // Reject atomics and any call with side effects.
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
            if (CB->onlyReadsMemory() || CB->getMemoryEffects().onlyAccessesInaccessibleMem()) {
                continue;
            }
            Remark("❌ call with side effects");
            return false;
        }
    }

    SmallVector<Instruction *, 16> MemInsts;
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            Value *Ptr = getMemPointer(I);
            if (!Ptr) {
                continue;
            }
            bool IsLocal = LC.LoopLocalAllocas.count(getUnderlyingObject(Ptr));
            if (isa<StoreInst>(I) && L->isLoopInvariant(Ptr) && !IsLocal) {
                Remark("❌ store to loop-invariant address");
                return false;
            }
            if (!IsLocal) {
                MemInsts.push_back(&I);
            }
        }
    }

    for (auto *Src : MemInsts) {
        for (auto *Dst : MemInsts) {
            if (isa<LoadInst>(Src) && isa<LoadInst>(Dst)) {
                continue;
            }
            auto D = DI.depends(Src, Dst, true);
            if (!D) {
                continue;
            }
            if (D->isConfused()) {
                Remark("❌ potential cross-iteration memory dependency");
                return false;
            }
            unsigned Levels = D->getLevels();
            if (Levels == 0) {
                continue;
            }
            unsigned Dir = D->getDirection(1);
            if (Dir & (Dependence::DVEntry::LT | Dependence::DVEntry::GT)) {
                Remark("❌ cross-iteration memory dependency");
                return false;
            }
        }
    }

    return true;
}

static bool parallelizeLoop(Loop *L, OpenMPIRBuilder &OMPBuilder, ScalarEvolution &SE,
                            DependenceInfo &DI, Function &F, auto &Remark) {
    LoopCandidate LC;
    if (!checkStructure(L, SE, LC, Remark)) {
        return false;
    }
    if (!checkSafety(L, DI, F, LC, Remark)) {
        return false;
    }

    if (LC.Reductions.empty()) {
        Remark("✅ parallelizing: #omp parallel for simd");
    } else {
        Remark("✅ parallelizing: #omp parallel for simd reduction");
    }

    return emitParallelLoop(L, LC, OMPBuilder, F);
}

PreservedAnalyses AutoOMPPass::run(Function &F, FunctionAnalysisManager &AM) {
    bool hadRemark = F.hasFnAttribute("__skip_auto_omp");
    F.addFnAttr("__skip_auto_omp", "true");

    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
    auto &DI = AM.getResult<DependenceAnalysis>(F);

    OpenMPIRBuilder OMPBuilder(*F.getParent());
    OMPBuilder.Config.IsTargetDevice = false;
    OMPBuilder.initialize();

    auto OuterLoops = SmallVector<Loop *, 4>(LI.begin(), LI.end());
    bool Changed = false;

    for (Loop *L : OuterLoops) {
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
        Changed |= parallelizeLoop(L, OMPBuilder, SE, DI, F, Remark);
    }

    if (Changed) {
        OMPBuilder.finalize();
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
