#include "switchrecomp/ir/verifier.hpp"

#include <limits>
#include <set>
#include <string>

namespace switchrecomp::ir
{

namespace
{

[[nodiscard]] Result<void> invalid(std::string message)
{
    return Result<void>::failure(make_error(ErrorCode::IrVerificationFailed,
                                            "invalid semantic IR: " + std::move(message)));
}

[[nodiscard]] bool is_register_type(Type type, const GuestRegister& reg) noexcept
{
    return type == (reg.width == RegisterWidth::W32 ? i32_type() : i64_type());
}

[[nodiscard]] bool valid_integer_type(Type type) noexcept
{
    return type.is_integer();
}

[[nodiscard]] bool valid_flag(Flag flag) noexcept
{
    switch (flag)
    {
    case Flag::N:
    case Flag::Z:
    case Flag::C:
    case Flag::V:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_condition(ConditionCode condition) noexcept
{
    switch (condition)
    {
    case ConditionCode::Eq:
    case ConditionCode::Ne:
    case ConditionCode::Cs:
    case ConditionCode::Cc:
    case ConditionCode::Mi:
    case ConditionCode::Pl:
    case ConditionCode::Vs:
    case ConditionCode::Vc:
    case ConditionCode::Hi:
    case ConditionCode::Ls:
    case ConditionCode::Ge:
    case ConditionCode::Lt:
    case ConditionCode::Gt:
    case ConditionCode::Le:
    case ConditionCode::Al:
    case ConditionCode::Nv:
        return true;
    }
    return false;
}

[[nodiscard]] bool constant_fits(Type type, std::uint64_t value) noexcept
{
    if (type == f32_type() || type == f64_type() || type == v128_type())
    {
        return type == v128_type() || type.bit_width() == 32U || type.bit_width() == 64U;
    }
    if (!valid_integer_type(type))
    {
        return false;
    }
    if (type.bit_width() == 64U)
    {
        return true;
    }
    return value < (std::uint64_t{1} << type.bit_width());
}

[[nodiscard]] bool valid_rounding(RoundingMode mode) noexcept
{
    switch (mode)
    {
    case RoundingMode::NearestEven:
    case RoundingMode::PlusInfinity:
    case RoundingMode::MinusInfinity:
    case RoundingMode::TowardZero: return true;
    }
    return false;
}

[[nodiscard]] bool valid_fp_binary(FpBinaryOperation operation) noexcept
{
    switch (operation)
    {
    case FpBinaryOperation::Add:
    case FpBinaryOperation::Sub:
    case FpBinaryOperation::Mul:
    case FpBinaryOperation::Div:
    case FpBinaryOperation::Min:
    case FpBinaryOperation::Max: return true;
    }
    return false;
}

[[nodiscard]] bool valid_fp_unary(FpUnaryOperation operation) noexcept
{
    switch (operation)
    {
    case FpUnaryOperation::Neg:
    case FpUnaryOperation::Abs:
    case FpUnaryOperation::Sqrt: return true;
    }
    return false;
}

[[nodiscard]] bool valid_fp_conversion(FpConversion conversion) noexcept
{
    switch (conversion)
    {
    case FpConversion::SignedIntToFp:
    case FpConversion::UnsignedIntToFp:
    case FpConversion::FpToSignedIntTowardZero:
    case FpConversion::FpToUnsignedIntTowardZero:
    case FpConversion::Fp32ToFp64:
    case FpConversion::Fp64ToFp32: return true;
    }
    return false;
}

[[nodiscard]] bool valid_vector_operation(VectorOperation operation) noexcept
{
    switch (operation)
    {
    case VectorOperation::And:
    case VectorOperation::Or:
    case VectorOperation::Xor:
    case VectorOperation::Bic:
    case VectorOperation::Add:
    case VectorOperation::Sub:
    case VectorOperation::Mul:
    case VectorOperation::FpAdd:
    case VectorOperation::FpSub:
    case VectorOperation::FpMul:
    case VectorOperation::FpDiv: return true;
    }
    return false;
}

[[nodiscard]] bool valid_vector_compare(VectorCompareOperation operation) noexcept
{
    switch (operation)
    {
    case VectorCompareOperation::Equal:
    case VectorCompareOperation::SignedGreaterThan:
    case VectorCompareOperation::SignedGreaterEqual:
    case VectorCompareOperation::UnsignedHigher:
    case VectorCompareOperation::UnsignedHigherEqual:
    case VectorCompareOperation::FpEqual:
    case VectorCompareOperation::FpGreaterThan:
    case VectorCompareOperation::FpGreaterEqual: return true;
    }
    return false;
}

[[nodiscard]] bool valid_arrangement(VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case VectorArrangement::B8:
    case VectorArrangement::B16:
    case VectorArrangement::H4:
    case VectorArrangement::H8:
    case VectorArrangement::S2:
    case VectorArrangement::S4:
    case VectorArrangement::D1:
    case VectorArrangement::D2:
    case VectorArrangement::Raw128: return true;
    }
    return false;
}

[[nodiscard]] std::uint8_t arrangement_bits(VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case VectorArrangement::B8:
    case VectorArrangement::B16: return 8U;
    case VectorArrangement::H4:
    case VectorArrangement::H8: return 16U;
    case VectorArrangement::S2:
    case VectorArrangement::S4: return 32U;
    case VectorArrangement::D1:
    case VectorArrangement::D2: return 64U;
    case VectorArrangement::Raw128: return 0U;
    }
    return 0U;
}

[[nodiscard]] std::uint8_t arrangement_lanes(VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case VectorArrangement::B8: return 8U;
    case VectorArrangement::B16: return 16U;
    case VectorArrangement::H4: return 4U;
    case VectorArrangement::H8: return 8U;
    case VectorArrangement::S2: return 2U;
    case VectorArrangement::S4: return 4U;
    case VectorArrangement::D1: return 1U;
    case VectorArrangement::D2: return 2U;
    case VectorArrangement::Raw128: return 1U;
    }
    return 0U;
}

[[nodiscard]] Result<Type> operand_type(const Function& function, ValueId id,
                                        std::string_view context)
{
    const auto* value = function.value(id);
    if (value == nullptr)
    {
        return Result<Type>::failure(make_error(
            ErrorCode::InvalidIrOperand,
            std::string(context) + " references an invalid value id " + std::to_string(id)));
    }
    return Result<Type>::success(value->type);
}

[[nodiscard]] Result<void> require_arity(const Instruction& instruction, std::size_t expected)
{
    if (instruction.operands.size() != expected)
    {
        return invalid(std::string(opcode_name(instruction.opcode)) + " expects " +
                       std::to_string(expected) + " operands, got " +
                       std::to_string(instruction.operands.size()));
    }
    return Result<void>::success();
}

} // namespace

