#pragma once

#include <cstdint>
#include <string_view>

namespace switchrecomp::ir
{

enum class Opcode : std::uint8_t
{
    Constant,
    Nop,
    SetPc,
    Add,
    Sub,
    Mul,
    And,
    Or,
    Xor,
    Not,
    ShiftLeft,
    LogicalShiftRight,
    ArithmeticShiftRight,
    RotateRight,
    Truncate,
    ZeroExtend,
    SignExtend,
    CompareEqual,
    CompareNotEqual,
    CompareUnsigned,
    CompareSigned,
    Select,
    AddCarry,
    AddOverflow,
    SubCarry,
    SubOverflow,
    EvaluateCondition,
    ReadRegister,
    WriteRegister,
    ReadFlag,
    WriteFlag,
    GuestAddressAdd,
    GuestAddressAddValue,
    GuestLoad,
    GuestStore,
};

enum class Flag : std::uint8_t
{
    N,
    Z,
    C,
    V,
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

[[nodiscard]] std::string_view opcode_name(Opcode opcode) noexcept;
[[nodiscard]] std::string_view flag_name(Flag flag) noexcept;
[[nodiscard]] std::string_view condition_code_name(ConditionCode condition) noexcept;

} // namespace switchrecomp::ir
