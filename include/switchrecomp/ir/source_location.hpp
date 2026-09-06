#pragma once

#include <cstdint>
#include <string>

namespace switchrecomp::ir
{

using GuestAddress = std::uint64_t;

struct SourceLocation
{
    GuestAddress guest_pc = 0U;
    std::uint32_t opcode = 0U;
    std::string disassembly;

    friend bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

} // namespace switchrecomp::ir
