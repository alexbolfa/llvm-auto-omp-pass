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
                         ScalarEvolution &SE, LoopAccessInfoManager &LAM,
                         Function &F) {
    auto Quiet = [](StringRef) {};
    Loop *Candidate = L->getParentLoop();
    while (Candidate) {
        LoopCandidate CandLC;
        if (!checkStructure(Candidate, SE, CandLC, Quiet))
            break;
        const LoopAccessInfo &LAI = LAM.getInfo(*Candidate);
        if (!checkSafety(Candidate, Candidate, F, CandLC, &LAI, Quiet))
            break;
        OutermostL = Candidate;
        L = Candidate;
        LC = CandLC;
        Candidate = Candidate->getParentLoop();
    }
}

static bool parallelizeLoop(Loop *L, OpenMPIRBuilder &OMPBuilder, ScalarEvolution &SE,
                            LoopAccessInfoManager &LAM, Function &F, OptimizationRemarkEmitter &ORE,
                            SmallPtrSetImpl<Loop *> &SkipLoops) {
    auto Remark = [&](Loop *Target, StringRef Msg) {
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
        tryPromotion(L, OutermostL, LC, SE, LAM, F);

    // Mark all subloops as consumed so the worklist skips them.
    for (auto *Sub : depth_first(OutermostL))
        if (Sub != OutermostL)
            SkipLoops.insert(Sub);

    // Emit remark at the outermost parallelized loop's location.
    unsigned Depth = LC.OuterLevels.size() + 1;
    std::string Tag = "✅ #omp parallel for simd";
    if (Depth > 1)
        Tag += " collapse(" + std::to_string(Depth) + ")";
    if (!LC.Reductions.empty())
        Tag += " reduction";
    Tag += " in " + F.getName().str();
    Remark(OutermostL, Tag);

    return emitParallelLoop(L, OutermostL, LC, OMPBuilder, F);
}

PreservedAnalyses AutoOMPPass::run(Function &F, FunctionAnalysisManager &AM) {
    if (F.hasFnAttribute("__skip_auto_omp")) {
        return PreservedAnalyses::all();
    }
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
        Changed |= parallelizeLoop(L, OMPBuilder, SE, LAM, F, ORE, SkipLoops);
    }

    if (Changed)
        OMPBuilder.finalize();

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
