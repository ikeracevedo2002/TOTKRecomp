#pragma once

#include "switchrecomp/common/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace switchrecomp::format
{

inline constexpr std::size_t nso_header_size = 0x100;

enum class NsoSegmentKind
{
    Text,
    RoData,
    Data,
};

[[nodiscard]] std::string_view nso_segment_kind_name(NsoSegmentKind kind) noexcept;

struct NsoSegment
{
    NsoSegmentKind kind;
    std::uint32_t file_offset;
    std::uint32_t memory_offset;
    std::uint32_t memory_size;
    std::uint32_t stored_size;
    bool compressed;
    bool hash_required;
    std::array<std::byte, 32> hash;
};

[[nodiscard]] constexpr bool is_compressed(const NsoSegment& segment) noexcept
{
    return segment.compressed;
}

[[nodiscard]] constexpr bool has_hash(const NsoSegment& segment) noexcept
{
    return segment.hash_required;
}

struct NsoRelativeRange
{
    std::uint32_t offset;
    std::uint32_t size;
};

struct NsoHeader
{
    std::uint32_t version;
    std::uint32_t reserved;
    std::uint32_t flags;

    NsoSegment text;
    NsoSegment rodata;
    NsoSegment data;

    std::uint32_t bss_size;

    std::uint32_t module_name_offset;
    std::uint32_t module_name_size;

    std::array<std::byte, 32> module_id;
    std::array<std::byte, 0x1C> reserved_region;

    NsoRelativeRange embedded;
    NsoRelativeRange dynstr;
    NsoRelativeRange dynsym;

    bool execute_only_memory;
    bool use_zbic_compression;

    [[nodiscard]] const NsoSegment& segment(NsoSegmentKind kind) const noexcept;
    [[nodiscard]] bool is_compressed(NsoSegmentKind kind) const noexcept;
    [[nodiscard]] bool has_hash(NsoSegmentKind kind) const noexcept;
};

[[nodiscard]] Result<NsoHeader> parse_nso_header(std::span<const std::byte> file_bytes);

[[nodiscard]] std::string module_id_hex(const NsoHeader& header);

} // namespace switchrecomp::format
