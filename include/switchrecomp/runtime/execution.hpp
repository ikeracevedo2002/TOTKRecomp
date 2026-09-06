#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <cstddef>

namespace switchrecomp::runtime
{

enum class ExecutionStatus
{
    Returned,
    Trapped,
    Fault,
    LimitExceeded,
};

struct ExecutionOptions
{
    std::size_t max_ir_operations = 100'000U;
};

struct ExecutionResult
{
    ExecutionStatus status = ExecutionStatus::Returned;
    std::size_t executed_operations = 0U;
    std::size_t executed_blocks = 0U;
    std::uint64_t final_guest_pc = 0U;
};

[[nodiscard]] const char* execution_status_name(ExecutionStatus status) noexcept;

} // namespace switchrecomp::runtime
