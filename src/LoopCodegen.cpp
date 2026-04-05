#include "LoopCodegen.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

using namespace llvm;

// ===----------------------------------------------------------------------===//
// Helpers
// ===----------------------------------------------------------------------===//

static void remapOperands(Instruction *I, DenseMap<Value *, Value *> &VMap) {
    for (unsigned i = 0, e = I->getNumOperands(); i < e; ++i)
        if (auto It = VMap.find(I->getOperand(i)); It != VMap.end())
            I->setOperand(i, It->second);
}

/// Compute ceil((Stop - Start) / Step), returning 0 for empty ranges.
static Value *computeTripCount(IRBuilder<> &B, const LoopLevel &Lev, Type *IdxTy) {
    Value *Start = Lev.Start, *Stop = Lev.Stop, *Step = Lev.Step;
    if (Start->getType() != IdxTy)
        Start = B.CreateIntCast(Start, IdxTy, Lev.IsSigned);
    if (Stop->getType() != IdxTy)
        Stop = B.CreateIntCast(Stop, IdxTy, Lev.IsSigned);
    if (Step->getType() != IdxTy)
        Step = B.CreateIntCast(Step, IdxTy, Lev.IsSigned);

    Value *Span = B.CreateSub(Stop, Start);
    if (Lev.Inclusive)
        Span = B.CreateAdd(Span, ConstantInt::get(IdxTy, 1));
    Value *TC = B.CreateAdd(B.CreateUDiv(B.CreateSub(Span, ConstantInt::get(IdxTy, 1)), Step),
                            ConstantInt::get(IdxTy, 1));
    auto EmptyPred = Lev.Inclusive
                         ? (Lev.IsSigned ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT)
                         : (Lev.IsSigned ? ICmpInst::ICMP_SLE : ICmpInst::ICMP_ULE);
    Value *Empty = B.CreateICmp(EmptyPred, Stop, Start);
    return B.CreateSelect(Empty, ConstantInt::get(IdxTy, 0), TC);
}

/// Derive individual IVs from a collapsed iteration index K via div/rem.
static void deriveCollapsedIVs(IRBuilder<> &B, Value *K, ArrayRef<LoopLevel> Levels,
                               ArrayRef<Value *> TripCounts, DenseMap<Value *, Value *> &VMap) {
    Value *Rem = K;
    for (int i = Levels.size() - 1; i >= 0; --i) {
        Value *Idx = (i == 0) ? Rem : B.CreateURem(Rem, TripCounts[i]);
        if (i != 0)
            Rem = B.CreateUDiv(Rem, TripCounts[i]);
        auto &Lev = Levels[i];
        Value *IV = B.CreateAdd(Lev.Start, B.CreateMul(Idx, Lev.Step));
        if (IV->getType() != Lev.IV->getType())
            IV = B.CreateIntCast(IV, Lev.IV->getType(), Lev.IsSigned);
        VMap[Lev.IV] = IV;
    }
}

/// Clone non-terminator, non-PHI instructions from outer nest blocks into the
/// body, so the inner loop body can reference them (e.g. GEPs using outer IVs).
static void cloneOuterBlockValues(ArrayRef<BasicBlock *> AllNestBlocks,
                                  ArrayRef<BasicBlock *> InnerBlocks, BasicBlock *BodyBB,
                                  DenseMap<Value *, Value *> &VMap) {
    SmallPtrSet<BasicBlock *, 8> InnerSet(InnerBlocks.begin(), InnerBlocks.end());
    SmallVector<Instruction *, 16> Clones;
    for (auto *BB : AllNestBlocks) {
        if (InnerSet.count(BB))
            continue;
        for (Instruction &I : *BB) {
            if (I.isTerminator() || isa<PHINode>(&I) || VMap.count(&I))
                continue;
            auto *C = I.clone();
            C->insertInto(BodyBB, BodyBB->getTerminator()->getIterator());
            C->setName(I.getName());
            VMap[&I] = C;
            Clones.push_back(C);
        }
    }
    for (auto *C : Clones)
        remapOperands(C, VMap);
}

