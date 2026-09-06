#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/execution.hpp"

namespace switchrecomp::interpreter
{

#ifndef SWITCHRECOMP_LEGACY_INTERPRETER_IMPL
[[nodiscard]] Result<runtime::ExecutionResult> execute(
    const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
    const runtime::ExecutionOptions& options = {});
#endif
[[nodiscard]] Result<runtime::ExecutionResult> execute_legacy(
    const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
    const runtime::ExecutionOptions& options = {});

} // namespace switchrecomp::interpreter
