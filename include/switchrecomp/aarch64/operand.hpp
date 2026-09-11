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
    FloatingImmediate,
    Memory,
    Condition,
    System,
    Other,
};

enum class VectorArrangement : std::uint8_t
{
    Invalid,
    B8,
    B16,
    H4,
    H8,
    S2,
    S4,
    D1,
    D2,
    Q1,
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
    VectorArrangement arrangement = VectorArrangement::Invalid;
    std::int8_t vector_index = -1;
    double floating_immediate = 0.0;
    bool has_floating_immediate = false;
    std::string text;
};

[[nodiscard]] std::string_view operand_kind_name(OperandKind kind) noexcept;
[[nodiscard]] std::string_view condition_code_name(ConditionCode condition) noexcept;
[[nodiscard]] std::string_view vector_arrangement_name(VectorArrangement arrangement) noexcept;
[[nodiscard]] std::uint8_t vector_element_bits(VectorArrangement arrangement) noexcept;
[[nodiscard]] std::uint8_t vector_lane_count(VectorArrangement arrangement) noexcept;

} // namespace switchrecomp::aarch64
