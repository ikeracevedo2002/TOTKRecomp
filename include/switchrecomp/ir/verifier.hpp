#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

namespace switchrecomp::ir
{

#ifndef SWITCHRECOMP_LEGACY_VERIFIER_IMPL
[[nodiscard]] Result<void> verify(const Function& function);
#endif
[[nodiscard]] Result<void> verify_legacy(const Function& function);

} // namespace switchrecomp::ir
