#include "AutoOMPPass.h"
#include "LoopAnalysis.h"
#include "LoopCodegen.h"
#include "LoopSafety.h"

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Frontend/OpenMP/OMPIRBuilder.h"

using namespace llvm;

#define DEBUG_TYPE "auto-omp"

/// After the innermost loop is verified, try promoting to a larger outer loop.
static void tryPromotion(Loop *&L, Loop *&OutermostL, LoopCandidate &LC,
                         ScalarEvolution &SE, Function &F) {
    auto Quiet = [](StringRef) {};
    Loop *Candidate = L->getParentLoop();
    while (Candidate) {
        LoopCandidate CandLC;
        if (!checkStructure(Candidate, SE, CandLC, Quiet))
            break;
        if (!checkSafety(Candidate, Candidate, F, CandLC, nullptr, Quiet))
            break;
        OutermostL = Candidate;
        L = Candidate;
        LC = CandLC;
        Candidate = Candidate->getParentLoop();
    }
}

static bool parallelizeLoop(Loop *L, OpenMPIRBuilder &OMPBuilder, ScalarEvolution &SE,
                            LoopAccessInfoManager &LAM, Function &F,
                            OptimizationRemarkEmitter &ORE, bool SuppressRemarks,
                            SmallPtrSetImpl<Loop *> &SkipLoops) {
    auto Remark = [&](Loop *Target, StringRef Msg) {
        if (SuppressRemarks)
            return;
        ORE.emit([&]() {
            return OptimizationRemarkAnalysis(DEBUG_TYPE, "LoopParallelize",
                                              Target->getStartLoc(), Target->getHeader())
                   << Msg;
        });
    };
    auto RemarkAt = [&](StringRef Msg) { Remark(L, Msg); };

    LoopCandidate LC;
    if (!checkStructure(L, SE, LC, RemarkAt))
        return false;

    Loop *OutermostL = L;
    detectCollapse(L, SE, LC, OutermostL);

    const LoopAccessInfo &LAI = LAM.getInfo(*L);
    if (!checkSafety(L, OutermostL, F, LC, &LAI, RemarkAt))
        return false;

    // If collapse didn't fire, try promoting to a larger outer loop.
    if (LC.OuterLevels.empty())
        tryPromotion(L, OutermostL, LC, SE, F);

    // Mark all subloops as consumed so the worklist skips them.
    for (auto *Sub : depth_first(OutermostL))
        if (Sub != OutermostL)
            SkipLoops.insert(Sub);

    // Emit remark at the outermost parallelized loop's location.
    unsigned Depth = LC.OuterLevels.size() + 1;
    if (LC.Reductions.empty() && Depth == 1)
        Remark(OutermostL, "✅ #omp parallel for simd");
    else if (LC.Reductions.empty())
        Remark(OutermostL,
               ("✅ #omp parallel for simd collapse(" + Twine(Depth) + ")").str());
    else
        Remark(OutermostL, "✅ #omp parallel for simd reduction");

    return emitParallelLoop(L, OutermostL, LC, OMPBuilder, F);
}

PreservedAnalyses AutoOMPPass::run(Function &F, FunctionAnalysisManager &AM) {
    bool hadRemark = F.hasFnAttribute("__skip_auto_omp");
    F.addFnAttr("__skip_auto_omp", "true");

    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
    auto &LAM = AM.getResult<LoopAccessAnalysis>(F);

    OpenMPIRBuilder OMPBuilder(*F.getParent());
    OMPBuilder.Config.IsTargetDevice = false;
    OMPBuilder.initialize();

    SmallVector<Loop *, 8> Worklist;
    for (auto *TopLoop : LI)
        for (auto *L : depth_first(TopLoop))
            if (L->isInnermost())
                Worklist.push_back(L);

    bool Changed = false;
    SmallPtrSet<Loop *, 8> SkipLoops;

    for (Loop *L : Worklist) {
        if (SkipLoops.count(L))
            continue;
        Changed |= parallelizeLoop(L, OMPBuilder, SE, LAM, F, ORE, hadRemark, SkipLoops);
    }

    if (Changed)
        OMPBuilder.finalize();

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
