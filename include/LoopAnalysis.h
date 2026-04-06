#ifndef LOOP_ANALYSIS_H
#define LOOP_ANALYSIS_H

#include "LoopCandidate.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include <optional>

namespace llvm {

/// Extract canonical bounds (IV, start, stop, step) from a loop.
std::optional<LoopLevel> extractBounds(Loop *L, ScalarEvolution &SE);

/// Verify canonical structure and extract bounds into LC.
bool checkStructure(Loop *L, ScalarEvolution &SE, LoopCandidate &LC, auto &Remark) {
    unsigned TC = SE.getSmallConstantTripCount(L);
    if (TC != 0 && TC <= 4096) {
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

    auto Bounds = extractBounds(L, SE);
    if (!Bounds) {
        Remark("❌ cannot determine loop bounds");
        return false;
    }
    LC.Bounds = *Bounds;

    // Reject memory-bound loops: count arithmetic vs memory ops in the body.
    unsigned MemOps = 0, ArithOps = 0;
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
                ++MemOps;
            } else if (I.isBinaryOp() || isa<CmpInst>(I)) {
                ++ArithOps;
            }
        }
    }

    if (MemOps > 0 && ArithOps <= MemOps && L->isInnermost()) {
        Remark("❌ loop is memory-bound");
        return false;
    }

    return true;
}

/// Walk down from L through perfectly nested children for collapse.
/// Returns the innermost loop of the collapsed nest.
Loop *detectCollapse(Loop *L, ScalarEvolution &SE, LoopCandidate &LC);

} // end namespace llvm

#endif // LOOP_ANALYSIS_H
