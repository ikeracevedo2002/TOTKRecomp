#pragma once

#include "switchrecomp/aarch64/register.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace switchrecomp::aarch64
{

enum class OperandKind : std::uint8_t
{
    Register,
    Immediate,
    Memory,
    Condition,
    System,
    Other,
};

enum class ConditionCode : std::uint8_t
{
    Eq,
    Ne,
    Cs,
    Cc,
    Mi,
    Pl,
    Vs,
    Vc,
    Hi,
    Ls,
    Ge,
    Lt,
    Gt,
    Le,
    Al,
    Nv,
};

enum class MemoryAddressingMode : std::uint8_t
{
    Base,
    PreIndex,
    PostIndex,
    Literal,
    RegisterOffset,
};

struct MemoryOperand
{
    Register base;
    Register index;
    std::int64_t displacement = 0;
    std::uint8_t shift = 0U;
    MemoryAddressingMode addressing = MemoryAddressingMode::Base;
    bool writeback = false;
};

struct Operand
{
    OperandKind kind = OperandKind::Other;
    Register reg;
    std::int64_t immediate = 0;
    MemoryOperand memory;
    ConditionCode condition = ConditionCode::Al;
    std::string text;
};

[[nodiscard]] std::string_view operand_kind_name(OperandKind kind) noexcept;
[[nodiscard]] std::string_view condition_code_name(ConditionCode condition) noexcept;

} // namespace switchrecomp::aarch64
