namespace {
    class LambdaizeLoop : public llvm::PassInfoMixin<LambdaizeLoop> {
    public:
        // NOTE: REQUIRES LOOPS SIMPLIFIED AND INSTRUCTIONS NAMED
        llvm::PreservedAnalyses run(llvm::Loop &Loop, llvm::LoopAnalysisManager &, llvm::LoopStandardAnalysisResults &, llvm::LPMUpdater &)
        {
            return extractLoopIntoFunction(Loop) ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
        }

    private:
        bool extractLoopIntoFunction(llvm::Loop &Loop)
        {
            // TODO: handle nested loop
            if (!Loop.isInnermost()) {
                return false;
            }
            // TODO: handle loop with multiple exit and exiting
            if (!Loop.getExitingBlock() || !Loop.getExitBlock()) {
                return false;
            }
            auto *Term = llvm::dyn_cast<llvm::BranchInst>(Loop.getLoopPreheader()->getTerminator());
            auto *Exit = Loop.getExitBlock();
            // HACK: ArgsToLooper[0] should contain pointer to Extracted, so reserve place
            std::vector<llvm::Value *> ArgsToLooper(1);
            auto *Extracted = createExtracted(Loop, std::back_inserter(ArgsToLooper));
            Term->setSuccessor(0, Exit);
            llvm::IRBuilder Builder(Term);
            ArgsToLooper[0] = Extracted;
            Builder.CreateCall(getLooperFC(*Loop.getHeader()->getModule()), llvm::ArrayRef(ArgsToLooper));
            return true;
        }
        template <class OutputIterator>
        llvm::Function *createExtracted(llvm::Loop &Loop, OutputIterator NeededArguments)
        {
            auto &Context = Loop.getHeader()->getContext();
            std::vector<llvm::Value *> OutsideDefined;
            setOutsideDefinedVariables(Loop, std::back_inserter(OutsideDefined));
            std::copy(OutsideDefined.begin(), OutsideDefined.end(), NeededArguments);
            auto *Extracted = llvm::Function::Create(
                llvm::FunctionType::get(
                    llvm::Type::getInt1Ty(Context),
                    llvm::ArrayRef<llvm::Type *>{getVaListType(Context)->getPointerTo()},
                    false),
                llvm::GlobalValue::LinkageTypes::PrivateLinkage,
                "extracted",
                *Loop.getHeader()->getModule());
            std::map<llvm::StringRef, llvm::Value *> ArgAddrMap;
            llvm::IRBuilder Builder(llvm::BasicBlock::Create(Context, "", Extracted));
            for (auto *OD : OutsideDefined) {
                ArgAddrMap[OD->getName()] = Builder.CreateVAArg(Extracted->getArg(0), OD->getType());
            }
            std::vector<llvm::BasicBlock *> BlocksFromLoop;
            RemoveLoop(Loop, std::back_inserter(BlocksFromLoop));
            Builder.CreateBr(BlocksFromLoop.front());
            for (auto *Block : BlocksFromLoop) {
                for (auto &&Inst : *Block) {
                    for (auto &&Op : Inst.operands()) {
                        if (ArgAddrMap.count(Op->getName())) {
                            Op = ArgAddrMap[Op->getName()];
                        }
                    }
                }
                Block->insertInto(Extracted);
            }
            return Extracted;
        }
        template <class OutputIterator>
        OutputIterator setOutsideDefinedVariables(llvm::Loop &Loop, OutputIterator result)
        {
            std::set<llvm::Value *> Declared, Arguments;
            for (auto *Block : Loop.blocks()) {
                for (auto &&Inst : *Block) {
                    if (!Inst.getName().empty()) {
                        Declared.insert(&Inst);
                    }
                    for (auto *Op : Inst.operand_values()) {
                        if (!Op->getType()->isLabelTy() && !Op->getName().empty()) {
                            Arguments.insert(Op);
                        }
                    }
                }
            }
            return std::set_difference(
                Arguments.begin(), Arguments.end(),
                Declared.begin(), Declared.end(),
                result);
        }
        template <class OutputIterator>
        OutputIterator RemoveLoop(llvm::Loop &Loop, OutputIterator Dest)
        {
            auto &Context = Loop.getHeader()->getContext();
            auto *CondBr = llvm::dyn_cast<llvm::BranchInst>(Loop.getExitingBlock()->getTerminator());
            llvm::BasicBlock *EndBlock;
            if (auto *IfTrue = CondBr->getSuccessor(0), *IfFalse = CondBr->getSuccessor(1); Loop.contains(IfTrue)) {
                EndBlock = llvm::BasicBlock::Create(Context, IfFalse->getName());
                auto *Cond = CondBr->getCondition();
                EndBlock->getInstList().push_back(llvm::ReturnInst::Create(Context, Cond));
            } else /* Loop.contains(IfFalse) */ {
                EndBlock = llvm::BasicBlock::Create(Context, IfTrue->getName());
                auto *Cond = llvm::BinaryOperator::CreateNot(CondBr->getCondition());
                EndBlock->getInstList().push_back(llvm::ReturnInst::Create(Context, Cond));
            }
            for (auto *Block : Loop.getBlocks()) {
                Block->removeFromParent();
                auto *BrInst = llvm::dyn_cast<llvm::BranchInst>(Block->getTerminator());
                for (size_t i = 0; i < BrInst->getNumSuccessors(); ++i) {
                    if (auto *Successor = BrInst->getSuccessor(i);
                        Successor == Loop.getHeader() || Successor->getName() == EndBlock->getName()) {
                        BrInst->setSuccessor(i, EndBlock);
                    }
                }
                *Dest++ = Block;
            }
            *Dest++ = EndBlock;
            return Dest;
        }
        llvm::FunctionCallee getLooperFC(llvm::Module &Module)
        {
            return Module.getOrInsertFunction(
                "looper",
                llvm::FunctionType::get(llvm::Type::getVoidTy(Module.getContext()), true));
        }
        llvm::StructType *getVaListType(llvm::LLVMContext &Context)
        {
            const std::string Name = "struct.va_list";
            if (auto *Type = llvm::StructType::getTypeByName(Context, Name)) {
                return Type;
            }
            auto *i32 = llvm::IntegerType::getInt32Ty(Context);
            auto *i8p = llvm::IntegerType::getInt8PtrTy(Context);
            return llvm::StructType::create(Name, i32, i32, i8p, i8p);
        }
    };
}

extern "C" LLVM_ATTRIBUTE_WEAK llvm::PassPluginLibraryInfo llvmGetPassPluginInfo()
{
    return {
        LLVM_PLUGIN_API_VERSION,
        "Lambdaize loop (under development)",
        LLVM_VERSION_STRING,
        [](llvm::PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](llvm::StringRef Name, llvm::FunctionPassManager &FPM, llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                    if (Name == "lambdaize-loop") {
                        FPM.addPass(llvm::LoopSimplifyPass());
                        FPM.addPass(llvm::InstructionNamerPass());
                        FPM.addPass(llvm::createFunctionToLoopPassAdaptor(LambdaizeLoop()));
                        return true;
                    }
                    return false;
                });
        }};
}
