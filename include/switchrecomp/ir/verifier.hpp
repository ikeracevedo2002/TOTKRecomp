#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

namespace switchrecomp::ir
{

[[nodiscard]] Result<void> verify_function(const IrFunction& function);

} // namespace switchrecomp::ir
