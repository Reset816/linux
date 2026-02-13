#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <filesystem> // C++17
#include <iostream>

#define TRACING_FUNC_NAME "log_indirect_call"

namespace fs = std::filesystem; // C++17
using namespace std;
using namespace llvm;

typedef uint64_t Counter;

static bool isNoInstrumentFunction(const Function &F)
{
    StringRef Section = F.getSection();
    if (Section.empty())
    {
        return false;
    }

    return Section.contains(".noinstr.text") ||
           Section.contains(".entry.text") ||
           Section.contains(".irqentry.text") ||
           Section.contains(".head.text") ||
           Section.contains(".init.text") ||
           Section.contains(".exit.text") ||
           Section.contains(".text.unlikely");
}

bool doInstrument(const string &moduleName)
{
    // blacklist
    // has the highest priority
    static vector<string> blacklist{
        "fs/proc/myinst.c",
        "mm/kasan/generic.c"
    };
    for (auto &item : blacklist)
    {
        if (moduleName.rfind(item, 0) == 0)
        {
            return false;
        }
    }

    return true;
}

namespace
{
    struct ICallPass : public PassInfoMixin<ICallPass>
    {
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM)
        {
            // source C filename
            string moduleName = M.getName().str();
            fs::path srcNamePath(moduleName);

            // emit IR
            if (srcNamePath.extension() == ".c")
            {
                fs::path outIRPath = srcNamePath;
                outIRPath.replace_extension(".unopt.bc");
                std::error_code ec;
                llvm::raw_fd_ostream OS(outIRPath.string(), ec);
                WriteBitcodeToFile(M, OS);
            }

            if (!doInstrument(moduleName))
            {
                return PreservedAnalyses::all();
            }

            LLVMContext &ctx = M.getContext();

            // create module name global string
            IRBuilder<> builder(ctx);
            // Constant *ptr2ModuleNameStr = builder.CreateGlobalStringPtr(moduleName, "llvm_module_name", 0, &M);

            // create the tracing function (declaration only)
            FunctionType *tracingFuncTy = FunctionType::get(
                Type::getVoidTy(ctx),
                {
                    Type::getInt8PtrTy(ctx),                   // module name string
                    Type::getInt8PtrTy(ctx),                   // function name string
                    Type::getIntNTy(ctx, sizeof(Counter) * 8), // call offset in function
                    Type::getInt8PtrTy(ctx)                    // indirect call target
                },
                false);
            Function *tracingFunc = Function::Create(tracingFuncTy, Function::ExternalLinkage, TRACING_FUNC_NAME, &M);
            tracingFunc->setDSOLocal(true);

            // insert calls to the tracing function
            for (Function &F : M)
            {
                if (F.isDeclaration() || isNoInstrumentFunction(F))
                {
                    continue;
                }

                // Constant *ptr2FuncNameStr = builder.CreateGlobalStringPtr(
                //     F.getName().str(), "llvm_func_name", 0, &M);
                Counter instOffset = 0;
                for (BasicBlock &BB : F)
                {
                    for (Instruction &I : BB)
                    {
                        auto *CB = dyn_cast<CallBase>(&I);
                        if (!CB || !CB->isIndirectCall())
                        {
                            instOffset++;
                            continue;
                        }

                        // insert point: right before the indirect call
                        builder.SetInsertPoint(&I);

                        // cast the target operand to i8*
                        Value *castTarget = builder.CreateBitCast(
                            CB->getCalledOperand(), Type::getInt8PtrTy(ctx));

                        // call to the tracing function
                        // ConstantInt *constInt = ConstantInt::get(
                        //     Type::getIntNTy(ctx, sizeof(Counter) * 8), instOffset);
                        builder.CreateCall(
                            tracingFuncTy, tracingFunc,
                            // {ptr2ModuleNameStr, ptr2FuncNameStr, constInt, castTarget}
                            {castTarget});

                        instOffset++;
                    }
                }
            }
            return PreservedAnalyses::all();
        }
    };
}

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo()
{
    return {
        LLVM_PLUGIN_API_VERSION, "ICallPass", "v0.1",
        [](PassBuilder &PB)
        {
            // insert into default pipeline
            PB.registerPipelineStartEPCallback(
                [&](llvm::ModulePassManager &MPM, llvm::OptimizationLevel opt)
                {
                    MPM.addPass(ICallPass());
                });

            // need to specify the name
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>)
                {
                    if (Name == "ICallPass")
                    {
                        MPM.addPass(ICallPass());
                        return true;
                    }
                    return false;
                });
        }};
}
