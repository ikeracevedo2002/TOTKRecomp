#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

#include <filesystem>

namespace switchrecomp::codegen
{

[[nodiscard]] Result<void> emit_native_object(const ir::IrFunction& function,
                                              const std::filesystem::path& output_path);

} // namespace switchrecomp::codegen
