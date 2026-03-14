#ifndef AUTO_OMP_PASS_H
#define AUTO_OMP_PASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

class AutoOMPPass : public PassInfoMixin<AutoOMPPass> {
  public:
    static char ID;

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

    static bool isRequired() { return true; }
};

} // end namespace llvm

#endif // AUTO_OMP_PASS_H
