#ifndef LOOP_CODEGEN_H
#define LOOP_CODEGEN_H

#include "LoopCandidate.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Frontend/OpenMP/OMPIRBuilder.h"

namespace llvm {

/// Transform a validated LoopCandidate into an OpenMP parallel region.
bool emitParallelLoop(Loop *L, LoopCandidate &LC, OpenMPIRBuilder &OMPBuilder, Function &F);

} // end namespace llvm

#endif // LOOP_CODEGEN_H
