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

    // Lowers and verifies a function, returning textual LLVM IR for diagnostics and tests.
    [[nodiscard]] Result<std::string> lower_to_llvm_ir(const ir::Function& function) const;

    // Compiles the Semantic IR with LLVM's native JIT and invokes the internal ABI.
    [[nodiscard]] Result<runtime::ExecutionResult> execute(
        const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
        const runtime::ExecutionOptions& options = {}) const;

  private:
    struct Impl;

    explicit LlvmBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace switchrecomp::codegen
