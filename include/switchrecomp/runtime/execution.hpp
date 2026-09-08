#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
    UnsupportedInstruction,
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
    std::span<const std::uint64_t> observed_guest_pcs;
    std::size_t max_observed_guest_pcs = 32U;
};

struct ObservedInstructionExecution
{
    std::uint64_t guest_pc = 0U;
    CpuState pre_state{};
    CpuState post_state{};
    std::optional<std::uint64_t> next_guest_pc;
};

struct ExecutionResult
{
    ExecutionStatus status = ExecutionStatus::Returned;
    std::size_t executed_operations = 0U;
    std::size_t executed_blocks = 0U;
    std::size_t executed_guest_instructions = 0U;
    std::uint64_t final_guest_pc = 0U;
    std::vector<std::uint64_t> observed_guest_pcs;
    std::vector<ObservedInstructionExecution> observed_instruction_executions;
    // Internal execution trace used by the session to derive bounded counts;
    // it is intentionally not serialized into public reports.
    std::vector<std::uint64_t> executed_guest_pcs;
    ExecutionBoundary boundary;
};

[[nodiscard]] const char* execution_status_name(ExecutionStatus status) noexcept;
[[nodiscard]] const char* execution_boundary_kind_name(ExecutionBoundaryKind kind) noexcept;

} // namespace switchrecomp::runtime
