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
    case Opcode::AtomicLoad: return "atomic_load";
    case Opcode::AtomicStore: return "atomic_store";
    case Opcode::ExclusiveLoad: return "exclusive_load";
    case Opcode::ExclusiveStore: return "exclusive_store";
    case Opcode::ClearExclusive: return "clear_exclusive";
    case Opcode::MemoryBarrier: return "memory_barrier";
    case Opcode::ReadSystemRegister: return "read_system_register";
    case Opcode::WriteSystemRegister: return "write_system_register";
    case Opcode::BitCast: return "bitcast";
    case Opcode::ReadVectorRegister: return "read_vector_register";
    case Opcode::WriteVectorRegister: return "write_vector_register";
    case Opcode::ReadFpControl: return "read_fpcr";
    case Opcode::WriteFpControl: return "write_fpcr";
    case Opcode::ReadFpStatus: return "read_fpsr";
    case Opcode::WriteFpStatus: return "write_fpsr";
    case Opcode::FpBinary: return "fp_binary";
    case Opcode::FpUnary: return "fp_unary";
    case Opcode::FpCompare: return "fp_compare";
    case Opcode::FpConvert: return "fp_convert";
    case Opcode::FpRound: return "fp_round";
    case Opcode::VectorExtractLane: return "vector_extract_lane";
    case Opcode::VectorInsertLane: return "vector_insert_lane";
    case Opcode::VectorBroadcast: return "vector_broadcast";
    case Opcode::VectorBinary: return "vector_binary";
    case Opcode::VectorCompare: return "vector_compare";
    case Opcode::VectorShuffle: return "vector_shuffle";
    case Opcode::GuestLoadVector: return "guest_load_vector";
    case Opcode::GuestStoreVector: return "guest_store_vector";
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

std::string_view vector_arrangement_name(VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case VectorArrangement::Raw128: return "raw128";
    case VectorArrangement::B8: return "8b";
    case VectorArrangement::B16: return "16b";
    case VectorArrangement::H4: return "4h";
    case VectorArrangement::H8: return "8h";
    case VectorArrangement::S2: return "2s";
    case VectorArrangement::S4: return "4s";
    case VectorArrangement::D1: return "1d";
    case VectorArrangement::D2: return "2d";
    }
    return "invalid";
}

std::string_view memory_order_name(MemoryOrder order) noexcept
{
    switch (order)
    {
    case MemoryOrder::Relaxed: return "relaxed";
    case MemoryOrder::Acquire: return "acquire";
    case MemoryOrder::Release: return "release";
    case MemoryOrder::AcquireRelease: return "acq_rel";
    case MemoryOrder::SequentiallyConsistent: return "seq_cst";
    }
    return "invalid";
}

std::string_view barrier_kind_name(BarrierKind kind) noexcept
{
    switch (kind)
    {
    case BarrierKind::Dmb: return "dmb";
    case BarrierKind::Dsb: return "dsb";
    case BarrierKind::Isb: return "isb";
    }
    return "invalid";
}

std::string_view barrier_option_name(BarrierOption option) noexcept
{
    switch (option)
    {
    case BarrierOption::Sy: return "sy";
    case BarrierOption::St: return "st";
    case BarrierOption::Ld: return "ld";
    case BarrierOption::Ish: return "ish";
    case BarrierOption::Ishst: return "ishst";
    case BarrierOption::Ishld: return "ishld";
    case BarrierOption::Nsh: return "nsh";
    case BarrierOption::Nshst: return "nshst";
    case BarrierOption::Nshld: return "nshld";
    case BarrierOption::Osh: return "osh";
    case BarrierOption::Oshst: return "oshst";
    case BarrierOption::Oshld: return "oshld";
    }
    return "invalid";
}

std::string_view system_register_name(SystemRegister reg) noexcept
{
    switch (reg)
    {
    case SystemRegister::TpidrEl0: return "tpidr_el0";
    case SystemRegister::TpidrroEl0: return "tpidrro_el0";
    }
    return "invalid";
}

} // namespace switchrecomp::ir
