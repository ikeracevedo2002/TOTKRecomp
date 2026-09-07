#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace switchrecomp::runtime
{

enum class ExecutionStatus
{
    Returned,
    Boundary,
    Trapped,
    Fault,
    LimitExceeded,
};

enum class ExecutionBoundaryKind
{
    None,
    Return,
    DirectCall,
    IndirectCall,
    FunctionTransfer,
    IndirectBranch,
    Trap,
    BudgetExhaustion,
};

struct ExecutionBoundary
{
    ExecutionBoundaryKind kind = ExecutionBoundaryKind::None;
    std::uint64_t source_guest_pc = 0U;
    std::uint64_t target_guest_address = 0U;
    bool target_known = false;
    std::uint32_t continuation_block = UINT32_MAX;
    std::uint64_t continuation_guest_pc = 0U;
    std::uint64_t function_entry = 0U;
    bool has_provenance_address = false;
    std::uint64_t provenance_address = 0U;
    std::string target_provenance;
    std::string target_register;
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
    ExecutionBoundary boundary;
};

[[nodiscard]] const char* execution_status_name(ExecutionStatus status) noexcept;
[[nodiscard]] const char* execution_boundary_kind_name(ExecutionBoundaryKind kind) noexcept;

} // namespace switchrecomp::runtime
