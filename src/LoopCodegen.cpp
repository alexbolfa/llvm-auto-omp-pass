#include "LoopCodegen.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"

using namespace llvm;

static unsigned getReductionBinOp(Instruction *I) {
    if (auto *II = dyn_cast<IntrinsicInst>(I)) {
        if (II->getIntrinsicID() == Intrinsic::fmuladd || II->getIntrinsicID() == Intrinsic::fma) {
            return Instruction::FAdd;
        }
    }
    return I->getOpcode();
}

static void remapOperands(Instruction *I, DenseMap<Value *, Value *> &VMap) {
    for (unsigned i = 0, e = I->getNumOperands(); i < e; ++i) {
        if (auto It = VMap.find(I->getOperand(i)); It != VMap.end()) {
            I->setOperand(i, It->second);
        }
    }
}

static void cloneLoopBodyBlocks(ArrayRef<BasicBlock *> SrcBlocks, BasicBlock *Header,
                                BasicBlock *Latch, BasicBlock *BodyBB, BasicBlock *ContinueBB,
                                DenseMap<Value *, Value *> &VMap, Function &F,
                                ArrayRef<ReductionInfo> Reductions) {
    DenseMap<BasicBlock *, BasicBlock *> BBMap;
    for (auto *BB : SrcBlocks) {
        BBMap[BB] = BasicBlock::Create(F.getContext(), BB->getName() + ".par", &F);
    }

    if (!Reductions.empty()) {
        IRBuilder<> HdrB(BBMap[Header], BBMap[Header]->begin());
        for (auto &R : Reductions) {
            VMap[R.Phi] = HdrB.CreateLoad(R.Phi->getType(), R.SharedAlloca, "red.cur");
        }
    }

    for (auto *BB : SrcBlocks) {
        for (Instruction &I : *BB) {
            if (I.isTerminator() || (BB == Header && isa<PHINode>(&I))) {
                continue;
            }
            auto *C = I.clone();
            C->insertInto(BBMap[BB], BBMap[BB]->end());
            C->setName(I.getName());
            VMap[&I] = C;
        }
    }

    for (auto *BB : SrcBlocks) {
        for (Instruction &I : *BBMap[BB]) {
            remapOperands(&I, VMap);
            if (auto *PN = dyn_cast<PHINode>(&I)) {
                for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
                    if (auto It = BBMap.find(PN->getIncomingBlock(i)); It != BBMap.end()) {
                        PN->setIncomingBlock(i, It->second);
                    }
                }
            }
        }
    }

    for (auto *BB : SrcBlocks) {
        if (BB == Latch) {
            if (!Reductions.empty()) {
                IRBuilder<> LB(BBMap[Latch], BBMap[Latch]->end());
                for (auto &R : Reductions) {
                    LB.CreateStore(VMap[R.Update], R.SharedAlloca);
                }
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

// initial value of values
static Constant *getReductionIdentity(unsigned Opcode, Type *Ty) {
    switch (Opcode) {
    case Instruction::Add:
        return ConstantInt::get(Ty, 0);
    case Instruction::FAdd:
        return ConstantFP::get(Ty, 0.0);
    case Instruction::Mul:
        return ConstantInt::get(Ty, 1);
    case Instruction::FMul:
        return ConstantFP::get(Ty, 1.0);
    case Instruction::And:
        return ConstantInt::getAllOnesValue(Ty);
    case Instruction::Or:
    case Instruction::Xor:
        return ConstantInt::get(Ty, 0);
    default:
        return nullptr;
    }
}

bool llvm::emitParallelLoop(Loop *L, LoopCandidate &LC, OpenMPIRBuilder &OMPBuilder, Function &F) {
    SmallVector<BasicBlock *, 8> LoopBlocks(L->blocks());
    DebugLoc DL = L->getStartLoc();

    // Create shared reduction allocas in the entry block.
    if (!LC.Reductions.empty()) {
        IRBuilder<> EntryB(&F.getEntryBlock(), F.getEntryBlock().getFirstInsertionPt());
        IRBuilder<> PreB(LC.Preheader->getTerminator());
        for (auto &R : LC.Reductions) {
            R.SharedAlloca = EntryB.CreateAlloca(R.Phi->getType(), nullptr, "red.shared");
            PreB.CreateStore(R.InitVal, R.SharedAlloca);
        }
    }

    SmallVector<std::pair<Use *, ReductionInfo *>, 4> ExternalRedUses;
    for (auto &R : LC.Reductions) {
        for (Value *V : {static_cast<Value *>(R.Update), static_cast<Value *>(R.Phi)}) {
            for (Use &U : V->uses()) {
                if (!L->contains(cast<Instruction>(U.getUser())->getParent())) {
                    ExternalRedUses.push_back({&U, &R});
                }
            }
        }
    }

    // Remove non-reduction LCSSA PHIs in the exit block.
    SmallPtrSet<Value *, 4> RedValues;
    for (auto &R : LC.Reductions) {
        RedValues.insert(R.Phi);
        RedValues.insert(R.Update);
    }
    for (PHINode &PN : make_early_inc_range(LC.ExitBlock->phis())) {
        if (!any_of(PN.incoming_values(), [&](Value *V) { return RedValues.count(V); })) {
            PN.replaceAllUsesWith(PN.getIncomingValue(0));
            PN.eraseFromParent();
        }
    }

    // Disconnect preheader from loop header.
    LC.Preheader->getTerminator()->eraseFromParent();

    // If preheader is the entry block, split so OMPIRBuilder has separate blocks.
    if (LC.Preheader == &F.getEntryBlock()) {
        auto *NewPH = BasicBlock::Create(F.getContext(), "par.ph", &F, LC.ExitBlock);
        BranchInst::Create(NewPH, LC.Preheader);
        LC.Preheader = NewPH;
    }

    OpenMPIRBuilder::InsertPointTy InsertPt(LC.Preheader, LC.Preheader->end());
    OpenMPIRBuilder::InsertPointTy AllocaIP(&F.getEntryBlock(),
                                            F.getEntryBlock().getFirstInsertionPt());

    OpenMPIRBuilder::InsertPointTy SavedBodyIP, SavedBodyAllocaIP;

    auto BodyGenCB = [&](OpenMPIRBuilder::InsertPointTy InnerAllocaIP,
                         OpenMPIRBuilder::InsertPointTy CodeGenIP) -> Error {
        auto LoopBodyCB = [&](OpenMPIRBuilder::InsertPointTy BodyIP, Value *NewIV) -> Error {
            DenseMap<Value *, Value *> VMap;
            VMap[LC.IV] = NewIV;
            cloneLoopBodyBlocks(LoopBlocks, LC.Header, LC.Latch, BodyIP.getBlock(),
                                BodyIP.getBlock()->getTerminator()->getSuccessor(0), VMap, F,
                                LC.Reductions);
            return Error::success();
        };

        auto CLI = OMPBuilder.createCanonicalLoop({CodeGenIP, DL}, LoopBodyCB, LC.Start, LC.Stop,
                                                  LC.Step, LC.IsSigned, LC.Inclusive);
        if (!CLI) {
            return CLI.takeError();
        }

        if (LC.Reductions.empty()) {
            OMPBuilder.applySimd(*CLI, {}, nullptr, omp::OrderKind::OMP_ORDER_concurrent, nullptr,
                                 nullptr);
        }

        auto WS = OMPBuilder.applyWorkshareLoop(DL, *CLI, InnerAllocaIP, true);
        if (!WS) {
            return WS.takeError();
        }

        SavedBodyIP = *WS;
        SavedBodyAllocaIP = InnerAllocaIP;
        return Error::success();
    };

    SmallDenseMap<Value *, AllocaInst *> PrivatizedAllocas;
    auto PrivCB = [&](OpenMPIRBuilder::InsertPointTy InnerAllocaIP,
                      OpenMPIRBuilder::InsertPointTy CodeGenIP, Value &Original, Value &Inner,
                      Value *&ReplVal) -> OpenMPIRBuilder::InsertPointOrErrorTy {
        IRBuilder<> AllocaB(InnerAllocaIP.getBlock(), InnerAllocaIP.getPoint());
        for (auto &R : LC.Reductions) {
            if (&Original == R.SharedAlloca) {
                auto *Priv = AllocaB.CreateAlloca(R.Phi->getType(), nullptr, "red.priv");
                AllocaB.CreateStore(
                    getReductionIdentity(getReductionBinOp(R.Update), R.Phi->getType()), Priv);
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

    // Generate reduction combine code.
    if (!LC.Reductions.empty()) {
        SmallVector<OpenMPIRBuilder::ReductionInfo, 2> OMPReds;
        SmallVector<bool, 2> IsByRef;
        for (auto &R : LC.Reductions) {
            auto RedCB = [Opc = getReductionBinOp(R.Update)](
                             OpenMPIRBuilder::InsertPointTy IP, Value *LHS, Value *RHS,
                             Value *&Res) -> OpenMPIRBuilder::InsertPointOrErrorTy {
                IRBuilder<> B(IP.getBlock(), IP.getPoint());
                Res = B.CreateBinOp(static_cast<Instruction::BinaryOps>(Opc), LHS, RHS, "red");
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

    // Replace external uses of reduction values with final results.
    BasicBlock *AfterBB = Result->getBlock();
    if (!ExternalRedUses.empty()) {
        IRBuilder<> B(AfterBB);
        if (AfterBB->getTerminator()) {
            B.SetInsertPoint(AfterBB->getTerminator());
        }
        DenseMap<ReductionInfo *, Value *> FinalVals;
        for (auto &R : LC.Reductions) {
            FinalVals[&R] = B.CreateLoad(R.Phi->getType(), R.SharedAlloca, "red.result");
        }
        for (auto &[U, R] : ExternalRedUses) {
            U->set(FinalVals[R]);
        }
    }

    for (PHINode &PN : make_early_inc_range(LC.ExitBlock->phis())) {
        if (PN.getNumIncomingValues() == 1) {
            PN.replaceAllUsesWith(PN.getIncomingValue(0));
            PN.eraseFromParent();
        }
    }

    if (!AfterBB->getTerminator()) {
        BranchInst::Create(LC.ExitBlock, AfterBB);
    }

    // Delete original loop blocks.
    for (auto *BB : LoopBlocks) {
        for (Instruction &I : *BB) {
            I.replaceAllUsesWith(UndefValue::get(I.getType()));
        }
    }
    for (auto *BB : LoopBlocks) {
        BB->dropAllReferences();
    }
    for (auto *BB : LoopBlocks) {
        BB->eraseFromParent();
    }

    return true;
}
