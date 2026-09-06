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
            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::ShiftLeft:
            case Opcode::LogicalShiftRight:
            case Opcode::ArithmeticShiftRight:
            {
                const auto pair = require_same_integer_pair();
                if (!pair)
                {
                    checked = invalid(pair.error().message);
                }
                else
                {
                    checked = require_result(pair.value());
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
            case Opcode::GuestLoad:
                checked = require_arity(instruction, 1U);
                if (checked)
                {
                    const auto type = operand_type(function, instruction.operands[0], "guest load");
                    const bool size_ok = instruction.memory_size == 4U || instruction.memory_size == 8U;
                    if (!type || type.value() != i64_type() || !size_ok ||
                        instruction.result_type != (instruction.memory_size == 4U ? i32_type() : i64_type()))
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
                    const bool size_ok = instruction.memory_size == 4U || instruction.memory_size == 8U;
                    if (!address || !value || address.value() != i64_type() || !size_ok ||
                        value.value() != (instruction.memory_size == 4U ? i32_type() : i64_type()))
                    {
                        checked = invalid("guest_store has invalid address, size, or value type");
                    }
                    else
                    {
                        checked = require_void();
                    }
                }
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
                terminator.condition != invalid_value || !terminator.trap_reason.empty())
            {
                return invalid("branch terminator has an invalid target or carries extra data");
            }
            break;
        case TerminatorKind::ConditionalBranch:
        {
            const auto type = function.value(terminator.condition);
            if (type == nullptr || type->type != i1_type() || !valid_target(terminator.target) ||
                !valid_target(terminator.false_target) || !terminator.trap_reason.empty())
            {
                return invalid("conditional branch has an invalid condition or target");
            }
            break;
        }
        case TerminatorKind::Return:
            if (terminator.target != invalid_block || terminator.false_target != invalid_block ||
                terminator.condition != invalid_value)
            {
                return invalid("return terminator carries branch data");
            }
            break;
        case TerminatorKind::Trap:
            if (terminator.target != invalid_block || terminator.false_target != invalid_block ||
                terminator.condition != invalid_value || terminator.trap_reason.empty())
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
