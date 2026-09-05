#pragma once

#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <span>

namespace switchrecomp::format
{

enum class NsoMagicStatus
{
    Valid,
    TooShort,
    Unexpected,
};

[[nodiscard]] Result<NsoMagicStatus> inspect_nso_magic(std::span<const std::byte> bytes);

} // namespace switchrecomp::format
