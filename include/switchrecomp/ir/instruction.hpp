#pragma once

#include "switchrecomp/ir/ids.hpp"
#include "switchrecomp/ir/opcode.hpp"
#include "switchrecomp/ir/register.hpp"
#include "switchrecomp/ir/source_location.hpp"
#include "switchrecomp/ir/type.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace switchrecomp::ir
{

struct Instruction
{
    Opcode opcode = Opcode::Nop;
    ValueId result = invalid_value;
    Type result_type = void_type();
    std::vector<ValueId> operands;
    GuestRegister reg{};
    Flag flag = Flag::N;
    ConditionCode condition = ConditionCode::Al;
    std::int64_t immediate = 0;
    std::uint64_t constant = 0U;
    std::uint8_t memory_size = 0U;
    SourceLocation source;
    bool address_offset_signed = false;
    std::uint64_t constant_high = 0U;
    std::uint8_t vector_index = 0U;
    VectorArrangement arrangement = VectorArrangement::Raw128;
    std::uint8_t lane_index = 0U;
    FpBinaryOperation fp_binary = FpBinaryOperation::Add;
    FpUnaryOperation fp_unary = FpUnaryOperation::Neg;
    FpConversion fp_conversion = FpConversion::SignedIntToFp;
    RoundingMode rounding_mode = RoundingMode::NearestEven;
    VectorOperation vector_operation = VectorOperation::And;
    VectorCompareOperation vector_compare = VectorCompareOperation::Equal;
    bool signaling = false;
    bool signed_operation = false;
    MemoryOrder memory_order = MemoryOrder::Relaxed;
    BarrierKind barrier_kind = BarrierKind::Dmb;
    BarrierOption barrier_option = BarrierOption::Sy;
    SystemRegister system_register = SystemRegister::TpidrEl0;
};

enum class TerminatorKind : std::uint8_t
{
    Branch,
    ConditionalBranch,
    DirectCall,
    IndirectBranch,
    IndirectCall,
    Return,
    Trap,
};

struct Terminator
{
    TerminatorKind kind = TerminatorKind::Return;
    ValueId condition = invalid_value;
    BlockId target = invalid_block;
    BlockId false_target = invalid_block;
    SourceLocation source;
    std::string trap_reason;
    ValueId target_value = invalid_value;
};

} // namespace switchrecomp::ir
