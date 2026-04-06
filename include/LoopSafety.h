#ifndef LOOP_SAFETY_H
#define LOOP_SAFETY_H

#include "LoopCandidate.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"

namespace llvm {

/// Trace a value in the callee to determine if it depends on a function
/// argument whose corresponding call-site operand varies with the loop IV.
static bool hasLoopVariantArgDep(Value *V, CallBase *CB, Loop *L) {
    SmallVector<Value *, 8> Worklist = {V};
    SmallPtrSet<Value *, 16> Visited;
    while (!Worklist.empty()) {
        Value *Cur = Worklist.pop_back_val();
        if (!Visited.insert(Cur).second) {
            continue;
        }
        if (auto *A = dyn_cast<Argument>(Cur)) {
            if (!L->isLoopInvariant(CB->getArgOperand(A->getArgNo()))) {
                return true;
            }
            continue;
        }
        if (auto *I = dyn_cast<Instruction>(Cur)) {
            for (Use &Op : I->operands()) {
                Worklist.push_back(Op.get());
            }
        }
    }
    return false;
}

/// Check if a call to a module-local function is safe within a parallel loop
/// by analyzing the callee's body.
static bool isCallSafeForLoop(CallBase *CB, Loop *L) {
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isDeclaration()) {
        return false;
    }
    if (Callee->getInstructionCount() > 64) {
        return false;
    }
    for (auto &BB : *Callee) {
        for (auto &I : BB) {
            if (I.isAtomic()) {
                return false;
            }
            if (auto *SubCB = dyn_cast<CallBase>(&I)) {
                if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
                    if (II->isLifetimeStartOrEnd()) {
                        continue;
                    }
                }
                if (!SubCB->doesNotAccessMemory()) {
                    return false;
                }
            }
            if (auto *SI = dyn_cast<StoreInst>(&I)) {
                if (SI->isVolatile()) {
                    return false;
                }
                if (!hasLoopVariantArgDep(SI->getPointerOperand(), CB, L)) {
                    return false;
                }
            }
            if (isa<LoadInst>(&I)) {
                return false;
            }
        }
    }
    return true;
}

/// Check that the loop body is safe to parallelize
bool checkSafety(Loop *L, Loop *OuterL, Function &F, LoopCandidate &LC,
                  const LoopAccessInfo *LAI, auto &Remark) {
    // Validate header PHIs as reductions.
    for (PHINode &PN : LC.Header->phis()) {
        if (&PN == LC.Bounds.IV)
            continue;
        RecurrenceDescriptor RD;
        if (!RecurrenceDescriptor::isReductionPHI(&PN, L, RD)) {
            Remark("❌ unsupported loop-carried dependency");
            return false;
        }
        Value *InitVal = PN.getIncomingValueForBlock(L->getLoopPreheader());
        auto *Update = cast<Instruction>(PN.getIncomingValueForBlock(L->getLoopLatch()));
        LC.Reductions.push_back({&PN, InitVal, Update, RD.getRecurrenceKind()});
    }

    // Single pass over all blocks: reduction stores, atomics, unsafe calls.
    SmallPtrSet<Value *, 4> RedVals;
    for (auto &R : LC.Reductions) {
        RedVals.insert(R.Phi);
        RedVals.insert(R.Update);
    }
    bool HasExplicitStores = false;
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            if (auto *SI = dyn_cast<StoreInst>(&I)) {
                if (RedVals.count(SI->getValueOperand())) {
                    Remark("❌ reduction value stored to memory");
                    return false;
                }
                HasExplicitStores = true;
            }
            if (I.isAtomic()) {
                Remark("❌ loop contains atomic instructions");
                return false;
            }
            if (auto *CB = dyn_cast<CallBase>(&I)) {
                if (auto *II = dyn_cast<IntrinsicInst>(&I))
                    if (II->isLifetimeStartOrEnd())
                        continue;
                if (!CB->doesNotAccessMemory() &&
                    !CB->getMemoryEffects().onlyAccessesInaccessibleMem()) {
                    if (!isCallSafeForLoop(CB, L)) {
                        Remark("❌ call with side effects");
                        return false;
                    }
                }
            }
        }
    }

    // Identify entry-block allocas used exclusively within the loop.
    for (Instruction &AI : F.getEntryBlock()) {
        auto *Alloca = dyn_cast<AllocaInst>(&AI);
        if (!Alloca || Alloca->user_empty())
            continue;
        if (llvm::all_of(Alloca->users(), [&](User *U) {
                auto *UI = dyn_cast<Instruction>(U);
                return UI && OuterL->contains(UI->getParent());
            }))
            LC.LoopLocalAllocas.insert(Alloca);
    }

    if (LAI && HasExplicitStores) {
        if (!LAI->canVectorizeMemory() || LAI->getNumRuntimePointerChecks() > 0) {
            Remark("❌ unsafe memory dependence");
            return false;
        }
        auto &DepChecker = LAI->getDepChecker();
        if (auto *Deps = DepChecker.getDependences())
            for (auto &D : *Deps)
                if (D.Type != MemoryDepChecker::Dependence::NoDep) {
                    Remark("❌ loop-carried memory dependence");
                    return false;
                }
    }

    // Reject non-reduction escaping values.
    for (PHINode &PN : LC.ExitBlock->phis())
        for (unsigned i = 0; i < PN.getNumIncomingValues(); ++i) {
            auto *I = dyn_cast<Instruction>(PN.getIncomingValue(i));
            if (I && OuterL->contains(I->getParent()) && !RedVals.count(I)) {
                Remark("❌ non-reduction value escapes loop");
                return false;
            }
        }

    return true;
}

} // end namespace llvm

#endif // LOOP_SAFETY_H
