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
    BitCast,
    ReadVectorRegister,
    WriteVectorRegister,
    ReadFpControl,
    WriteFpControl,
    ReadFpStatus,
    WriteFpStatus,
    FpBinary,
    FpUnary,
    FpCompare,
    FpConvert,
    FpRound,
    VectorExtractLane,
    VectorInsertLane,
    VectorBroadcast,
    VectorBinary,
    VectorCompare,
    VectorShuffle,
    GuestLoadVector,
    GuestStoreVector,
};

enum class FpBinaryOperation : std::uint8_t { Add, Sub, Mul, Div, Min, Max };
enum class FpUnaryOperation : std::uint8_t { Neg, Abs, Sqrt };
enum class FpConversion : std::uint8_t {
    SignedIntToFp,
    UnsignedIntToFp,
    FpToSignedIntTowardZero,
    FpToUnsignedIntTowardZero,
    Fp32ToFp64,
    Fp64ToFp32,
};
enum class RoundingMode : std::uint8_t { NearestEven, PlusInfinity, MinusInfinity, TowardZero };
enum class VectorArrangement : std::uint8_t { Raw128, B8, B16, H4, H8, S2, S4, D1, D2 };
enum class VectorOperation : std::uint8_t {
    And, Or, Xor, Bic, Add, Sub, Mul, FpAdd, FpSub, FpMul, FpDiv
};
enum class VectorCompareOperation : std::uint8_t {
    Equal, SignedGreaterThan, SignedGreaterEqual, UnsignedHigher, UnsignedHigherEqual,
    FpEqual, FpGreaterThan, FpGreaterEqual
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
[[nodiscard]] std::string_view vector_arrangement_name(VectorArrangement arrangement) noexcept;

} // namespace switchrecomp::ir
