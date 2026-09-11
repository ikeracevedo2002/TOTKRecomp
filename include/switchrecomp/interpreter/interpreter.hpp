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
    // The next instruction in current_block. This cursor is advanced only
    // after the instruction has completed, so a slice can never replay it.
    std::size_t ir_operation_index = 0U;
    // Functions are immutable for the lifetime of a frame. Avoid repeating
    // the full verifier on every internal slice while retaining verification
    // before the first operation of each function generation.
    bool function_verified = false;
    ir::BlockId counted_block = ir::invalid_block;
    std::size_t block_entry_serial = 0U;
    ir::BlockId completed_block = ir::invalid_block;
    bool block_terminated = false;
    std::vector<std::uint64_t> values;
    std::vector<std::uint64_t> high_values;
    std::vector<InterpreterValueProvenance> provenance;
    std::array<InterpreterValueProvenance, 31> register_provenance{};
    std::optional<runtime::ObservedInstructionExecution> pending_observation;
    std::vector<std::uint64_t> completed_observation_pcs;

    void reset(const ir::Function& function_value);

    // Select a continuation block after a real guest control-flow boundary.
    // The session uses this instead of carrying a stale operation cursor into
    // the caller's continuation.
    void resume_at(ir::BlockId block) noexcept;
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
