#pragma once

#include "switchrecomp/analysis/control_flow_graph.hpp"
#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"
#include "switchrecomp/aarch64/instruction.hpp"

#include <cstddef>

namespace switchrecomp::lifter
{

struct LiftOptions
{
    bool verify_result = true;
    bool preserve_source_mapping = true;
    std::size_t max_basic_blocks = 16'384U;
    std::size_t max_ir_instructions = 1'000'000U;
    std::size_t max_values = 1'000'000U;
    std::size_t max_ir_operations_per_guest_instruction = 64U;
};

#ifndef SWITCHRECOMP_LEGACY_LIFTER_IMPL
[[nodiscard]] Result<ir::Function> lift_function(
    const analysis::ControlFlowGraph& cfg, const LiftOptions& options = {});

[[nodiscard]] bool is_instruction_liftable(aarch64::InstructionId id) noexcept;
[[nodiscard]] bool is_instruction_liftable(const aarch64::DecodedInstruction& instruction) noexcept;
#endif

// Internal Milestone 0-8 implementation. Milestone 9 delegates non-concurrency instructions here.
[[nodiscard]] Result<ir::Function> lift_function_legacy(
    const analysis::ControlFlowGraph& cfg, const LiftOptions& options = {});
[[nodiscard]] bool is_instruction_liftable_legacy(aarch64::InstructionId id) noexcept;
[[nodiscard]] bool is_instruction_liftable_legacy(const aarch64::DecodedInstruction& instruction) noexcept;

} // namespace switchrecomp::lifter
