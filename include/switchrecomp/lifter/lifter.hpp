#pragma once

#include "switchrecomp/analysis/control_flow_graph.hpp"
#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

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

[[nodiscard]] Result<ir::Function> lift_function(
    const analysis::ControlFlowGraph& cfg, const LiftOptions& options = {});

} // namespace switchrecomp::lifter
