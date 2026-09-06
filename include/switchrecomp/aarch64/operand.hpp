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

enum class ShiftKind : std::uint8_t
{
    None,
    Lsl,
    Lsr,
    Asr,
    Ror,
};

enum class ExtensionKind : std::uint8_t
{
    None,
    Uxtb,
    Uxth,
    Uxtw,
    Uxtx,
    Sxtb,
    Sxth,
    Sxtw,
    Sxtx,
};

struct MemoryOperand
{
    Register base;
    Register index;
    std::int64_t displacement = 0;
    std::uint8_t shift = 0U;
    ExtensionKind extension = ExtensionKind::None;
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
    ShiftKind shift_kind = ShiftKind::None;
    std::uint8_t shift = 0U;
    ExtensionKind extension = ExtensionKind::None;
    std::string text;
};

[[nodiscard]] std::string_view operand_kind_name(OperandKind kind) noexcept;
[[nodiscard]] std::string_view condition_code_name(ConditionCode condition) noexcept;

} // namespace switchrecomp::aarch64
