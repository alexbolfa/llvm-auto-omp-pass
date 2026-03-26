#ifndef LOOP_CANDIDATE_H
#define LOOP_CANDIDATE_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"

namespace llvm {

struct ReductionInfo {
    PHINode *Phi;
    Value *InitVal;
    Instruction *Update;
    RecurKind Kind;
    AllocaInst *SharedAlloca = nullptr;
};

struct LoopLevel {
    PHINode *IV = nullptr;
    Value *Start = nullptr;
    Value *Stop = nullptr;
    Value *Step = nullptr;
    bool IsSigned = false;
    bool Inclusive = false;
};

struct LoopCandidate {
    BasicBlock *Preheader = nullptr;
    BasicBlock *Header = nullptr;    // innermost header
    BasicBlock *Latch = nullptr;     // innermost latch
    BasicBlock *ExitBlock = nullptr; // outermost exit when collapsing
    BasicBlock *ExitingBlock = nullptr;
    LoopLevel Bounds;                              // loop being parallelized
    SmallVector<LoopLevel, 2> OuterLevels;         // collapse: outermost first
    SmallVector<ReductionInfo, 2> Reductions;
    SmallPtrSet<Value *, 4> LoopLocalAllocas;
};

} // end namespace llvm

#endif // LOOP_CANDIDATE_H
