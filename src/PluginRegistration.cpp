#include <llvm/Config/llvm-config.h>

#include "AutoOMPPass.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif

using namespace llvm;

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "AutoOMPPass", LLVM_VERSION_STRING, [](PassBuilder &PB) {
                // 'opt -passes=auto-omp' support
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, FunctionPassManager &FPM, ...) {
                        if (Name == "auto-omp") {
                            FPM.addPass(LoopSimplifyPass());
                            FPM.addPass(RequireAnalysisPass<LoopAnalysis, Function>());
                            FPM.addPass(RequireAnalysisPass<LoopAccessAnalysis, Function>());
                            FPM.addPass(AutoOMPPass());
                            return true;
                        }
                        return false;
                    });

                // 'clang -fpass-plugin' support — run before the vectorizer
                // so we see clean, unvectorized loops.
                PB.registerVectorizerStartEPCallback(
                    [](FunctionPassManager &FPM, OptimizationLevel Level) {
                        if (Level != OptimizationLevel::O0) {
                            FPM.addPass(LoopSimplifyPass());
                            FPM.addPass(RequireAnalysisPass<LoopAnalysis, Function>());
                            FPM.addPass(RequireAnalysisPass<LoopAccessAnalysis, Function>());
                            FPM.addPass(AutoOMPPass());
                        }
                    });
            }};
}
