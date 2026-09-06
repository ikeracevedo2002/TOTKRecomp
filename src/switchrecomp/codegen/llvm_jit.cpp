#include "switchrecomp/codegen/llvm_jit.hpp"

#include "switchrecomp/codegen/llvm_lowerer.hpp"

#include <string>
#include <cstdint>
#include <memory>
#include <utility>

#if defined(SWITCHRECOMP_HAS_LLVM)
#include <llvm/AsmParser/Parser.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#endif

namespace switchrecomp::codegen
{

#if !defined(SWITCHRECOMP_HAS_LLVM)

Result<ir::ExecutionResult> execute_with_llvm_jit(
    const ir::IrFunction& function, runtime::RuntimeExecutionContext& context)
{
    (void)function;
    (void)context;
    return Result<ir::ExecutionResult>::failure(make_error(
        ErrorCode::LLVMUnavailable,
        "LLVM JIT is not available; configure with an installed LLVM development package"));
}

#else

namespace
{

[[nodiscard]] std::string llvm_error(llvm::Error error)
{
    return llvm::toString(std::move(error));
}

[[nodiscard]] std::string diagnostic_text(const llvm::SMDiagnostic& diagnostic)
{
    std::string text;
    llvm::raw_string_ostream output(text);
    diagnostic.print("switchrecomp", output);
    output.flush();
    return text;
}

} // namespace

Result<ir::ExecutionResult> execute_with_llvm_jit(
    const ir::IrFunction& function, runtime::RuntimeExecutionContext& context)
{
    if (context.cpu_state == nullptr || context.guest_memory == nullptr)
    {
        return Result<ir::ExecutionResult>::failure(
            make_error(ErrorCode::InvalidArgument, "LLVM JIT requires CpuState and GuestMemory"));
    }
    const auto lowered = lower_to_llvm(function);
    if (!lowered)
    {
        return Result<ir::ExecutionResult>::failure(lowered.error());
    }

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto llvm_context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(lowered.value().textual_ir, diagnostic, *llvm_context);
    if (!module)
    {
        return Result<ir::ExecutionResult>::failure(make_error(
            ErrorCode::LLVMVerificationFailed,
            "could not parse generated LLVM IR: " + diagnostic_text(diagnostic)));
    }
    if (llvm::verifyModule(*module, &llvm::errs()))
    {
        return Result<ir::ExecutionResult>::failure(make_error(
            ErrorCode::LLVMVerificationFailed, "generated LLVM module failed verification"));
    }

    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit)
    {
        return Result<ir::ExecutionResult>::failure(make_error(
            ErrorCode::JitFailure, "could not create LLJIT: " + llvm_error(jit.takeError())));
    }
    auto jit_instance = std::move(*jit);

    auto& execution_session = jit_instance->getExecutionSession();
    llvm::orc::SymbolMap runtime_symbols;
    const auto exported = llvm::JITSymbolFlags(llvm::JITSymbolFlags::Exported);
    runtime_symbols[execution_session.intern("switchrecomp_read_register")] =
        llvm::orc::ExecutorSymbolDef{llvm::orc::ExecutorAddr::fromPtr(&switchrecomp_read_register),
                                     exported};
    runtime_symbols[execution_session.intern("switchrecomp_write_register")] =
        llvm::orc::ExecutorSymbolDef{llvm::orc::ExecutorAddr::fromPtr(&switchrecomp_write_register),
                                     exported};
    runtime_symbols[execution_session.intern("switchrecomp_guest_read_u32")] =
        llvm::orc::ExecutorSymbolDef{llvm::orc::ExecutorAddr::fromPtr(&switchrecomp_guest_read_u32),
                                     exported};
    runtime_symbols[execution_session.intern("switchrecomp_guest_read_u64")] =
        llvm::orc::ExecutorSymbolDef{llvm::orc::ExecutorAddr::fromPtr(&switchrecomp_guest_read_u64),
                                     exported};
    runtime_symbols[execution_session.intern("switchrecomp_guest_write_u32")] =
        llvm::orc::ExecutorSymbolDef{llvm::orc::ExecutorAddr::fromPtr(&switchrecomp_guest_write_u32),
                                     exported};
    runtime_symbols[execution_session.intern("switchrecomp_guest_write_u64")] =
        llvm::orc::ExecutorSymbolDef{llvm::orc::ExecutorAddr::fromPtr(&switchrecomp_guest_write_u64),
                                     exported};
    runtime_symbols[execution_session.intern("switchrecomp_set_guest_pc")] =
        llvm::orc::ExecutorSymbolDef{llvm::orc::ExecutorAddr::fromPtr(&switchrecomp_set_guest_pc),
                                     exported};
    if (auto defined = jit_instance->getMainJITDylib().define(
            llvm::orc::absoluteSymbols(std::move(runtime_symbols))); !defined)
    {
        return Result<ir::ExecutionResult>::failure(make_error(
            ErrorCode::JitFailure, "could not register runtime symbols: " + llvm_error(defined.takeError())));
    }

    if (auto added = jit_instance->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(module), std::move(llvm_context)); !added)
    {
        return Result<ir::ExecutionResult>::failure(make_error(
            ErrorCode::JitFailure, "could not add module to LLJIT: " + llvm_error(added.takeError())));
    }
    auto symbol = jit_instance->lookup(lowered.value().function_name);
    if (!symbol)
    {
        return Result<ir::ExecutionResult>::failure(make_error(
            ErrorCode::JitFailure,
            "could not find generated function: " + llvm_error(symbol.takeError())));
    }
    const auto address = static_cast<std::uintptr_t>(symbol->getValue());
    using GeneratedFunction = int (*)(runtime::RuntimeExecutionContext*);
    const auto generated = reinterpret_cast<GeneratedFunction>(address);
    const int status = generated(&context);
    if (status != runtime::execution_success || context.failed)
    {
        if (context.failed)
        {
            return Result<ir::ExecutionResult>::failure(context.last_error);
        }
        return Result<ir::ExecutionResult>::failure(
            make_error(ErrorCode::JitFailure, "generated function returned an error status"));
    }
    return Result<ir::ExecutionResult>::success(
        ir::ExecutionResult{context.cpu_state->pc, 0U});
}

#endif

} // namespace switchrecomp::codegen
