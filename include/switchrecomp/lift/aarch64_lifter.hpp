#pragma once

#include "switchrecomp/analysis/control_flow_graph.hpp"
#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

#include <cstddef>
#include <string>

namespace switchrecomp::lift
{

struct LiftOptions
{
    std::string function_name;
};

[[nodiscard]] Result<ir::IrFunction> lift_aarch64(
    const analysis::ControlFlowGraph& graph, const LiftOptions& options = {});

} // namespace switchrecomp::lift
