#pragma once

#include <cstdint>

namespace switchrecomp::ir
{

using ValueId = std::uint32_t;
using BlockId = std::uint32_t;

inline constexpr ValueId invalid_value = UINT32_MAX;
inline constexpr BlockId invalid_block = UINT32_MAX;

} // namespace switchrecomp::ir
