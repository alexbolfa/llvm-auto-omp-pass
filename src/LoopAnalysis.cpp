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
                     ICmpInst::isSigned(Pred),
                     Pred == ICmpInst::ICMP_SLE || Pred == ICmpInst::ICMP_ULE};
}

void llvm::detectCollapse(Loop *L, ScalarEvolution &SE, LoopCandidate &LC, Loop *&OutermostL) {
    Loop *Current = L;
    SmallVector<LoopLevel, 2> TmpLevels;
    while (Loop *Parent = Current->getParentLoop()) {
        if (!LoopNest::arePerfectlyNested(*Parent, *Current, SE))
            break;
        auto Bounds = extractBounds(Parent, SE);
        if (!Bounds)
            break;
        TmpLevels.push_back(*Bounds);
        Current = Parent;
    }
    if (!TmpLevels.empty()) {
        OutermostL = Current;
        for (int i = TmpLevels.size() - 1; i >= 0; --i)
            LC.OuterLevels.push_back(TmpLevels[i]);
        LC.Preheader = OutermostL->getLoopPreheader();
        LC.ExitBlock = OutermostL->getExitBlock();
    }
}
