//===- LoopSuggestionAnalyzer.cpp - Loop Analysis and Suggestion Pass ----===//
//
// Out-of-tree LLVM Pass
// Simple loop analysis pass that prints loop information and suggestions
//
//===----------------------------------------------------------------------===//

#include "LoopSuggestionAnalyzer.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "loop-suggestions"

PreservedAnalyses LoopSuggestionAnalyzerPass::run(Function &F, FunctionAnalysisManager &AM) {
  std::string report;
  raw_string_ostream os(report);

  // TODO: logic

  if (!report.empty()) {
    AM.getResult<OptimizationRemarkEmitterAnalysis>(F).emit(
        [&]() { return OptimizationRemarkAnalysis(DEBUG_TYPE, "Analysis", &F) << os.str(); });
  }

  return PreservedAnalyses::all();
}

//===------------------------ Pass Plugin Registration ------------------------===//

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LoopSuggestionAnalyzer", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback([](StringRef Name, FunctionPassManager &FPM, ...) {
              if (Name == "loop-suggestions") {
                FPM.addPass(RequireAnalysisPass<LoopAnalysis, Function>());
                FPM.addPass(LoopSuggestionAnalyzerPass());
                return true;
              }
              return false;
            });
          }};
}
