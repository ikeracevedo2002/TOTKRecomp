#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <cstddef>
#include <cstdint>

namespace switchrecomp::ir
{

struct InterpreterOptions
{
    std::size_t max_steps = 1'000'000U;
};

struct ExecutionResult
{
    std::uint64_t final_pc = 0U;
    std::size_t executed_operations = 0U;
};

[[nodiscard]] Result<ExecutionResult> execute_function(
    const IrFunction& function, runtime::CpuState& state, memory::GuestMemory& memory,
    const InterpreterOptions& options = {});

} // namespace switchrecomp::ir
