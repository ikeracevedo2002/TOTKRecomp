#include "switchrecomp/ir/builder.hpp"

#include <new>

namespace switchrecomp::ir
{

Result<void> Builder::set_insert_block(BlockId block)
{
    if (function_.block(block) == nullptr)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidIrBlock, "cannot insert into a missing IR block"));
    }
    insert_block_ = block;
    return Result<void>::success();
}

Result<ValueId> Builder::emit(Instruction instruction)
{
    auto* block = function_.block(insert_block_);
    if (block == nullptr)
    {
        return Result<ValueId>::failure(
            make_error(ErrorCode::InvalidIrBlock, "IR builder has no valid insertion block"));
    }
    if (block->has_terminator)
    {
        return Result<ValueId>::failure(make_error(
            ErrorCode::InvalidIrBlock, "cannot emit an instruction after a block terminator"));
    }
    if (!instruction.result_type.valid())
    {
        return Result<ValueId>::failure(
            make_error(ErrorCode::InvalidIrValue, "IR instruction has an invalid result type"));
    }
    try
    {
        const auto index = static_cast<std::uint32_t>(block->instructions.size());
        ValueId result = invalid_value;
        if (!instruction.result_type.is_void())
        {
            const auto kind = instruction.opcode == Opcode::Constant ? ValueKind::Constant
                                                                       : ValueKind::Instruction;
            result = function_.add_value(kind, instruction.result_type,
                                         kind == ValueKind::Constant ? invalid_block : insert_block_,
                                         kind == ValueKind::Constant ? 0U : index,
                                         instruction.constant, instruction.constant_high);
            instruction.result = result;
        }
        else if (instruction.result != invalid_value)
        {
            return Result<ValueId>::failure(make_error(
                ErrorCode::InvalidIrValue, "void IR instruction cannot define a value"));
        }
        block->instructions.push_back(std::move(instruction));
        return Result<ValueId>::success(result);
    }
    catch (const std::bad_alloc&)
    {
        return Result<ValueId>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate IR instruction storage"));
    }
}

Result<ValueId> Builder::constant(Type type, std::uint64_t value, SourceLocation source)
{
    Instruction instruction;
    instruction.opcode = Opcode::Constant;
    instruction.result_type = type;
    instruction.constant = value;
    instruction.source = std::move(source);
    return emit(std::move(instruction));
}

Result<ValueId> Builder::constant128(std::uint64_t low, std::uint64_t high, SourceLocation source)
{
    Instruction instruction;
    instruction.opcode = Opcode::Constant;
    instruction.result_type = v128_type();
    instruction.constant = low;
    instruction.constant_high = high;
    instruction.source = std::move(source);
    return emit(std::move(instruction));
}

Result<void> Builder::emit_void(Instruction instruction)
{
    if (!instruction.result_type.is_void())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidIrValue, "emit_void requires a void IR instruction"));
    }
    const auto emitted = emit(std::move(instruction));
    if (!emitted)
    {
        return Result<void>::failure(emitted.error());
    }
    return Result<void>::success();
}

Result<void> Builder::set_terminator(Terminator terminator)
{
    auto* block = function_.block(insert_block_);
    if (block == nullptr)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidIrBlock, "IR builder has no valid insertion block"));
    }
    if (block->has_terminator)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidIrBlock, "IR block already has a terminator"));
    }
    try
    {
        block->terminator = std::move(terminator);
        block->has_terminator = true;
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate IR terminator storage"));
    }
    return Result<void>::success();
}

} // namespace switchrecomp::ir
