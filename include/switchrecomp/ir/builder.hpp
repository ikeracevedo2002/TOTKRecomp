#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/function.hpp"

#include <span>

namespace switchrecomp::ir
{

class Builder
{
  public:
    explicit Builder(Function& function) : function_(function) {}

    [[nodiscard]] Result<void> set_insert_block(BlockId block);
    [[nodiscard]] Result<ValueId> emit(Instruction instruction);
    [[nodiscard]] Result<ValueId> constant(Type type, std::uint64_t value,
                                            SourceLocation source = {});
    [[nodiscard]] Result<void> emit_void(Instruction instruction);
    [[nodiscard]] Result<void> set_terminator(Terminator terminator);

    [[nodiscard]] BlockId insert_block() const noexcept { return insert_block_; }
    [[nodiscard]] Function& function() noexcept { return function_; }

  private:
    Function& function_;
    BlockId insert_block_ = invalid_block;
};

} // namespace switchrecomp::ir
