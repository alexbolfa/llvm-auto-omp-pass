#include "LoopAnalysis.h"

#include "llvm/Analysis/LoopNestAnalysis.h"

using namespace llvm;

std::optional<LoopLevel> llvm::extractBounds(Loop *L, ScalarEvolution &SE) {
    auto MaybeBounds = L->getBounds(SE);
    if (!MaybeBounds)
        return std::nullopt;
    auto &B = *MaybeBounds;
    auto *IV = L->getInductionVariable(SE);
    auto *Step = B.getStepValue();
    if (!IV || !Step)
        return std::nullopt;
    auto Pred = B.getCanonicalPredicate();
    if (Pred == ICmpInst::BAD_ICMP_PREDICATE)
        return std::nullopt;
    return LoopLevel{IV, &B.getInitialIVValue(), &B.getFinalIVValue(), Step,
                     ICmpInst::isSigned(Pred)};
}

Loop *llvm::detectCollapse(Loop *L, ScalarEvolution &SE, LoopCandidate &LC) {
    Loop *Current = L;
    while (Current->getSubLoops().size() == 1) {
        Loop *Child = Current->getSubLoops()[0];
        if (Current->getSubLoops().size() != 1)
            break;
        bool Clean = true;
        for (auto *BB : Current->blocks()) {
            if (Child->contains(BB))
                continue;
            for (auto &I : *BB) {
                if (I.isTerminator() || isa<PHINode>(I))
                    continue;
                if (I.mayHaveSideEffects() || I.mayReadFromMemory()) {
                    Clean = false;
                    break;
                }
            }
            if (!Clean)
                break;
        }
        if (!Clean)
            break;
        auto Bounds = extractBounds(Child, SE);
        if (!Bounds)
            break;
        // Reject non-rectangular iteration spaces.
        if (!L->isLoopInvariant(Bounds->Start) ||
            !L->isLoopInvariant(Bounds->Stop) ||
            !L->isLoopInvariant(Bounds->Step))
            break;
        LC.OuterLevels.push_back(LC.Bounds);
        LC.Bounds = *Bounds;
        Current = Child;
    }
    if (!LC.OuterLevels.empty()) {
        LC.Header = Current->getHeader();
        LC.Latch = Current->getLoopLatch();
        LC.ExitingBlock = Current->getExitingBlock();
    }
    return Current;
}
