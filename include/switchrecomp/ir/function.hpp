#pragma once

#include "switchrecomp/ir/basic_block.hpp"
#include "switchrecomp/ir/ids.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace switchrecomp::ir
{

enum class ValueKind : std::uint8_t
{
    Parameter,
    Constant,
    Instruction,
};

struct ValueDefinition
{
    ValueId id = invalid_value;
    ValueKind kind = ValueKind::Instruction;
    Type type = void_type();
    std::uint64_t constant = 0U;
    std::uint64_t constant_high = 0U;
    BlockId defining_block = invalid_block;
    std::uint32_t instruction_index = 0U;
};

class Function
{
  public:
    Function() = default;
    Function(std::string name, GuestAddress guest_entry);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] GuestAddress guest_entry() const noexcept { return guest_entry_; }
    [[nodiscard]] BlockId entry_block() const noexcept { return entry_block_; }
    void set_entry_block(BlockId block) noexcept { entry_block_ = block; }

    [[nodiscard]] const std::vector<BasicBlock>& blocks() const noexcept { return blocks_; }
    [[nodiscard]] std::vector<BasicBlock>& blocks() noexcept { return blocks_; }
    [[nodiscard]] const std::vector<ValueDefinition>& values() const noexcept { return values_; }
    [[nodiscard]] std::vector<ValueDefinition>& values() noexcept { return values_; }

    [[nodiscard]] BlockId add_block(GuestAddress guest_start, std::string label);
    [[nodiscard]] ValueId add_value(ValueKind kind, Type type, BlockId defining_block,
                                     std::uint32_t instruction_index, std::uint64_t constant = 0U,
                                     std::uint64_t constant_high = 0U);

    [[nodiscard]] const BasicBlock* block(BlockId id) const noexcept;
    [[nodiscard]] BasicBlock* block(BlockId id) noexcept;
    [[nodiscard]] const ValueDefinition* value(ValueId id) const noexcept;

  private:
    std::string name_;
    GuestAddress guest_entry_ = 0U;
    BlockId entry_block_ = invalid_block;
    std::vector<BasicBlock> blocks_;
    std::vector<ValueDefinition> values_;
};

} // namespace switchrecomp::ir
