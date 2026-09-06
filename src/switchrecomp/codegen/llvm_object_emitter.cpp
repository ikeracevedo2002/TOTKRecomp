#include "switchrecomp/codegen/llvm_object_emitter.hpp"

#include "switchrecomp/codegen/llvm_lowerer.hpp"

#include <memory>
#include <optional>

#if defined(SWITCHRECOMP_HAS_LLVM)
#include <llvm/ADT/Triple.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#endif

namespace switchrecomp::codegen
{

#if !defined(SWITCHRECOMP_HAS_LLVM)

Result<void> emit_native_object(const ir::IrFunction& function,
                                const std::filesystem::path& output_path)
{
    (void)function;
    (void)output_path;
    return Result<void>::failure(make_error(
        ErrorCode::LLVMUnavailable,
        "LLVM object emission is not available; configure with an installed LLVM development package"));
}

#else

namespace
{

[[nodiscard]] std::string diagnostic_text(const llvm::SMDiagnostic& diagnostic)
{
    std::string text;
    llvm::raw_string_ostream output(text);
    diagnostic.print("switchrecomp", output);
    output.flush();
    return text;
}

} // namespace

Result<void> emit_native_object(const ir::IrFunction& function,
                                const std::filesystem::path& output_path)
{
    const auto lowered = lower_to_llvm(function);
    if (!lowered)
    {
        return Result<void>::failure(lowered.error());
    }
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(lowered.value().textual_ir, diagnostic, context);
    if (!module)
    {
        return Result<void>::failure(make_error(
            ErrorCode::LLVMVerificationFailed,
            "could not parse generated LLVM IR: " + diagnostic_text(diagnostic)));
    }
    if (llvm::verifyModule(*module, &llvm::errs()))
    {
        return Result<void>::failure(make_error(
            ErrorCode::LLVMVerificationFailed, "generated LLVM module failed verification"));
    }

    const auto triple = llvm::sys::getDefaultTargetTriple();
    std::string target_error;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, target_error);
    if (target == nullptr)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ObjectEmissionFailed, "could not select host target: " + target_error));
    }
    llvm::TargetOptions target_options;
    std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple, "generic", "", target_options, std::nullopt));
    if (machine == nullptr)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ObjectEmissionFailed, "could not create host target machine"));
    }
    module->setTargetTriple(triple);
    module->setDataLayout(machine->createDataLayout());
    std::error_code file_error;
    llvm::raw_fd_ostream output(output_path.string(), file_error, llvm::sys::fs::OF_None);
    if (file_error)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ObjectEmissionFailed,
            "could not open native object output: " + file_error.message()));
    }
    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, output, nullptr, llvm::CodeGenFileType::ObjectFile))
    {
        return Result<void>::failure(make_error(
            ErrorCode::ObjectEmissionFailed, "host target cannot emit native object files"));
    }
    passes.run(*module);
    output.flush();
    return Result<void>::success();
}

#endif

} // namespace switchrecomp::codegen