// ===----------------------------------------------------------------------===//
// Body cloning
// ===----------------------------------------------------------------------===//

/// Clone loop body blocks into new BBs, remap operands via VMap, and wire
/// reduction loads/stores at header entry and latch exit.
static void cloneLoopBodyBlocks(ArrayRef<BasicBlock *> SrcBlocks, BasicBlock *Header,
                                BasicBlock *Latch, BasicBlock *BodyBB, BasicBlock *ContinueBB,
                                DenseMap<Value *, Value *> &VMap, Function &F,
                                ArrayRef<ReductionInfo> Reductions) {
    DenseMap<BasicBlock *, BasicBlock *> BBMap;
    for (auto *BB : SrcBlocks)
        BBMap[BB] = BasicBlock::Create(F.getContext(), BB->getName() + ".par", &F);

    // Load current reduction values at header entry.
    if (!Reductions.empty()) {
        IRBuilder<> HdrB(BBMap[Header], BBMap[Header]->begin());
        for (auto &R : Reductions)
            VMap[R.Phi] = HdrB.CreateLoad(R.Phi->getType(), R.SharedAlloca, "red.cur");
    }

    // Clone non-terminator, non-header-PHI instructions.
    for (auto *BB : SrcBlocks)
        for (Instruction &I : *BB) {
            if (I.isTerminator() || (BB == Header && isa<PHINode>(&I)))
                continue;
            auto *C = I.clone();
            C->insertInto(BBMap[BB], BBMap[BB]->end());
            C->setName(I.getName());
            VMap[&I] = C;
        }

    // Remap operands and PHI incoming blocks.
    for (auto *BB : SrcBlocks)
        for (Instruction &I : *BBMap[BB]) {
            remapOperands(&I, VMap);
            if (auto *PN = dyn_cast<PHINode>(&I))
                for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
                    if (auto It = BBMap.find(PN->getIncomingBlock(i)); It != BBMap.end())
                        PN->setIncomingBlock(i, It->second);
        }

    // Wire terminators.
    for (auto *BB : SrcBlocks) {
        if (BB == Latch) {
            if (!Reductions.empty()) {
                IRBuilder<> LB(BBMap[Latch], BBMap[Latch]->end());
                for (auto &R : Reductions)
                    LB.CreateStore(VMap[R.Update], R.SharedAlloca);
            }
            BranchInst::Create(ContinueBB, BBMap[BB]);
        } else {
            auto *T = BB->getTerminator()->clone();
            remapOperands(T, VMap);
            for (unsigned i = 0; i < T->getNumSuccessors(); ++i) {
                auto It = BBMap.find(T->getSuccessor(i));
                T->setSuccessor(i, It != BBMap.end() ? It->second : ContinueBB);
            }
            T->insertInto(BBMap[BB], BBMap[BB]->end());
        }
    }

    BodyBB->getTerminator()->eraseFromParent();
    BranchInst::Create(BBMap[Header], BodyBB);
}

// ===----------------------------------------------------------------------===//
// Parallel loop emission
// ===----------------------------------------------------------------------===//

static void addVectorizeHint(CanonicalLoopInfo *CLI, LLVMContext &Ctx) {
    auto *Latch = CLI->getLatch();
    auto *Term = Latch->getTerminator();
    MDNode *VecEnable = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop.vectorize.enable"),
                                          ConstantAsMetadata::get(ConstantInt::getTrue(Ctx))});
    MDNode *LoopID = MDNode::get(Ctx, {nullptr, VecEnable});
    LoopID->replaceOperandWith(0, LoopID); // self-reference
    Term->setMetadata(LLVMContext::MD_loop, LoopID);
}

