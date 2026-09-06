#include "switchrecomp/ir/interpreter.hpp"

#include "switchrecomp/ir/verifier.hpp"

#include <array>
#include <map>
#include <string>

namespace switchrecomp::ir
{

namespace
{

[[nodiscard]] Result<std::uint64_t> value_of(const std::map<ValueId, std::uint64_t>& values,
                                             ValueId id)
{
    const auto found = values.find(id);
    if (found == values.end())
    {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::InvalidValueId, "interpreter encountered an undefined ValueId"));
    }
    return Result<std::uint64_t>::success(found->second);
}

[[nodiscard]] Result<std::uint64_t> load_integer(const memory::GuestMemory& memory,
                                                 std::uint64_t address, std::uint8_t size)
{
    std::array<std::byte, 8> bytes{};
    const auto read = memory.read(address, std::span<std::byte>(bytes.data(), size));
    if (!read)
    {
        return Result<std::uint64_t>::failure(make_error(
            ErrorCode::ExecutionMemoryFault,
            "guest load at 0x" + std::to_string(address) + " failed: " + read.error().message));
    }
    std::uint64_t value = 0U;
    for (std::uint8_t index = 0U; index < size; ++index)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index])) <<
                 (static_cast<unsigned int>(index) * 8U);
    }
    return Result<std::uint64_t>::success(value);
}

[[nodiscard]] Result<void> store_integer(memory::GuestMemory& memory, std::uint64_t address,
                                          std::uint64_t value, std::uint8_t size)
{
    std::array<std::byte, 8> bytes{};
    for (std::uint8_t index = 0U; index < size; ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (static_cast<unsigned int>(index) * 8U)) &
                                               0xffU);
    }
    const auto write = memory.write(address, std::span<const std::byte>(bytes.data(), size));
    if (!write)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ExecutionMemoryFault,
            "guest store at 0x" + std::to_string(address) + " failed: " + write.error().message));
    }
    return Result<void>::success();
}

[[nodiscard]] const IrBasicBlock* find_block(const IrFunction& function, BlockId id)
{
    for (const auto& block : function.blocks)
    {
        if (block.id == id)
        {
            return &block;
        }
    }
    return nullptr;
}

} // namespace

