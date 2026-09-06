#include "switchrecomp/format/nso_magic.hpp"

#include <array>

namespace switchrecomp::format
{

Result<NsoMagicStatus> inspect_nso_magic(std::span<const std::byte> bytes)
{
    if (bytes.size() < 4)
    {
        return Result<NsoMagicStatus>::success(NsoMagicStatus::TooShort);
    }

    constexpr std::array<char, 4> expected{'N', 'S', 'O', '0'};
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (std::to_integer<char>(bytes[index]) != expected[index])
        {
            return Result<NsoMagicStatus>::success(NsoMagicStatus::Unexpected);
        }
    }
    return Result<NsoMagicStatus>::success(NsoMagicStatus::Valid);
}

} // namespace switchrecomp::format
