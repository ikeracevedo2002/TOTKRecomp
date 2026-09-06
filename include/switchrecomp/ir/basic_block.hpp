#pragma once

#include "switchrecomp/ir/instruction.hpp"

#include <cstdint>
#include <vector>

namespace switchrecomp::ir
{

struct IrBasicBlock
{
    BlockId id{};
    aarch64::GuestAddress guest_start = 0U;
    std::vector<IrInstruction> instructions;
};

} // namespace switchrecomp::ir
