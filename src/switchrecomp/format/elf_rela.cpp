#include "switchrecomp/format/elf_rela.hpp"

#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace switchrecomp::format
{

namespace
{

using memory::GuestAddress;
using memory::GuestMemory;

[[nodiscard]] Result<std::vector<RelaEntry>> parse_table(
    const GuestMemory& guest_memory, GuestAddress module_base,
    const std::optional<DynamicPointer>& table, const std::optional<std::size_t>& count,
    std::size_t max_relocations, std::string_view name)
{
    if (!table && !count)
    {
        return Result<std::vector<RelaEntry>>::success({});
    }
    if (!table || !count)
    {
        return Result<std::vector<RelaEntry>>::failure(make_error(
            ErrorCode::InvalidFormat,
            std::string(name) + " metadata is incomplete"));
    }
    if (count.value() > max_relocations)
    {
        return Result<std::vector<RelaEntry>>::failure(make_error(
            ErrorCode::ResourceLimit,
            std::string(name) + " entry count exceeds the configured relocation limit"));
    }

    try
    {
        std::vector<RelaEntry> result;
        result.reserve(count.value());
        for (std::size_t index = 0U; index < count.value(); ++index)
        {
            const auto byte_offset = checked_mul_u64(static_cast<std::uint64_t>(index),
                                                     static_cast<std::uint64_t>(elf64_rela_size));
            if (!byte_offset)
            {
                return Result<std::vector<RelaEntry>>::failure(make_error(
                    byte_offset.error().code,
                    std::string(name) + " entry offset overflows"));
            }
            const auto entry_address = checked_add_u64(table->address, byte_offset.value());
            if (!entry_address)
            {
                return Result<std::vector<RelaEntry>>::failure(make_error(
                    entry_address.error().code,
                    std::string(name) + " entry address overflows"));
            }

            std::array<std::byte, elf64_rela_size> bytes{};
            const auto read = guest_memory.read(entry_address.value(), bytes);
            if (!read)
            {
                return Result<std::vector<RelaEntry>>::failure(make_error(
                    read.error().code,
                    "failed to read " + std::string(name) + " entry " +
                        std::to_string(index) + ": " + read.error().message));
            }
            const BinaryReader reader(bytes);
            const auto offset = reader.read_u64_le(0U);
            const auto info = reader.read_u64_le(8U);
            const auto raw_addend = reader.read_u64_le(16U);
            if (!offset || !info || !raw_addend)
            {
                const Error* error = !offset ? &offset.error() : !info ? &info.error()
                                                                        : &raw_addend.error();
                return Result<std::vector<RelaEntry>>::failure(make_error(
                    error->code,
                    "failed to decode " + std::string(name) + " entry " +
                        std::to_string(index) + ": " + error->message));
            }

            const auto target_address = checked_add_u64(module_base, offset.value());
            if (!target_address)
            {
                return Result<std::vector<RelaEntry>>::failure(make_error(
                    target_address.error().code,
                    std::string(name) + " target address overflows module base"));
            }
            result.push_back(RelaEntry{offset.value(), target_address.value(), info.value(),
                                       std::bit_cast<std::int64_t>(raw_addend.value())});
        }
        return Result<std::vector<RelaEntry>>::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Result<std::vector<RelaEntry>>::failure(
            make_error(ErrorCode::ResourceLimit,
                       std::string(name) + " metadata allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<std::vector<RelaEntry>>::failure(
            make_error(ErrorCode::ResourceLimit,
                       std::string(name) + " metadata exceeds host container limits"));
    }
}

} // namespace

Result<std::vector<RelaEntry>> parse_rela_table(const GuestMemory& guest_memory,
                                                const DynamicInfo& dynamic,
                                                const DynamicParseLimits& limits)
{
    return parse_table(guest_memory, dynamic.module_base, dynamic.rela, dynamic.rela_count,
                       limits.max_relocations, "RELA");
}

Result<std::vector<RelaEntry>> parse_jmprel_table(const GuestMemory& guest_memory,
                                                  const DynamicInfo& dynamic,
                                                  const DynamicParseLimits& limits)
{
    if (dynamic.plt_rel_type &&
        dynamic.plt_rel_type.value() != static_cast<std::uint64_t>(DynamicTag::DT_RELA))
    {
        return Result<std::vector<RelaEntry>>::failure(make_error(
            ErrorCode::Unsupported,
            "unsupported DT_PLTREL encoding; only DT_RELA is supported"));
    }
    return parse_table(guest_memory, dynamic.module_base, dynamic.jmprel,
                       dynamic.jmprel_count, limits.max_relocations, "JMPREL");
}

} // namespace switchrecomp::format
