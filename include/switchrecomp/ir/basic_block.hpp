#pragma once

#include "switchrecomp/ir/ids.hpp"
#include "switchrecomp/ir/instruction.hpp"

#include <string>
#include <vector>

namespace switchrecomp::ir
{

struct BasicBlock
{
    BlockId id = invalid_block;
    GuestAddress guest_start = 0U;
    std::string label;
    std::vector<Instruction> instructions;
    Terminator terminator;
    bool has_terminator = false;
};

} // namespace switchrecomp::ir
