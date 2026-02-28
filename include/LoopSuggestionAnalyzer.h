#pragma once

#ifndef LOOP_SUGGESTION_ANALYZER_H
#define LOOP_SUGGESTION_ANALYZER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;
class Loop;
class LoopInfo;

class LoopSuggestionAnalyzerPass : public PassInfoMixin<LoopSuggestionAnalyzerPass> {
 public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }

 private:
};

}  // end namespace llvm

#endif  // LOOP_SUGGESTION_ANALYZER_H
