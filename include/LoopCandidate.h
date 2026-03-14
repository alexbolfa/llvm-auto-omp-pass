#ifndef LOOP_CANDIDATE_H
#define LOOP_CANDIDATE_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"

namespace llvm {

struct ReductionInfo {
    PHINode *Phi;
    Value *InitVal;
    Instruction *Update;
    AllocaInst *SharedAlloca = nullptr;
};

struct LoopCandidate {
    BasicBlock *Preheader;
    BasicBlock *Header;
    BasicBlock *Latch;
    BasicBlock *ExitBlock;
    BasicBlock *ExitingBlock;
    PHINode *IV;
    Value *Start;
    Value *Stop;
    Value *Step;
    bool IsSigned;
    bool Inclusive;
    SmallVector<ReductionInfo, 2> Reductions;
    SmallPtrSet<Value *, 4> LoopLocalAllocas;
};

} // end namespace llvm

#endif // LOOP_CANDIDATE_H
