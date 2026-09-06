#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/execution.hpp"

#include <memory>
#include <string>

namespace switchrecomp::codegen
{

class LlvmBackend
{
  public:
    LlvmBackend(const LlvmBackend&) = delete;
    LlvmBackend& operator=(const LlvmBackend&) = delete;
    LlvmBackend(LlvmBackend&&) noexcept;
    LlvmBackend& operator=(LlvmBackend&&) noexcept;
    ~LlvmBackend();

    [[nodiscard]] static Result<std::unique_ptr<LlvmBackend>> create();

#ifndef SWITCHRECOMP_LEGACY_LLVM_IMPL
    [[nodiscard]] Result<std::string> lower_to_llvm_ir(const ir::Function& function) const;
    [[nodiscard]] Result<runtime::ExecutionResult> execute(
        const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
        const runtime::ExecutionOptions& options = {}) const;
#endif

    // Internal Milestone 0-8 backend retained for functions that do not contain concurrency IR.
    [[nodiscard]] Result<std::string> lower_to_llvm_ir_legacy(const ir::Function& function) const;
    [[nodiscard]] Result<runtime::ExecutionResult> execute_legacy(
        const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
        const runtime::ExecutionOptions& options = {}) const;

  private:
    struct Impl;

    explicit LlvmBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace switchrecomp::codegen
