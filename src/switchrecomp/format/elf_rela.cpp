#include "switchrecomp/format/elf_rela.hpp"

#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace switchrecomp::format
{

namespace
{

using memory::GuestAddress;
using memory::GuestMemory;

[[nodiscard]] std::uint64_t load_u64_le(const std::byte* bytes) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index])) <<
                 (index * 8U);
    return value;
}

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
        const auto table_size = checked_mul_u64(
            static_cast<std::uint64_t>(count.value()), static_cast<std::uint64_t>(elf64_rela_size));
        if (!table_size)
            return Result<std::vector<RelaEntry>>::failure(make_error(
                table_size.error().code, std::string(name) + " table size overflows"));
        if (table_size.value() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return Result<std::vector<RelaEntry>>::failure(make_error(
                ErrorCode::ResourceLimit, std::string(name) + " table is too large for the host"));
        const auto table_end = checked_add_u64(table->address, table_size.value());
        if (!table_end)
            return Result<std::vector<RelaEntry>>::failure(make_error(
                table_end.error().code, std::string(name) + " table address overflows"));

        std::vector<std::byte> table_bytes(static_cast<std::size_t>(table_size.value()));
        if (!table_bytes.empty())
        {
            const auto read = guest_memory.read(table->address, table_bytes);
            if (!read)
                return Result<std::vector<RelaEntry>>::failure(make_error(
                    read.error().code,
                    "failed to read " + std::string(name) + " table: " + read.error().message));
        }
        std::vector<RelaEntry> result;
        result.reserve(count.value());
        for (std::size_t index = 0U; index < count.value(); ++index)
        {
            const auto byte_offset = index * elf64_rela_size;
            const auto* entry_bytes = table_bytes.data() + static_cast<std::ptrdiff_t>(byte_offset);
            const auto offset = load_u64_le(entry_bytes);
            const auto info = load_u64_le(entry_bytes + 8U);
            const auto raw_addend = load_u64_le(entry_bytes + 16U);

            const auto target_address = checked_add_u64(module_base, offset);
            if (!target_address)
            {
                return Result<std::vector<RelaEntry>>::failure(make_error(
                    target_address.error().code,
                    std::string(name) + " target address overflows module base"));
            }
            result.push_back(RelaEntry{offset, target_address.value(), info,
                                       std::bit_cast<std::int64_t>(raw_addend)});
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

AArch64RelocationType aarch64_relocation_type(std::uint32_t raw_type) noexcept
{
    switch (raw_type)
    {
    case 0U:
        return AArch64RelocationType::None;
    case 257U:
        return AArch64RelocationType::Abs64;
    case 258U:
        return AArch64RelocationType::Abs32;
    case 1027U:
        return AArch64RelocationType::Relative;
    case 1025U:
        return AArch64RelocationType::GlobDat;
    case 1026U:
        return AArch64RelocationType::JumpSlot;
    default:
        return AArch64RelocationType::Unknown;
    }
}

std::string_view aarch64_relocation_type_name(AArch64RelocationType type) noexcept
{
    switch (type)
    {
    case AArch64RelocationType::None:
        return "R_AARCH64_NONE";
    case AArch64RelocationType::Abs64:
        return "R_AARCH64_ABS64";
    case AArch64RelocationType::Abs32:
        return "R_AARCH64_ABS32";
    case AArch64RelocationType::Relative:
        return "R_AARCH64_RELATIVE";
    case AArch64RelocationType::GlobDat:
        return "R_AARCH64_GLOB_DAT";
    case AArch64RelocationType::JumpSlot:
        return "R_AARCH64_JUMP_SLOT";
    case AArch64RelocationType::Unknown:
        return "R_AARCH64_UNKNOWN";
    }
    return "R_AARCH64_UNKNOWN";
}

std::string_view relocation_source_name(RelocationSource source) noexcept
{
    switch (source)
    {
    case RelocationSource::Rela: return "RELA";
    case RelocationSource::JmpRel: return "JMPREL";
    }
    return "unknown";
}

Result<std::vector<Relocation>> make_relocations(std::span<const RelaEntry> entries,
                                                  RelocationSource source)
{
    try
    {
        std::vector<Relocation> result;
        result.reserve(entries.size());
        for (const auto& entry : entries)
        {
            result.push_back(Relocation{entry.offset, entry.target_address,
                                        entry.relocation_type(),
                                        aarch64_relocation_type(entry.relocation_type()),
                                        entry.symbol_index(), entry.addend, source});
        }
        return Result<std::vector<Relocation>>::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Result<std::vector<Relocation>>::failure(
            make_error(ErrorCode::ResourceLimit, "semantic relocation allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<std::vector<Relocation>>::failure(
            make_error(ErrorCode::ResourceLimit, "semantic relocation table exceeds host limits"));
    }
}

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
