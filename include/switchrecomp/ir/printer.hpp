#pragma once

#include "switchrecomp/ir/function.hpp"

#include <string>

namespace switchrecomp::ir
{

[[nodiscard]] std::string print(const Function& function);

} // namespace switchrecomp::ir