Result<void> verify(const Function& function)
{
    if (function.blocks().empty())
    {
        return invalid("function has no basic blocks");
    }
    if (function.entry_block() == invalid_block || function.block(function.entry_block()) == nullptr)
    {
        return invalid("function has no valid entry block");
    }

    std::set<GuestAddress> guest_starts;
    for (std::size_t index = 0U; index < function.blocks().size(); ++index)
    {
        const auto& block = function.blocks()[index];
        if (block.id != index)
        {
            return invalid("basic block id does not match its stable vector index");
        }
        if (!guest_starts.insert(block.guest_start).second)
        {
            return invalid("duplicate guest basic-block address");
        }
        if (!block.has_terminator)
        {
            return invalid("block " + std::to_string(block.id) + " has no terminator");
        }

        for (std::size_t instruction_index = 0U;
             instruction_index < block.instructions.size(); ++instruction_index)
        {
            const auto& instruction = block.instructions[instruction_index];
            if (!instruction.result_type.valid())
            {
                return invalid("instruction " + std::to_string(instruction_index) +
                               " has an invalid result type");
            }
            if (instruction.result_type.is_void() && instruction.result != invalid_value)
            {
                return invalid("void instruction defines a value");
            }
            if (!instruction.result_type.is_void())
            {
                const auto* result = function.value(instruction.result);
                const auto expected_kind = instruction.opcode == Opcode::Constant
                                               ? ValueKind::Constant
                                               : ValueKind::Instruction;
                if (result == nullptr || result->kind != expected_kind ||
                    result->type != instruction.result_type ||
                    (expected_kind == ValueKind::Instruction &&
                     (result->defining_block != block.id ||
                      result->instruction_index != instruction_index)))
                {
                    return invalid("instruction result does not match its value definition");
                }
            }

            for (const auto operand : instruction.operands)
            {
                const auto* definition = function.value(operand);
                if (definition == nullptr)
                {
                    return invalid("instruction references an invalid value id");
                }
                if (definition->kind == ValueKind::Instruction &&
                    (definition->defining_block != block.id ||
                     definition->instruction_index >= instruction_index))
                {
                    return invalid("value is used before its definition or across blocks without a phi");
                }
            }

            const auto require_result = [&](Type type) -> Result<void> {
                return instruction.result_type == type
                           ? Result<void>::success()
                           : invalid("instruction " + std::string(opcode_name(instruction.opcode)) +
                                     " has result type " + std::string(type_name(instruction.result_type)) +
                                     ", expected " + std::string(type_name(type)));
            };
            const auto require_void = [&]() -> Result<void> {
                return require_result(void_type());
            };
            const auto require_same_integer_pair = [&]() -> Result<Type> {
                if (instruction.operands.size() != 2U)
                {
                    return Result<Type>::failure(make_error(
                        ErrorCode::IrVerificationFailed, "binary instruction has wrong operand count"));
                }
                const auto left = operand_type(function, instruction.operands[0], "left operand");
                const auto right = operand_type(function, instruction.operands[1], "right operand");
                if (!left || !right || left.value() != right.value() || !valid_integer_type(left.value()))
                {
                    return Result<Type>::failure(make_error(
                        ErrorCode::IrVerificationFailed, "binary operands must have the same integer type"));
                }
                return left;
            };

            Result<void> checked = Result<void>::success();
            switch (instruction.opcode)
            {
            case Opcode::Constant:
                if (instruction.operands.size() != 0U || instruction.result_type.is_void() ||
                    !constant_fits(instruction.result_type, instruction.constant))
                {
                    checked = invalid("constant has invalid operands, type, or value");
                }
                break;
            case Opcode::Nop:
            case Opcode::SetPc:
                checked = require_void();
                if (checked && !instruction.operands.empty())
                {
                    checked = invalid(std::string(opcode_name(instruction.opcode)) +
                                      " must not have operands");
                }
                break;
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::Not:
            case Opcode::ShiftLeft:
            case Opcode::LogicalShiftRight:
            case Opcode::ArithmeticShiftRight:
            case Opcode::RotateRight:
            {
                if (instruction.opcode == Opcode::Not)
                {
                    checked = require_arity(instruction, 1U);
                    if (checked)
                    {
                        const auto operand = operand_type(function, instruction.operands[0], "not");
                        if (!operand || !valid_integer_type(operand.value()) ||
                            operand.value() != instruction.result_type)
                        {
                            checked = invalid("not requires one operand matching its result type");
                        }
                    }
                }
                else
                {
                    const auto pair = require_same_integer_pair();
                    if (!pair)
                    {
                        checked = invalid(pair.error().message);
                    }
                    else if (instruction.opcode == Opcode::RotateRight && pair.value().bit_width() == 1U)
                    {
                        checked = invalid("rotate_right requires an integer width greater than one");
                    }
                    else
                    {
                        checked = require_result(pair.value());
                    }
                }
                break;
            }
            case Opcode::CompareEqual:
            case Opcode::CompareNotEqual:
            case Opcode::CompareUnsigned:
            case Opcode::CompareSigned:
            case Opcode::AddCarry:
            case Opcode::AddOverflow:
            case Opcode::SubCarry:
            case Opcode::SubOverflow:
            {
                const auto pair = require_same_integer_pair();
                if (!pair)
                {
                    checked = invalid(pair.error().message);
                }
                else
                {
                    checked = require_result(i1_type());
                }
                break;
            }
            case Opcode::Truncate:
            case Opcode::ZeroExtend:
            case Opcode::SignExtend:
            {
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "cast");
                    if (!source || !valid_integer_type(source.value()) ||
                        !valid_integer_type(instruction.result_type) ||
                        (instruction.opcode == Opcode::Truncate
                             ? instruction.result_type.bit_width() >= source.value().bit_width()
                             : instruction.result_type.bit_width() <= source.value().bit_width()))
                    {
                        checked = invalid("invalid integer cast widths");
                    }
                }
                break;
            }
            case Opcode::Select:
                checked = require_arity(instruction, 3U);
                if (checked)
                {
                    const auto condition = operand_type(function, instruction.operands[0], "select");
                    const auto left = operand_type(function, instruction.operands[1], "select");
                    const auto right = operand_type(function, instruction.operands[2], "select");
                    if (!condition || !left || !right || condition.value() != i1_type() ||
                        left.value() != right.value() || left.value() != instruction.result_type)
                    {
                        checked = invalid("select has incompatible types");
                    }
                }
                break;
            case Opcode::EvaluateCondition:
                checked = require_arity(instruction, 4U);
                if (checked)
                {
                    if (!valid_condition(instruction.condition))
                    {
                        checked = invalid("evaluate_condition has an invalid condition code");
                    }
                    for (const auto operand : instruction.operands)
                    {
                        const auto type = operand_type(function, operand, "condition");
                        if (!type || type.value() != i1_type())
                        {
                            checked = invalid("condition flags must be i1");
                            break;
                        }
                    }
                    if (checked)
                    {
                        checked = require_result(i1_type());
                    }
                }
                break;
            case Opcode::ReadRegister:
                checked = require_arity(instruction, 0U);
                if (checked && (!instruction.reg.valid() || !is_register_type(instruction.result_type,
                                                                                instruction.reg)))
                {
                    checked = invalid("read_register has an invalid register or result width");
                }
                break;
            case Opcode::WriteRegister:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto type = operand_type(function, instruction.operands[0], "write_register");
                    if (!instruction.reg.valid() || !type || !is_register_type(type.value(), instruction.reg))
                    {
                        checked = invalid("write_register has an invalid register or operand width");
                    }
                    else
                    {
                        checked = require_void();
                    }
                }
                break;
            case Opcode::ReadFlag:
                checked = require_arity(instruction, 0U);
                if (checked && !valid_flag(instruction.flag))
                {
                    checked = invalid("read_flag has an invalid flag");
                }
                if (checked)
                {
                    checked = require_result(i1_type());
                }
                break;
            case Opcode::WriteFlag:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto type = operand_type(function, instruction.operands[0], "write_flag");
                    if (!valid_flag(instruction.flag))
                    {
                        checked = invalid("write_flag has an invalid flag");
                    }
                    else if (!type || type.value() != i1_type())
                    {
                        checked = invalid("write_flag operand must be i1");
                    }
                    else
                    {
                        checked = require_void();
                    }
                }
                break;
            case Opcode::GuestAddressAdd:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto type = operand_type(function, instruction.operands[0], "guest address");
                    if (!type || type.value() != i64_type())
                    {
                        checked = invalid("guest address base must be i64");
                    }
                    else
                    {
                        checked = require_result(i64_type());
                    }
                }
                break;
            case Opcode::GuestAddressAddValue:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto base = operand_type(function, instruction.operands[0], "guest address");
                    const auto offset = operand_type(function, instruction.operands[1], "guest address");
                    if (!base || !offset || base.value() != i64_type() || offset.value() != i64_type())
                    {
                        checked = invalid("guest address value addition requires two i64 operands");
                    }
                    else
                    {
                        checked = require_result(i64_type());
                    }
                }
                break;
            case Opcode::GuestLoad:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto type = operand_type(function, instruction.operands[0], "guest load");
                    const bool size_ok = instruction.memory_size == 1U || instruction.memory_size == 2U ||
                                         instruction.memory_size == 4U || instruction.memory_size == 8U;
                    const auto expected = instruction.memory_size == 1U
                                              ? i8_type()
                                              : instruction.memory_size == 2U
                                                    ? i16_type()
                                                    : instruction.memory_size == 4U ? i32_type()
                                                                                    : i64_type();
                    if (!type || type.value() != i64_type() || !size_ok ||
                        instruction.result_type != expected)
                    {
                        checked = invalid("guest_load has invalid address, size, or result type");
                    }
                }
                break;
            case Opcode::GuestStore:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto address = operand_type(function, instruction.operands[0], "guest store");
                    const auto value = operand_type(function, instruction.operands[1], "guest store");
                    const bool size_ok = instruction.memory_size == 1U || instruction.memory_size == 2U ||
                                         instruction.memory_size == 4U || instruction.memory_size == 8U;
                    if (!address || !value || address.value() != i64_type() || !size_ok ||
                        value.value() != (instruction.memory_size == 1U ? i8_type()
                                           : instruction.memory_size == 2U ? i16_type()
                                           : instruction.memory_size == 4U ? i32_type() : i64_type()))
                    {
                        checked = invalid("guest_store has invalid address, size, or value type");
                    }
                    else
                    {
                        checked = require_void();
                    }
                }
                break;
            case Opcode::BitCast:
            {
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "bitcast");
                    if (!source || source.value().is_void() || source.value().bit_width() !=
                                             instruction.result_type.bit_width())
                        checked = invalid("bitcast requires equal, non-void widths");
                }
                break;
            }
            case Opcode::ReadVectorRegister:
                checked = require_arity(instruction, 0U);
                if (checked && (instruction.vector_index >= 32U || instruction.result_type != v128_type()))
                    checked = invalid("read_vector_register has an invalid register or result type");
                break;
            case Opcode::WriteVectorRegister:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "write_vector_register");
                    if (instruction.vector_index >= 32U || !source || source.value() != v128_type())
                        checked = invalid("write_vector_register has an invalid register or operand");
                    else checked = require_void();
                }
                break;
            case Opcode::ReadFpControl:
            case Opcode::ReadFpStatus:
                checked = require_arity(instruction, 0U);
                if (checked) checked = require_result(i32_type());
                break;
            case Opcode::WriteFpControl:
            case Opcode::WriteFpStatus:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "fp state");
                    if (!source || source.value() != i32_type()) checked = invalid("FP state write requires i32");
                    else checked = require_void();
                }
                break;
            case Opcode::FpBinary:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto left = operand_type(function, instruction.operands[0], "fp binary");
                    const auto right = operand_type(function, instruction.operands[1], "fp binary");
                    if (!left || !right || !valid_fp_binary(instruction.fp_binary) ||
                        !left.value().is_floating() || left.value() != right.value() ||
                        instruction.result_type != left.value())
                        checked = invalid("FP binary operands must have matching f32/f64 types");
                }
                break;
            case Opcode::FpUnary:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "fp unary");
                    if (!source || !valid_fp_unary(instruction.fp_unary) || !source.value().is_floating() ||
                        instruction.result_type != source.value())
                        checked = invalid("FP unary operand must match f32/f64 result");
                }
                break;
            case Opcode::FpCompare:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto left = operand_type(function, instruction.operands[0], "fp compare");
                    const auto right = operand_type(function, instruction.operands[1], "fp compare");
                    if (!left || !right || !left.value().is_floating() || left.value() != right.value() ||
                        instruction.result_type != i32_type())
                        checked = invalid("FP compare requires matching f32/f64 operands and i32 flags");
                }
                break;
            case Opcode::FpConvert:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "fp convert");
                    const bool signed_int_to_fp = instruction.fp_conversion == FpConversion::SignedIntToFp ||
                                                  instruction.fp_conversion == FpConversion::UnsignedIntToFp;
                    const bool fp_to_int = instruction.fp_conversion == FpConversion::FpToSignedIntTowardZero ||
                                           instruction.fp_conversion == FpConversion::FpToUnsignedIntTowardZero;
                    const bool fp_width_change = instruction.fp_conversion == FpConversion::Fp32ToFp64 ||
                                                 instruction.fp_conversion == FpConversion::Fp64ToFp32;
                    const bool valid_shape = signed_int_to_fp
                                                 ? source && source.value().is_integer() && instruction.result_type.is_floating()
                                                 : fp_to_int
                                                       ? source && source.value().is_floating() && instruction.result_type.is_integer()
                                                       : fp_width_change
                                                             ? source && instruction.result_type.is_floating()
                                                             : false;
                    if (!source || !valid_fp_conversion(instruction.fp_conversion) ||
                        source.value().is_void() || instruction.result_type.is_void() || !valid_shape ||
                        (instruction.fp_conversion == FpConversion::Fp32ToFp64 &&
                         (source.value() != f32_type() || instruction.result_type != f64_type())) ||
                        (instruction.fp_conversion == FpConversion::Fp64ToFp32 &&
                         (source.value() != f64_type() || instruction.result_type != f32_type())) ||
                        !valid_rounding(instruction.rounding_mode))
                        checked = invalid("FP conversion has invalid source, result, or rounding mode");
                }
                break;
            case Opcode::FpRound:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "fp round");
                    if (!source || !source.value().is_floating() || source.value() != instruction.result_type ||
                        !valid_rounding(instruction.rounding_mode))
                        checked = invalid("FP round requires matching f32/f64 and valid rounding mode");
                }
                break;
            case Opcode::VectorExtractLane:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto source = operand_type(function, instruction.operands[0], "vector extract");
                    const auto bits = arrangement_bits(instruction.arrangement);
                    const auto lanes = arrangement_lanes(instruction.arrangement);
                    if (!source || source.value() != v128_type() || !valid_arrangement(instruction.arrangement) ||
                        bits == 0U || instruction.lane_index >= lanes || !instruction.result_type.is_integer() ||
                        instruction.result_type.bit_width() != bits)
                        checked = invalid("vector extract has invalid arrangement, lane, or result type");
                }
                break;
            case Opcode::VectorInsertLane:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto vector = operand_type(function, instruction.operands[0], "vector insert");
                    const auto lane = operand_type(function, instruction.operands[1], "vector insert");
                    const auto bits = arrangement_bits(instruction.arrangement);
                    if (!vector || !lane || vector.value() != v128_type() || !lane.value().is_integer() ||
                        !valid_arrangement(instruction.arrangement) || bits == 0U ||
                        instruction.lane_index >= arrangement_lanes(instruction.arrangement) ||
                        lane.value().bit_width() != bits || instruction.result_type != v128_type())
                        checked = invalid("vector insert has invalid arrangement, lane, or operand type");
                }
                break;
            case Opcode::VectorBroadcast:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto lane = operand_type(function, instruction.operands[0], "vector broadcast");
                    const auto bits = arrangement_bits(instruction.arrangement);
                    if (!lane || !lane.value().is_integer() || !valid_arrangement(instruction.arrangement) ||
                        bits == 0U || lane.value().bit_width() != bits || instruction.result_type != v128_type())
                        checked = invalid("vector broadcast has invalid arrangement or operand type");
                }
                break;
            case Opcode::VectorBinary:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto left = operand_type(function, instruction.operands[0], "vector binary");
                    const auto right = operand_type(function, instruction.operands[1], "vector binary");
                    if (!left || !right || !valid_vector_operation(instruction.vector_operation) ||
                        left.value() != v128_type() || right.value() != v128_type() ||
                        instruction.result_type != v128_type() || !valid_arrangement(instruction.arrangement) ||
                        arrangement_bits(instruction.arrangement) == 0U)
                        checked = invalid("vector binary requires V128 operands and a full arrangement");
                }
                break;
            case Opcode::VectorCompare:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto left = operand_type(function, instruction.operands[0], "vector compare");
                    const auto right = operand_type(function, instruction.operands[1], "vector compare");
                    if (!left || !right || !valid_vector_compare(instruction.vector_compare) ||
                        left.value() != v128_type() || right.value() != v128_type() ||
                        instruction.result_type != v128_type() || !valid_arrangement(instruction.arrangement) ||
                        arrangement_bits(instruction.arrangement) == 0U)
                        checked = invalid("vector compare requires V128 operands and a full arrangement");
                }
                break;
            case Opcode::VectorShuffle:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto left = operand_type(function, instruction.operands[0], "vector shuffle");
                    const auto right = operand_type(function, instruction.operands[1], "vector shuffle");
                    if (!left || !right || left.value() != v128_type() || right.value() != v128_type() ||
                        instruction.result_type != v128_type() || !valid_arrangement(instruction.arrangement))
                        checked = invalid("vector shuffle requires V128 operands and a valid arrangement");
                }
                break;
            case Opcode::GuestLoadVector:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto address = operand_type(function, instruction.operands[0], "vector guest load");
                    if (!address || address.value() != i64_type() || instruction.memory_size != 16U ||
                        instruction.result_type != v128_type())
                        checked = invalid("vector guest load requires i64 address and 16-byte V128 result");
                }
                break;
            case Opcode::GuestStoreVector:
                checked = require_arity(instruction, 2U);
                if (checked)
                {
                    const auto address = operand_type(function, instruction.operands[0], "vector guest store");
                    const auto value = operand_type(function, instruction.operands[1], "vector guest store");
                    if (!address || !value || address.value() != i64_type() || value.value() != v128_type() ||
                        instruction.memory_size != 16U)
                        checked = invalid("vector guest store requires i64 address and V128 value");
                    else checked = require_void();
                }
                break;
            // Milestone 9 extends the verifier in verifier_m9.cpp. Keep these opcodes
            // visible to the legacy structural verifier so a mixed function can be
            // checked in one pass without treating the extension as an unknown opcode.
            case Opcode::AtomicLoad:
            case Opcode::AtomicStore:
            case Opcode::ExclusiveLoad:
            case Opcode::ExclusiveStore:
            case Opcode::ClearExclusive:
            case Opcode::MemoryBarrier:
            case Opcode::ReadSystemRegister:
            case Opcode::WriteSystemRegister:
                checked = Result<void>::success();
                break;
            default:
                checked = invalid("instruction has an unknown opcode");
                break;
            }
            if (!checked)
            {
                return checked;
            }
        }

        const auto& terminator = block.terminator;
        const auto valid_target = [&function](BlockId target) {
            return target != invalid_block && function.block(target) != nullptr;
        };
        switch (terminator.kind)
        {
        case TerminatorKind::Branch:
            if (!valid_target(terminator.target) || terminator.false_target != invalid_block ||
                terminator.condition != invalid_value || terminator.target_value != invalid_value ||
                !terminator.trap_reason.empty())
            {
                return invalid("branch terminator has an invalid target or carries extra data");
            }
            break;
        case TerminatorKind::ConditionalBranch:
        {
            const auto type = function.value(terminator.condition);
            if (type == nullptr || type->type != i1_type() || !valid_target(terminator.target) ||
                !valid_target(terminator.false_target) || terminator.target_value != invalid_value ||
                !terminator.trap_reason.empty())
            {
                return invalid("conditional branch has an invalid condition or target");
            }
            break;
        }
        case TerminatorKind::DirectCall:
        case TerminatorKind::IndirectBranch:
        case TerminatorKind::IndirectCall:
            if (terminator.target_value == invalid_value || function.value(terminator.target_value) == nullptr ||
                function.value(terminator.target_value)->type != i64_type() ||
                terminator.target != invalid_block || terminator.false_target != invalid_block ||
                terminator.condition != invalid_value || !terminator.trap_reason.empty())
            {
                return invalid("call/indirect terminator has an invalid target value or carries extra data");
            }
            break;
        case TerminatorKind::Return:
            if (terminator.target != invalid_block || terminator.false_target != invalid_block ||
                terminator.condition != invalid_value ||
                (terminator.target_value != invalid_value &&
                 (function.value(terminator.target_value) == nullptr ||
                  function.value(terminator.target_value)->type != i64_type())))
            {
                return invalid("return terminator carries branch data");
            }
            break;
        case TerminatorKind::Trap:
            if (terminator.target != invalid_block || terminator.false_target != invalid_block ||
                terminator.condition != invalid_value || terminator.target_value != invalid_value ||
                terminator.trap_reason.empty())
            {
                return invalid("trap terminator has an invalid payload or no diagnostic reason");
            }
            break;
        default:
            return invalid("block has an unknown terminator kind");
        }
    }

    for (std::size_t index = 0U; index < function.values().size(); ++index)
    {
        const auto& value = function.values()[index];
        if (value.id != index || !value.type.valid() ||
            (value.kind == ValueKind::Constant &&
             (!constant_fits(value.type, value.constant) || value.defining_block != invalid_block)))
        {
            return invalid("value table contains an inconsistent definition");
        }
    }
    return Result<void>::success();
}

} // namespace switchrecomp::ir
