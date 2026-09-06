#pragma once

#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace switchrecomp
{

class BinaryReader
{
public:
    explicit BinaryReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t size() const noexcept
    {
        return bytes_.size();
    }

    [[nodiscard]] Result<std::uint16_t> read_u16_le(std::size_t offset) const;
    [[nodiscard]] Result<std::uint32_t> read_u32_le(std::size_t offset) const;
    [[nodiscard]] Result<std::uint64_t> read_u64_le(std::size_t offset) const;
    [[nodiscard]] Result<std::span<const std::byte>> slice(
        std::size_t offset,
        std::size_t size) const;

private:
    std::span<const std::byte> bytes_;
};

} // namespace switchrecomp
