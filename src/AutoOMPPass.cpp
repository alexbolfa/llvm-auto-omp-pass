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

static bool parallelizeLoop(Loop *L, OpenMPIRBuilder &OMPBuilder, ScalarEvolution &SE,
                            LoopAccessInfoManager &LAM, Function &F, OptimizationRemarkEmitter &ORE) {
    auto Remark = [&](Loop *Target, StringRef Msg) {
        ORE.emit([&]() {
            return OptimizationRemarkAnalysis(DEBUG_TYPE, "LoopParallelize",
                                              Target->getStartLoc(), Target->getHeader())
                   << Msg;
        });
    };

    // Try this loop, then drill into single children until we find one we
    // can parallelize. Once validated, collapse walks back up for the nest.
    Loop *Candidate = L;
    LoopCandidate LC;
    bool Validated = false;
    auto MaybeRemark = [&](StringRef Msg) {
        if (!Validated)
            Remark(Candidate, Msg);
    };
    do {
        LoopCandidate CandLC;
        if (checkStructure(Candidate, SE, CandLC, MaybeRemark)) {
            const LoopAccessInfo *LAI = Candidate->isInnermost() ? &LAM.getInfo(*Candidate) : nullptr;
            if (checkSafety(Candidate, Candidate, F, CandLC, LAI, SE, MaybeRemark)) {
                LC = CandLC;
                L = Candidate;
                Validated = true;
                break;
            }
        }
        // Drill into single child if this level failed.
        if (Candidate->getSubLoops().size() != 1)
            break;
        Candidate = Candidate->getSubLoops()[0];
    } while (true);

    if (!Validated)
        return false;

    // Try to extend downward via collapse into perfectly nested children.
    Loop *InnermostL = detectCollapse(L, SE, LC);

    // Emit remark at the outermost parallelized loop's location.
    unsigned Depth = LC.OuterLevels.size() + 1;
    std::string Tag = "✅ #omp parallel for simd";
    if (Depth > 1)
        Tag += " collapse(" + std::to_string(Depth) + ")";
    if (!LC.Reductions.empty())
        Tag += " reduction";
    Tag += " in " + F.getName().str();
    Remark(L, Tag);

    return emitParallelLoop(InnermostL, L, LC, OMPBuilder, F);
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

    // Build worklist outermost-first via depth_first, skip children of
    // parallelized loops.
    SmallVector<Loop *, 8> Worklist;
    for (auto *TopLoop : LI)
        // TODO(alex): BFS?
        for (auto *L : depth_first(TopLoop))
            Worklist.push_back(L);

    bool Changed = false;
    SmallPtrSet<Loop *, 8> SkipLoops;
    for (Loop *L : Worklist) {
        if (SkipLoops.count(L))
            continue;
        if (parallelizeLoop(L, OMPBuilder, SE, LAM, F, ORE)) {
            Changed = true;
            for (auto *Sub : depth_first(L))
                if (Sub != L)
                    SkipLoops.insert(Sub);
        }
    }

    if (Changed)
        OMPBuilder.finalize();

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
