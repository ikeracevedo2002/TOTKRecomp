#pragma once

#include "switchrecomp/interpreter/interpreter.hpp"

#include <functional>

namespace switchrecomp::interpreter
{

using M9ReadValue = std::function<Result<std::uint64_t>(ir::ValueId)>;
using M9StoreValue = std::function<Result<void>(std::uint64_t, InterpreterValueProvenance)>;

[[nodiscard]] bool is_m9_opcode(ir::Opcode opcode) noexcept;

[[nodiscard]] Result<void> execute_m9_instruction(
    const ir::Instruction& instruction,
    runtime::RuntimeContext& runtime, const M9ReadValue& read,
    const M9StoreValue& store);

} // namespace switchrecomp::interpreter
