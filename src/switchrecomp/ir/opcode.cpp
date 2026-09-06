#include "switchrecomp/ir/opcode.hpp"

namespace switchrecomp::ir
{

std::string_view opcode_name(Opcode opcode) noexcept
{
    switch (opcode)
    {
    case Opcode::Constant: return "constant";
    case Opcode::Nop: return "nop";
    case Opcode::SetPc: return "set_pc";
    case Opcode::Add: return "add";
    case Opcode::Sub: return "sub";
    case Opcode::Mul: return "mul";
    case Opcode::And: return "and";
    case Opcode::Or: return "or";
    case Opcode::Xor: return "xor";
    case Opcode::Not: return "not";
    case Opcode::ShiftLeft: return "shift_left";
    case Opcode::LogicalShiftRight: return "logical_shift_right";
    case Opcode::ArithmeticShiftRight: return "arithmetic_shift_right";
    case Opcode::RotateRight: return "rotate_right";
    case Opcode::Truncate: return "truncate";
    case Opcode::ZeroExtend: return "zero_extend";
    case Opcode::SignExtend: return "sign_extend";
    case Opcode::CompareEqual: return "compare_equal";
    case Opcode::CompareNotEqual: return "compare_not_equal";
    case Opcode::CompareUnsigned: return "compare_unsigned";
    case Opcode::CompareSigned: return "compare_signed";
    case Opcode::Select: return "select";
    case Opcode::AddCarry: return "add_carry";
    case Opcode::AddOverflow: return "add_overflow";
    case Opcode::SubCarry: return "sub_carry";
    case Opcode::SubOverflow: return "sub_overflow";
    case Opcode::EvaluateCondition: return "evaluate_condition";
    case Opcode::ReadRegister: return "read_register";
    case Opcode::WriteRegister: return "write_register";
    case Opcode::ReadFlag: return "read_flag";
    case Opcode::WriteFlag: return "write_flag";
    case Opcode::GuestAddressAdd: return "guest_address_add";
    case Opcode::GuestAddressAddValue: return "guest_address_add_value";
    case Opcode::GuestLoad: return "guest_load";
    case Opcode::GuestStore: return "guest_store";
    }
    return "unknown";
}

std::string_view flag_name(Flag flag) noexcept
{
    switch (flag)
    {
    case Flag::N: return "N";
    case Flag::Z: return "Z";
    case Flag::C: return "C";
    case Flag::V: return "V";
    }
    return "?";
}

std::string_view condition_code_name(ConditionCode condition) noexcept
{
    switch (condition)
    {
    case ConditionCode::Eq: return "eq";
    case ConditionCode::Ne: return "ne";
    case ConditionCode::Cs: return "cs";
    case ConditionCode::Cc: return "cc";
    case ConditionCode::Mi: return "mi";
    case ConditionCode::Pl: return "pl";
    case ConditionCode::Vs: return "vs";
    case ConditionCode::Vc: return "vc";
    case ConditionCode::Hi: return "hi";
    case ConditionCode::Ls: return "ls";
    case ConditionCode::Ge: return "ge";
    case ConditionCode::Lt: return "lt";
    case ConditionCode::Gt: return "gt";
    case ConditionCode::Le: return "le";
    case ConditionCode::Al: return "al";
    case ConditionCode::Nv: return "nv";
    }
    return "unknown";
}

} // namespace switchrecomp::ir
