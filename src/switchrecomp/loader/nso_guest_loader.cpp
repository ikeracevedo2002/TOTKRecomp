#include "switchrecomp/loader/nso_guest_loader.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/common/logging.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace switchrecomp::loader
{

namespace
{

using memory::GuestAddress;
using memory::GuestMemory;
using memory::GuestMemoryPermissions;
using memory::GuestRegionKind;

struct SegmentMapping
{
    GuestAddress base;
    std::span<const std::byte> bytes;
    GuestMemoryPermissions permissions;
    GuestRegionKind kind;
    std::string_view name;
};

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] Result<GuestAddress>
resolve_address(GuestAddress module_base, std::uint64_t memory_offset, std::string_view name)
{
    const auto address = checked_add_u64(module_base, memory_offset);
    if (!address)
    {
        return Result<GuestAddress>::failure(make_error(
            ErrorCode::ArithmeticOverflow,
            "cannot load " + std::string(name) + ": module base plus segment offset overflows"));
    }
    return address;
}

[[nodiscard]] Result<void> validate_image(const format::NsoImage& image)
{
    if (image.header.text.kind != format::NsoSegmentKind::Text ||
        image.header.rodata.kind != format::NsoSegmentKind::RoData ||
        image.header.data.kind != format::NsoSegmentKind::Data ||
        image.text.kind != format::NsoSegmentKind::Text ||
        image.rodata.kind != format::NsoSegmentKind::RoData ||
        image.data.kind != format::NsoSegmentKind::Data)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidFormat,
                       "NsoImage segment metadata contains an unexpected segment kind"));
    }

    const auto validate_segment = [](const format::NsoSegment& header_segment,
                                     const format::MaterializedNsoSegment& materialized,
                                     std::string_view name) -> Result<void>
    {
        if (materialized.memory_offset != header_segment.memory_offset)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidFormat,
                std::string(name) + " materialized offset does not match its NSO metadata"));
        }
        if (materialized.bytes.size() != header_segment.memory_size)
        {
            return Result<void>::failure(make_error(
                ErrorCode::SizeMismatch,
                std::string(name) + " materialized byte count does not match its NSO memory size"));
        }
        return Result<void>::success();
    };

    const std::array<std::tuple<const format::NsoSegment&, const format::MaterializedNsoSegment&,
                                std::string_view>,
                     3>
        segments{{{image.header.text, image.text, ".text"},
                  {image.header.rodata, image.rodata, ".rodata"},
                  {image.header.data, image.data, ".data"}}};
    for (const auto& entry : segments)
    {
        const auto& [header_segment, materialized, name] = entry;
        const auto valid = validate_segment(header_segment, materialized, name);
        if (!valid)
        {
            return valid;
        }
    }

    if (image.bss.size() != image.header.bss_size)
    {
        return Result<void>::failure(make_error(
            ErrorCode::SizeMismatch, "BSS materialized byte count does not match its NSO size"));
    }

    const auto expected_bss_offset =
        checked_add_u64(image.header.data.memory_offset, image.header.data.memory_size);
    if (!expected_bss_offset)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ArithmeticOverflow,
                       "data memory offset plus size overflows while validating the BSS address"));
    }
    if (image.bss_memory_offset != expected_bss_offset.value())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidFormat, "BSS memory offset does not follow the NSO data segment"));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> map_segment(GuestMemory& guest_memory, const SegmentMapping& mapping)
{
    const auto mapped = guest_memory.map(mapping.base, mapping.bytes, mapping.permissions,
                                         mapping.name, mapping.kind);
    if (!mapped)
    {
        return Result<void>::failure(make_error(
            mapped.error().code, "failed to map " + std::string(mapping.name) + " at " +
                                     hex_address(mapping.base) + ": " + mapped.error().message));
    }
    return Result<void>::success();
}

void log_mapping(const SegmentMapping& mapping)
{
    logging::log_debug(logging::LogCategory::Memory,
                       "mapped " + std::string(mapping.name) + " at " + hex_address(mapping.base) +
                           " (" + std::to_string(mapping.bytes.size()) + " bytes)");
}

} // namespace

Result<void> load_nso(const format::NsoImage& image, memory::GuestMemory& guest_memory,
                      const NsoGuestLoadOptions& options)
{
    const auto valid = validate_image(image);
    if (!valid)
    {
        return valid;
    }

    const auto text_base = resolve_address(options.module_base, image.text.memory_offset, ".text");
    const auto rodata_base =
        resolve_address(options.module_base, image.rodata.memory_offset, ".rodata");
    const auto data_base = resolve_address(options.module_base, image.data.memory_offset, ".data");
    const auto bss_base = resolve_address(options.module_base, image.bss_memory_offset, ".bss");
    if (!text_base)
    {
        return Result<void>::failure(text_base.error());
    }
    if (!rodata_base)
    {
        return Result<void>::failure(rodata_base.error());
    }
    if (!data_base)
    {
        return Result<void>::failure(data_base.error());
    }
    if (!bss_base)
    {
        return Result<void>::failure(bss_base.error());
    }

    const std::array<SegmentMapping, 4> mappings{
        SegmentMapping{text_base.value(), image.text.bytes,
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute,
                       GuestRegionKind::Text, ".text"},
        SegmentMapping{rodata_base.value(), image.rodata.bytes, GuestMemoryPermissions::Read,
                       GuestRegionKind::Rodata, ".rodata"},
        SegmentMapping{data_base.value(), image.data.bytes,
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       GuestRegionKind::Data, ".data"},
        SegmentMapping{bss_base.value(), image.bss,
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       GuestRegionKind::Bss, ".bss"}};

    try
    {
        // Stage the complete load so overlap, limit, and allocation failures cannot mutate the
        // caller's existing map. The copy is intentionally limited to module-load operations;
        // individual reads and writes never copy backing storage.
        GuestMemory staged = guest_memory;
        for (const auto& mapping : mappings)
        {
            const auto mapped = map_segment(staged, mapping);
            if (!mapped)
            {
                return mapped;
            }
        }
        guest_memory = std::move(staged);
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "failed to stage the NSO guest memory load"));
    }
    catch (const std::length_error&)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceLimit, "NSO guest memory load exceeds host container limits"));
    }

    for (const auto& mapping : mappings)
    {
        if (!mapping.bytes.empty())
        {
            log_mapping(mapping);
        }
    }
    return Result<void>::success();
}

} // namespace switchrecomp::loader
