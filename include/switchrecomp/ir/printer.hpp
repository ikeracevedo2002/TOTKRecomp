#pragma once

#include "switchrecomp/ir/function.hpp"

#include <string>

namespace switchrecomp::ir
{

[[nodiscard]] std::string print_function(const IrFunction& function);

} // namespace switchrecomp::ir
