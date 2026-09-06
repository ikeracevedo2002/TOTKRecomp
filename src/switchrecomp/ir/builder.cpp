#include "switchrecomp/ir/builder.hpp"

#include <new>
#include <string>
#include <utility>

namespace switchrecomp::ir
{

Result<BlockId> IrBuilder::create_block(aarch64::GuestAddress guest_start)
{
    const auto id = BlockId{static_cast<std::uint32_t>(function_.blocks.size())};
    try
    {
        function_.blocks.push_back(IrBasicBlock{id, guest_start, {}});
    }
    catch (const std::bad_alloc&)
    {
        return Result<BlockId>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate semantic IR block"));
    }
    return Result<BlockId>::success(id);
}

ValueId IrBuilder::create_value() noexcept
{
    return ValueId{next_value_++};
}

IrBasicBlock* IrBuilder::find_block(BlockId block) noexcept
{
    for (auto& candidate : function_.blocks)
    {
        if (candidate.id == block)
        {
            return &candidate;
        }
    }
    return nullptr;
}

Result<void> IrBuilder::append(BlockId block, IrInstruction instruction)
{
    auto* target = find_block(block);
    if (target == nullptr)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidBlockId, "cannot append to an unknown IR block"));
    }
    if (!target->instructions.empty() && is_terminator(target->instructions.back().opcode))
    {
        return Result<void>::failure(make_error(
            ErrorCode::IrVerificationFailed,
            "cannot append an instruction after an IR block terminator"));
    }
    try
    {
        target->instructions.push_back(std::move(instruction));
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate semantic IR instruction"));
    }
    return Result<void>::success();
}

} // namespace switchrecomp::ir
