#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

#include <string>

namespace switchrecomp::codegen
{

struct GuestNativeMapping
{
    aarch64::GuestAddress guest_entry = 0U;
    std::string native_symbol;
};

struct LlvmModule
{
    std::string module_name;
    std::string function_name;
    std::string textual_ir;
    GuestNativeMapping mapping;
};

[[nodiscard]] Result<LlvmModule> lower_to_llvm(const ir::IrFunction& function);

} // namespace switchrecomp::codegen
