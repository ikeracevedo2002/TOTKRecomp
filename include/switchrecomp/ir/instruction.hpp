#pragma once

#include "switchrecomp/aarch64/instruction.hpp"
#include "switchrecomp/ir/type.hpp"
#include "switchrecomp/ir/value.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace switchrecomp::ir
{

enum class IrOpcode : std::uint8_t
{
    Constant,
    ReadRegister,
    WriteRegister,
    Add,
    Sub,
    And,
    Or,
    Xor,
    CompareEqual,
    CompareNotEqual,
    GuestLoad,
    GuestStore,
    Branch,
    ConditionalBranch,
    Return,
    Nop,
};

[[nodiscard]] std::string_view opcode_name(IrOpcode opcode) noexcept;
[[nodiscard]] constexpr bool is_terminator(IrOpcode opcode) noexcept
{
    return opcode == IrOpcode::Branch || opcode == IrOpcode::ConditionalBranch ||
           opcode == IrOpcode::Return;
}

struct IrSourceLocation
{
    aarch64::GuestAddress guest_pc = 0U;
    std::uint32_t opcode = 0U;
    std::optional<aarch64::InstructionId> instruction;
};

struct IrInstruction
{
    IrOpcode opcode = IrOpcode::Nop;
    IrType type = IrType::I64;
    ValueId result{};
    std::vector<ValueId> operands;
    std::uint64_t immediate = 0U;
    std::optional<aarch64::Register> reg;
    std::uint8_t access_size = 0U;
    bool signed_load = false;
    std::optional<BlockId> target;
    std::optional<BlockId> true_target;
    std::optional<BlockId> false_target;
    IrSourceLocation source;
};

} // namespace switchrecomp::ir
