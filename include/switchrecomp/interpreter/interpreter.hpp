#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/execution.hpp"

#include <array>
#include <vector>

namespace switchrecomp::interpreter
{

struct InterpreterValueProvenance
{
    enum class Kind
    {
        Unknown,
        Constant,
        GuestLoad,
    };

    Kind kind = Kind::Unknown;
    std::uint64_t address = 0U;
    std::int64_t adjustment = 0;
};

struct InterpreterFrame
{
    const ir::Function* function = nullptr;
    ir::BlockId current_block = ir::invalid_block;
    std::vector<std::uint64_t> values;
    std::vector<std::uint64_t> high_values;
    std::vector<InterpreterValueProvenance> provenance;
    std::array<InterpreterValueProvenance, 31> register_provenance{};

    void reset(const ir::Function& function_value);
};

[[nodiscard]] Result<runtime::ExecutionResult> execute_until_boundary(
    const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
    InterpreterFrame& frame, const runtime::ExecutionOptions& options = {});

#ifndef SWITCHRECOMP_LEGACY_INTERPRETER_IMPL
[[nodiscard]] Result<runtime::ExecutionResult> execute(
    const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
    const runtime::ExecutionOptions& options = {});
#endif
[[nodiscard]] Result<runtime::ExecutionResult> execute_legacy(
    const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
    const runtime::ExecutionOptions& options = {});

} // namespace switchrecomp::interpreter
