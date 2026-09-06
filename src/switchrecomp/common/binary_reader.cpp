#include "switchrecomp/common/binary_reader.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>

namespace switchrecomp
{

Result<std::span<const std::byte>> BinaryReader::slice(std::size_t offset, std::size_t size) const
{
    const auto range = checked_range(offset, size, bytes_.size());
    if (!range)
    {
        return Result<std::span<const std::byte>>::failure(range.error());
    }
    return Result<std::span<const std::byte>>::success(bytes_.subspan(offset, size));
}

Result<std::uint16_t> BinaryReader::read_u16_le(std::size_t offset) const
{
    const auto bytes = slice(offset, sizeof(std::uint16_t));
    if (!bytes)
    {
        return Result<std::uint16_t>::failure(bytes.error());
    }
    const auto* data = bytes.value().data();
    const auto byte = [](std::byte value) { return std::to_integer<std::uint16_t>(value); };
    return Result<std::uint16_t>::success(
        static_cast<std::uint16_t>(byte(data[0]) | (byte(data[1]) << 8U)));
}

Result<std::uint32_t> BinaryReader::read_u32_le(std::size_t offset) const
{
    const auto bytes = slice(offset, sizeof(std::uint32_t));
    if (!bytes)
    {
        return Result<std::uint32_t>::failure(bytes.error());
    }
    const auto* data = bytes.value().data();
    const auto byte = [](std::byte value) { return std::to_integer<std::uint32_t>(value); };
    return Result<std::uint32_t>::success(
        byte(data[0]) | (byte(data[1]) << 8U) | (byte(data[2]) << 16U) | (byte(data[3]) << 24U));
}

Result<std::uint64_t> BinaryReader::read_u64_le(std::size_t offset) const
{
    const auto bytes = slice(offset, sizeof(std::uint64_t));
    if (!bytes)
    {
        return Result<std::uint64_t>::failure(bytes.error());
    }
    const auto* data = bytes.value().data();
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index)
    {
        value |= std::to_integer<std::uint64_t>(data[index]) << (index * 8U);
    }
    return Result<std::uint64_t>::success(value);
}

} // namespace switchrecomp
