#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/interpreter.hpp"
#include "switchrecomp/ir/function.hpp"
#include "switchrecomp/runtime/abi.hpp"

namespace switchrecomp::codegen
{

[[nodiscard]] Result<ir::ExecutionResult> execute_with_llvm_jit(
    const ir::IrFunction& function, runtime::RuntimeExecutionContext& context);

} // namespace switchrecomp::codegen
