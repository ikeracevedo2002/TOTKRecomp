#include "switchrecomp/ir/verifier.hpp"

#include "switchrecomp/aarch64/register.hpp"

#include <map>
#include <set>
#include <sstream>
#include <string>

namespace switchrecomp::ir
{

namespace
{

[[nodiscard]] std::string location(const IrBasicBlock& block, const IrInstruction& instruction)
{
    std::ostringstream output;
    output << "block " << block.id.value << " at guest 0x" << std::hex << block.guest_start
           << ", instruction at guest 0x" << instruction.source.guest_pc;
    if (instruction.source.opcode != 0U)
    {
        output << " opcode 0x" << instruction.source.opcode;
    }
    return output.str();
}

[[nodiscard]] Result<void> failure(const IrBasicBlock& block, const IrInstruction& instruction,
                                   ErrorCode code, std::string message)
{
    return Result<void>::failure(
        make_error(code, "IR verification failed in " + location(block, instruction) + ": " +
                             std::move(message)));
}

[[nodiscard]] bool valid_general_register(const aarch64::Register& reg) noexcept
{
    return reg.kind == aarch64::RegisterKind::General &&
           (reg.width == aarch64::RegisterWidth::W32 || reg.width == aarch64::RegisterWidth::X64) &&
           (reg.is_stack_pointer || reg.is_zero || reg.index < 31U);
}

[[nodiscard]] bool has_result(IrOpcode opcode) noexcept
{
    switch (opcode)
    {
    case IrOpcode::Constant:
    case IrOpcode::ReadRegister:
    case IrOpcode::Add:
    case IrOpcode::Sub:
    case IrOpcode::And:
    case IrOpcode::Or:
    case IrOpcode::Xor:
    case IrOpcode::CompareEqual:
    case IrOpcode::CompareNotEqual:
    case IrOpcode::GuestLoad:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool valid_block(const std::set<BlockId>& blocks, const std::optional<BlockId>& id)
{
    return id.has_value() && id->valid() && blocks.contains(*id);
}

} // namespace

Result<void> verify_function(const IrFunction& function)
{
    if (function.blocks.empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::IrVerificationFailed, "IR function has no basic blocks"));
    }
    if (!function.entry_block.valid())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidBlockId, "IR function has an invalid entry block"));
    }

    std::set<BlockId> block_ids;
    bool has_entry = false;
    for (const auto& block : function.blocks)
    {
        if (!block.id.valid() || !block_ids.insert(block.id).second)
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidBlockId, "IR function contains duplicate/invalid block IDs"));
        }
        has_entry = has_entry || block.id == function.entry_block;
    }
    if (!has_entry)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidBlockId, "IR entry block does not exist"));
    }

    std::map<ValueId, IrType> definitions;
    for (const auto& block : function.blocks)
    {
        bool terminated = false;
        for (std::size_t index = 0U; index < block.instructions.size(); ++index)
        {
            const auto& instruction = block.instructions[index];
            if (terminated)
            {
                return failure(block, instruction, ErrorCode::IrVerificationFailed,
                               "instruction appears after a terminator");
            }
            const bool produces_value = has_result(instruction.opcode);
            if (produces_value != instruction.result.valid())
            {
                return failure(block, instruction, ErrorCode::InvalidValueId,
                               produces_value ? "value-producing instruction has no result ID"
                                               : "non-value instruction has a result ID");
            }
            if (produces_value && !definitions.emplace(instruction.result, instruction.type).second)
            {
                return failure(block, instruction, ErrorCode::InvalidValueId,
                               "duplicate ValueId definition");
            }

            const auto require_operands = [&](std::size_t count) -> Result<void> {
                if (instruction.operands.size() != count)
                {
                    return failure(block, instruction, ErrorCode::InvalidOperandCount,
                                   "expected " + std::to_string(count) + " operands, got " +
                                       std::to_string(instruction.operands.size()));
                }
                return Result<void>::success();
            };
            const auto require_value = [&](ValueId id, IrType expected) -> Result<void> {
                if (!id.valid())
                {
                    return failure(block, instruction, ErrorCode::InvalidValueId,
                                   "invalid operand ValueId");
                }
                if (produces_value && id == instruction.result)
                {
                    return failure(block, instruction, ErrorCode::UseBeforeDefinition,
                                   "instruction uses the ValueId it defines");
                }
                const auto defined = definitions.find(id);
                if (defined == definitions.end())
                {
                    return failure(block, instruction, ErrorCode::UseBeforeDefinition,
                                   "ValueId " + std::to_string(id.value) + " is not defined yet");
                }
                if (defined->second != expected)
                {
                    return failure(block, instruction, ErrorCode::InvalidIrType,
                                   "ValueId " + std::to_string(id.value) + " has type " +
                                       std::string(type_name(defined->second)) + ", expected " +
                                       std::string(type_name(expected)));
                }
                return Result<void>::success();
            };

            switch (instruction.opcode)
            {
            case IrOpcode::Constant:
                if (bit_width(instruction.type) == 0U)
                {
                    return failure(block, instruction, ErrorCode::InvalidIrType,
                                   "constant has an invalid type");
                }
                break;
            case IrOpcode::ReadRegister:
                if (!instruction.reg || !valid_general_register(*instruction.reg))
                {
                    return failure(block, instruction, ErrorCode::InvalidRegisterWidth,
                                   "read_reg requires a valid W/X general register");
                }
                if ((instruction.reg->width == aarch64::RegisterWidth::W32 && instruction.type != IrType::I32) ||
                    (instruction.reg->width == aarch64::RegisterWidth::X64 && instruction.type != IrType::I64))
                {
                    return failure(block, instruction, ErrorCode::InvalidRegisterWidth,
                                   "read_reg result type does not match register width");
                }
                break;
            case IrOpcode::WriteRegister:
                if (!instruction.reg || !valid_general_register(*instruction.reg))
                {
                    return failure(block, instruction, ErrorCode::InvalidRegisterWidth,
                                   "write_reg requires a valid W/X general register");
                }
                if (const auto count = require_operands(1U); !count)
                {
                    return count;
                }
                if (const auto value = require_value(instruction.operands[0],
                                                     instruction.reg->width == aarch64::RegisterWidth::W32
                                                         ? IrType::I32
                                                         : IrType::I64); !value)
                {
                    return value;
                }
                break;
            case IrOpcode::Add:
            case IrOpcode::Sub:
            case IrOpcode::And:
            case IrOpcode::Or:
            case IrOpcode::Xor:
                if (const auto count = require_operands(2U); !count)
                {
                    return count;
                }
                for (const auto operand : instruction.operands)
                {
                    if (const auto value = require_value(operand, instruction.type); !value)
                    {
                        return value;
                    }
                }
                break;
            case IrOpcode::CompareEqual:
            case IrOpcode::CompareNotEqual:
                if (instruction.type != IrType::I1)
                {
                    return failure(block, instruction, ErrorCode::InvalidIrType,
                                   "comparison result must be i1");
                }
                if (const auto count = require_operands(2U); !count)
                {
                    return count;
                }
                if (const auto left = definitions.find(instruction.operands[0]);
                    instruction.operands[0] == instruction.result)
                {
                    return failure(block, instruction, ErrorCode::UseBeforeDefinition,
                                   "comparison uses the value it defines");
                }
                else if (left == definitions.end())
                {
                    return failure(block, instruction, ErrorCode::UseBeforeDefinition,
                                   "comparison left operand is not defined");
                }
                else if (const auto right = definitions.find(instruction.operands[1]);
                         instruction.operands[1] == instruction.result)
                {
                    return failure(block, instruction, ErrorCode::UseBeforeDefinition,
                                   "comparison uses the value it defines");
                }
                else if (right == definitions.end())
                {
                    return failure(block, instruction, ErrorCode::UseBeforeDefinition,
                                   "comparison right operand is not defined");
                }
                else if (left->second != right->second)
                {
                    return failure(block, instruction, ErrorCode::InvalidIrType,
                                   "comparison operands have different types");
                }
                break;
            case IrOpcode::GuestLoad:
                if (instruction.access_size != 4U && instruction.access_size != 8U)
                {
                    return failure(block, instruction, ErrorCode::InvalidArgument,
                                   "guest_load size must be 4 or 8 bytes");
                }
                if (instruction.type != (instruction.access_size == 4U ? IrType::I32 : IrType::I64))
                {
                    return failure(block, instruction, ErrorCode::InvalidIrType,
                                   "guest_load type does not match access size");
                }
                if (const auto count = require_operands(1U); !count)
                {
                    return count;
                }
                if (const auto address = require_value(instruction.operands[0], IrType::I64); !address)
                {
                    return address;
                }
                break;
            case IrOpcode::GuestStore:
                if (instruction.access_size != 4U && instruction.access_size != 8U)
                {
                    return failure(block, instruction, ErrorCode::InvalidArgument,
                                   "guest_store size must be 4 or 8 bytes");
                }
                if (const auto count = require_operands(2U); !count)
                {
                    return count;
                }
                if (const auto address = require_value(instruction.operands[0], IrType::I64); !address)
                {
                    return address;
                }
                if (const auto value = require_value(instruction.operands[1],
                                                     instruction.access_size == 4U ? IrType::I32
                                                                                   : IrType::I64); !value)
                {
                    return value;
                }
                break;
            case IrOpcode::Branch:
                if (!valid_block(block_ids, instruction.target))
                {
                    return failure(block, instruction, ErrorCode::InvalidBlockId,
                                   "branch target does not exist");
                }
                break;
            case IrOpcode::ConditionalBranch:
                if (const auto count = require_operands(1U); !count)
                {
                    return count;
                }
                if (const auto condition = require_value(instruction.operands[0], IrType::I1);
                    !condition)
                {
                    return condition;
                }
                if (!valid_block(block_ids, instruction.true_target) ||
                    !valid_block(block_ids, instruction.false_target))
                {
                    return failure(block, instruction, ErrorCode::InvalidBlockId,
                                   "conditional branch target does not exist");
                }
                break;
            case IrOpcode::Return:
            case IrOpcode::Nop:
                if (!instruction.operands.empty())
                {
                    return failure(block, instruction, ErrorCode::InvalidOperandCount,
                                   "instruction does not accept operands");
                }
                break;
            }

            terminated = is_terminator(instruction.opcode);
            if (terminated && index + 1U != block.instructions.size())
            {
                return failure(block, instruction, ErrorCode::IrVerificationFailed,
                               "terminator is not the final instruction in the block");
            }
        }
        if (block.instructions.empty() || !is_terminator(block.instructions.back().opcode))
        {
            return Result<void>::failure(make_error(
                ErrorCode::IrVerificationFailed,
                "IR verification failed in block " + std::to_string(block.id.value) +
                    ": basic block has no terminator"));
        }
    }
    return Result<void>::success();
}

} // namespace switchrecomp::ir
