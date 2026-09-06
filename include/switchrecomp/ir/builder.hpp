#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

#include <cstdint>

namespace switchrecomp::ir
{

class IrBuilder
{
  public:
    explicit IrBuilder(IrFunction& function) : function_(function) {}

    [[nodiscard]] Result<BlockId> create_block(aarch64::GuestAddress guest_start);
    [[nodiscard]] ValueId create_value() noexcept;
    [[nodiscard]] Result<void> append(BlockId block, IrInstruction instruction);

  private:
    [[nodiscard]] IrBasicBlock* find_block(BlockId block) noexcept;

    IrFunction& function_;
    std::uint32_t next_value_ = 0U;
};

} // namespace switchrecomp::ir
