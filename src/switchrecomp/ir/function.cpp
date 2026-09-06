#include "switchrecomp/ir/function.hpp"

#include <utility>

namespace switchrecomp::ir
{

Function::Function(std::string name, GuestAddress guest_entry)
    : name_(std::move(name)), guest_entry_(guest_entry)
{
}

BlockId Function::add_block(GuestAddress guest_start, std::string label)
{
    const auto id = static_cast<BlockId>(blocks_.size());
    blocks_.push_back(BasicBlock{id, guest_start, std::move(label), {}, {}, false});
    return id;
}

ValueId Function::add_value(ValueKind kind, Type type, BlockId defining_block,
                            std::uint32_t instruction_index, std::uint64_t constant)
{
    const auto id = static_cast<ValueId>(values_.size());
    values_.push_back(ValueDefinition{id, kind, type, constant, defining_block,
                                      instruction_index});
    return id;
}

const BasicBlock* Function::block(BlockId id) const noexcept
{
    return id < blocks_.size() ? &blocks_[id] : nullptr;
}

BasicBlock* Function::block(BlockId id) noexcept
{
    return id < blocks_.size() ? &blocks_[id] : nullptr;
}

const ValueDefinition* Function::value(ValueId id) const noexcept
{
    return id < values_.size() ? &values_[id] : nullptr;
}

} // namespace switchrecomp::ir
