#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/execution.hpp"

namespace switchrecomp::interpreter
{

[[nodiscard]] Result<runtime::ExecutionResult> execute(
    const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
    const runtime::ExecutionOptions& options = {});

} // namespace switchrecomp::interpreter