Result<ExecutionResult> execute_function(const IrFunction& function, runtime::CpuState& state,
                                          memory::GuestMemory& memory,
                                          const InterpreterOptions& options)
{
    if (options.max_steps == 0U)
    {
        return Result<ExecutionResult>::failure(
            make_error(ErrorCode::InvalidArgument, "interpreter step limit must be non-zero"));
    }
    const auto verified = verify_function(function);
    if (!verified)
    {
        return Result<ExecutionResult>::failure(verified.error());
    }

    const IrBasicBlock* block = find_block(function, function.entry_block);
    if (block == nullptr)
    {
        return Result<ExecutionResult>::failure(
            make_error(ErrorCode::ExecutionInvalidBlock, "interpreter entry block does not exist"));
    }
    std::map<ValueId, std::uint64_t> values;
    std::size_t steps = 0U;
    while (block != nullptr)
    {
        state.pc = block->guest_start;
        bool transferred = false;
        for (const auto& instruction : block->instructions)
        {
            if (steps++ >= options.max_steps)
            {
                return Result<ExecutionResult>::failure(make_error(
                    ErrorCode::ExecutionStepLimitExceeded,
                    "semantic IR interpreter exceeded its configured step limit"));
            }
            const auto get = [&](ValueId id) { return value_of(values, id); };
            switch (instruction.opcode)
            {
            case IrOpcode::Constant:
                values[instruction.result] = mask_value(instruction.type, instruction.immediate);
                break;
            case IrOpcode::ReadRegister:
            {
                const auto value = runtime::read_register(state, instruction.reg.value());
                if (!value)
                {
                    return Result<ExecutionResult>::failure(value.error());
                }
                values[instruction.result] = mask_value(instruction.type, value.value());
                break;
            }
            case IrOpcode::WriteRegister:
            {
                const auto value = get(instruction.operands[0]);
                if (!value)
                {
                    return Result<ExecutionResult>::failure(value.error());
                }
                const auto written = runtime::write_register(state, instruction.reg.value(), value.value());
                if (!written)
                {
                    return Result<ExecutionResult>::failure(written.error());
                }
                break;
            }
            case IrOpcode::Add:
            case IrOpcode::Sub:
            case IrOpcode::And:
            case IrOpcode::Or:
            case IrOpcode::Xor:
            {
                const auto left = get(instruction.operands[0]);
                const auto right = get(instruction.operands[1]);
                if (!left || !right)
                {
                    return Result<ExecutionResult>::failure((!left ? left : right).error());
                }
                std::uint64_t result = 0U;
                if (instruction.opcode == IrOpcode::Add)
                {
                    result = left.value() + right.value();
                }
                else if (instruction.opcode == IrOpcode::Sub)
                {
                    result = left.value() - right.value();
                }
                else if (instruction.opcode == IrOpcode::And)
                {
                    result = left.value() & right.value();
                }
                else if (instruction.opcode == IrOpcode::Or)
                {
                    result = left.value() | right.value();
                }
                else
                {
                    result = left.value() ^ right.value();
                }
                values[instruction.result] = mask_value(instruction.type, result);
                break;
            }
            case IrOpcode::CompareEqual:
            case IrOpcode::CompareNotEqual:
            {
                const auto left = get(instruction.operands[0]);
                const auto right = get(instruction.operands[1]);
                if (!left || !right)
                {
                    return Result<ExecutionResult>::failure((!left ? left : right).error());
                }
                const bool equal = left.value() == right.value();
                values[instruction.result] =
                    (instruction.opcode == IrOpcode::CompareEqual ? equal : !equal) ? 1U : 0U;
                break;
            }
            case IrOpcode::GuestLoad:
            {
                const auto address = get(instruction.operands[0]);
                if (!address)
                {
                    return Result<ExecutionResult>::failure(address.error());
                }
                const auto loaded = load_integer(memory, address.value(), instruction.access_size);
                if (!loaded)
                {
                    return Result<ExecutionResult>::failure(loaded.error());
                }
                values[instruction.result] = mask_value(instruction.type, loaded.value());
                break;
            }
            case IrOpcode::GuestStore:
            {
                const auto address = get(instruction.operands[0]);
                const auto value = get(instruction.operands[1]);
                if (!address || !value)
                {
                    return Result<ExecutionResult>::failure((!address ? address : value).error());
                }
                const auto stored = store_integer(memory, address.value(), value.value(), instruction.access_size);
                if (!stored)
                {
                    return Result<ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case IrOpcode::Branch:
                block = find_block(function, instruction.target.value());
                if (block == nullptr)
                {
                    return Result<ExecutionResult>::failure(make_error(
                        ErrorCode::ExecutionInvalidBlock, "interpreter branch target does not exist"));
                }
                transferred = true;
                break;
            case IrOpcode::ConditionalBranch:
            {
                const auto condition = get(instruction.operands[0]);
                if (!condition)
                {
                    return Result<ExecutionResult>::failure(condition.error());
                }
                const auto target = condition.value() != 0U ? instruction.true_target
                                                            : instruction.false_target;
                block = find_block(function, target.value());
                if (block == nullptr)
                {
                    return Result<ExecutionResult>::failure(make_error(
                        ErrorCode::ExecutionInvalidBlock,
                        "interpreter conditional branch target does not exist"));
                }
                transferred = true;
                break;
            }
            case IrOpcode::Return:
                state.pc = instruction.source.guest_pc;
                return Result<ExecutionResult>::success(
                    ExecutionResult{state.pc, steps});
            case IrOpcode::Nop:
                break;
            }
            if (transferred)
            {
                break;
            }
        }
        if (!transferred)
        {
            return Result<ExecutionResult>::failure(make_error(
                ErrorCode::ExecutionInvalidBlock, "interpreter reached a block without transfer"));
        }
    }
    return Result<ExecutionResult>::failure(
        make_error(ErrorCode::ExecutionInvalidBlock, "interpreter reached an invalid block"));
}

} // namespace switchrecomp::ir
