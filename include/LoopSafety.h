#ifndef LOOP_SAFETY_H
#define LOOP_SAFETY_H

#include "LoopCandidate.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"

namespace llvm {

/// Check that the loop body is safe to parallelize: valid reductions,
/// no atomics, no memory-accessing calls, no unsafe memory dependencies,
/// and no non-reduction values escaping the loop.
/// LAI may be null during outer-loop promotion (inner memory already verified).
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
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            if (auto *SI = dyn_cast<StoreInst>(&I))
                if (RedVals.count(SI->getValueOperand())) {
                    Remark("❌ reduction value stored to memory");
                    return false;
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
                    Remark("❌ call with side effects");
                    return false;
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

    // Memory dependence check.
    if (LAI && (!LAI->canVectorizeMemory() || LAI->getNumRuntimePointerChecks() > 0)) {
        Remark("❌ unsafe memory dependence");
        return false;
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
