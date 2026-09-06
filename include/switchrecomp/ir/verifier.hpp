#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

namespace switchrecomp::ir
{

[[nodiscard]] Result<void> verify(const Function& function);

} // namespace switchrecomp::ir
