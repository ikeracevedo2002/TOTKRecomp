#pragma once

#include "switchrecomp/ir/basic_block.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace switchrecomp::ir
{

struct IrFunction
{
    std::string name;
    aarch64::GuestAddress entry = 0U;
    BlockId entry_block{};
    std::vector<IrBasicBlock> blocks;
};

} // namespace switchrecomp::ir