static Error finalizeLoop(CanonicalLoopInfo *CLI, OpenMPIRBuilder &OMPBuilder, DebugLoc DL,
                          bool HasReductions, OpenMPIRBuilder::InsertPointTy InnerAllocaIP,
                          OpenMPIRBuilder::InsertPointTy &SavedBodyIP,
                          OpenMPIRBuilder::InsertPointTy &SavedBodyAllocaIP) {
    if (!HasReductions)
        OMPBuilder.applySimd(CLI, {}, nullptr, omp::OrderKind::OMP_ORDER_concurrent, nullptr,
                             nullptr);
    else
        addVectorizeHint(CLI, OMPBuilder.M.getContext());
    auto WS = OMPBuilder.applyWorkshareLoop(DL, CLI, InnerAllocaIP, true);
    if (!WS)
        return WS.takeError();
    SavedBodyIP = *WS;
    SavedBodyAllocaIP = InnerAllocaIP;
    return Error::success();
}

bool llvm::emitParallelLoop(Loop *L, Loop *OutermostL, LoopCandidate &LC,
                            OpenMPIRBuilder &OMPBuilder, Function &F) {
    SmallVector<BasicBlock *, 8> InnerLoopBlocks(L->blocks());
    SmallVector<BasicBlock *, 16> AllNestBlocks(OutermostL->blocks());
    DebugLoc DL = L->getStartLoc();
    auto &Bnd = LC.Bounds;

    // --- Reduction setup ---
    if (!LC.Reductions.empty()) {
        IRBuilder<> EntryB(&F.getEntryBlock(), F.getEntryBlock().getFirstInsertionPt());
        IRBuilder<> PreB(LC.Preheader->getTerminator());
        for (auto &R : LC.Reductions) {
            R.SharedAlloca = EntryB.CreateAlloca(R.Phi->getType(), nullptr, "red.shared");
            PreB.CreateStore(R.InitVal, R.SharedAlloca);
        }
    }

    SmallVector<std::pair<Use *, ReductionInfo *>, 4> ExternalRedUses;
    SmallPtrSet<Value *, 4> RedValues;
    for (auto &R : LC.Reductions) {
        RedValues.insert(R.Phi);
        RedValues.insert(R.Update);
        for (Value *V : {static_cast<Value *>(R.Update), static_cast<Value *>(R.Phi)})
            for (Use &U : V->uses())
                if (!OutermostL->contains(cast<Instruction>(U.getUser())->getParent()))
                    ExternalRedUses.push_back({&U, &R});
    }

    // Remove non-reduction LCSSA PHIs in the exit block.
    for (PHINode &PN : make_early_inc_range(LC.ExitBlock->phis()))
        if (!any_of(PN.incoming_values(), [&](Value *V) { return RedValues.count(V); })) {
            PN.replaceAllUsesWith(PN.getIncomingValue(0));
            PN.eraseFromParent();
        }

    // --- Disconnect preheader ---
    LC.Preheader->getTerminator()->eraseFromParent();
    if (LC.Preheader == &F.getEntryBlock()) {
        auto *NewPH = BasicBlock::Create(F.getContext(), "par.ph", &F, LC.ExitBlock);
        BranchInst::Create(NewPH, LC.Preheader);
        LC.Preheader = NewPH;
    }

    OpenMPIRBuilder::InsertPointTy InsertPt(LC.Preheader, LC.Preheader->end());
    OpenMPIRBuilder::InsertPointTy AllocaIP(&F.getEntryBlock(),
                                            F.getEntryBlock().getFirstInsertionPt());
    OpenMPIRBuilder::InsertPointTy SavedBodyIP, SavedBodyAllocaIP;

    // --- Body generation ---
    auto BodyGenCB = [&](OpenMPIRBuilder::InsertPointTy InnerAllocaIP,
                         OpenMPIRBuilder::InsertPointTy CodeGenIP) -> Error {
        auto CloneBody = [&](OpenMPIRBuilder::InsertPointTy BodyIP, DenseMap<Value *, Value *> &VMap) {
            if (!LC.OuterLevels.empty())
                cloneOuterBlockValues(AllNestBlocks, InnerLoopBlocks, BodyIP.getBlock(), VMap);
            cloneLoopBodyBlocks(InnerLoopBlocks, LC.Header, LC.Latch, BodyIP.getBlock(),
                                BodyIP.getBlock()->getTerminator()->getSuccessor(0), VMap, F,
                                LC.Reductions);
        };

        Expected<CanonicalLoopInfo *> CLI = [&]() -> Expected<CanonicalLoopInfo *> {
            if (LC.OuterLevels.empty()) {
                // Single loop: use Start/Stop/Step overload.
                auto LoopBodyCB = [&](OpenMPIRBuilder::InsertPointTy BodyIP,
                                      Value *NewIV) -> Error {
                    DenseMap<Value *, Value *> VMap;
                    VMap[Bnd.IV] = NewIV;
                    CloneBody(BodyIP, VMap);
                    return Error::success();
                };
                return OMPBuilder.createCanonicalLoop({CodeGenIP, DL}, LoopBodyCB, Bnd.Start,
                                                      Bnd.Stop, Bnd.Step, Bnd.IsSigned,
                                                      Bnd.Inclusive);
            }

            // Collapse: linearize the iteration space.
            SmallVector<LoopLevel, 4> AllLevels(LC.OuterLevels.begin(), LC.OuterLevels.end());
            AllLevels.push_back(Bnd);

            IRBuilder<> TcB(CodeGenIP.getBlock(), CodeGenIP.getPoint());
            Type *IdxTy = AllLevels[0].IV->getType();
            SmallVector<Value *, 4> TripCounts;
            for (auto &Lev : AllLevels)
                TripCounts.push_back(computeTripCount(TcB, Lev, IdxTy));

            Value *TotalTC = TripCounts[0];
            for (unsigned i = 1; i < TripCounts.size(); ++i)
                TotalTC = TcB.CreateMul(TotalTC, TripCounts[i], "collapse.tc");

            auto LoopBodyCB = [&](OpenMPIRBuilder::InsertPointTy BodyIP, Value *K) -> Error {
                IRBuilder<> B(BodyIP.getBlock(), BodyIP.getPoint());
                DenseMap<Value *, Value *> VMap;
                deriveCollapsedIVs(B, K, AllLevels, TripCounts, VMap);
                CloneBody(BodyIP, VMap);
                return Error::success();
            };
            return OMPBuilder.createCanonicalLoop({CodeGenIP, DL}, LoopBodyCB, TotalTC);
        }();

        if (!CLI)
            return CLI.takeError();
        return finalizeLoop(*CLI, OMPBuilder, DL, !LC.Reductions.empty(), InnerAllocaIP,
                            SavedBodyIP, SavedBodyAllocaIP);
    };

    // --- Privatization ---
    SmallDenseMap<Value *, AllocaInst *> PrivatizedAllocas;
    auto PrivCB = [&](OpenMPIRBuilder::InsertPointTy InnerAllocaIP,
                      OpenMPIRBuilder::InsertPointTy CodeGenIP, Value &Original, Value &Inner,
                      Value *&ReplVal) -> OpenMPIRBuilder::InsertPointOrErrorTy {
        IRBuilder<> AllocaB(InnerAllocaIP.getBlock(), InnerAllocaIP.getPoint());
        for (auto &R : LC.Reductions) {
            if (&Original == R.SharedAlloca) {
                auto *Priv = AllocaB.CreateAlloca(R.Phi->getType(), nullptr, "red.priv");
                AllocaB.CreateStore(
                    getRecurrenceIdentity(R.Kind, R.Phi->getType(), FastMathFlags()), Priv);
                ReplVal = Priv;
                PrivatizedAllocas[R.SharedAlloca] = Priv;
                return CodeGenIP;
            }
        }
        if (LC.LoopLocalAllocas.count(&Original)) {
            ReplVal = AllocaB.CreateAlloca(cast<AllocaInst>(&Original)->getAllocatedType(), nullptr,
                                           "priv.local");
            return CodeGenIP;
        }
        ReplVal = &Inner;
        return CodeGenIP;
    };

    auto FiniCB = [](OpenMPIRBuilder::InsertPointTy) -> Error { return Error::success(); };

    auto Result = OMPBuilder.createParallel({InsertPt, DL}, AllocaIP, BodyGenCB, PrivCB, FiniCB,
                                            nullptr, nullptr, omp::OMP_PROC_BIND_default, false);
    if (!Result) {
        consumeError(Result.takeError());
        return false;
    }

    // --- Reduction combine ---
    if (!LC.Reductions.empty()) {
        SmallVector<OpenMPIRBuilder::ReductionInfo, 2> OMPReds;
        SmallVector<bool, 2> IsByRef;
        for (auto &R : LC.Reductions) {
            auto RedCB = [Kind = R.Kind](OpenMPIRBuilder::InsertPointTy IP, Value *LHS, Value *RHS,
                                         Value *&Res) -> OpenMPIRBuilder::InsertPointOrErrorTy {
                IRBuilder<> B(IP.getBlock(), IP.getPoint());
                switch (Kind) {
                case RecurKind::SMax:
                    Res = B.CreateBinaryIntrinsic(Intrinsic::smax, LHS, RHS);
                    break;
                case RecurKind::SMin:
                    Res = B.CreateBinaryIntrinsic(Intrinsic::smin, LHS, RHS);
                    break;
                case RecurKind::UMax:
                    Res = B.CreateBinaryIntrinsic(Intrinsic::umax, LHS, RHS);
                    break;
                case RecurKind::UMin:
                    Res = B.CreateBinaryIntrinsic(Intrinsic::umin, LHS, RHS);
                    break;
                case RecurKind::FMax:
                    Res = B.CreateBinaryIntrinsic(Intrinsic::maxnum, LHS, RHS);
                    break;
                case RecurKind::FMin:
                    Res = B.CreateBinaryIntrinsic(Intrinsic::minnum, LHS, RHS);
                    break;
                default:
                    Res = B.CreateBinOp(
                        static_cast<Instruction::BinaryOps>(RecurrenceDescriptor::getOpcode(Kind)),
                        LHS, RHS, "red");
                    break;
                }
                return B.saveIP();
            };
            OMPReds.push_back({R.Phi->getType(), R.SharedAlloca, PrivatizedAllocas[R.SharedAlloca],
                               OpenMPIRBuilder::EvalKind::Scalar, RedCB, nullptr, nullptr,
                               nullptr});
            IsByRef.push_back(false);
        }
        OMPBuilder.Config.setIsGPU(false);
        auto RedResult =
            OMPBuilder.createReductions({SavedBodyIP, DL}, SavedBodyAllocaIP, OMPReds, IsByRef);
        if (!RedResult) {
            consumeError(RedResult.takeError());
            return false;
        }
    }

    // --- Finalize ---
    BasicBlock *AfterBB = Result->getBlock();
    if (!ExternalRedUses.empty()) {
        IRBuilder<> B(AfterBB);
        if (AfterBB->getTerminator())
            B.SetInsertPoint(AfterBB->getTerminator());
        DenseMap<ReductionInfo *, Value *> FinalVals;
        for (auto &R : LC.Reductions)
            FinalVals[&R] = B.CreateLoad(R.Phi->getType(), R.SharedAlloca, "red.result");
        for (auto &[U, R] : ExternalRedUses)
            U->set(FinalVals[R]);
    }

    for (PHINode &PN : make_early_inc_range(LC.ExitBlock->phis()))
        if (PN.getNumIncomingValues() == 1) {
            PN.replaceAllUsesWith(PN.getIncomingValue(0));
            PN.eraseFromParent();
        }

    if (!AfterBB->getTerminator())
        BranchInst::Create(LC.ExitBlock, AfterBB);

    // Delete original loop blocks.
    for (auto *BB : AllNestBlocks)
        for (Instruction &I : *BB)
            I.replaceAllUsesWith(PoisonValue::get(I.getType()));
    for (auto *BB : AllNestBlocks)
        BB->dropAllReferences();
    for (auto *BB : AllNestBlocks)
        BB->eraseFromParent();

    return true;
}
