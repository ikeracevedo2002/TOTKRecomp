#pragma once

#include "switchrecomp/common/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

enum class NsoCompressionKind
{
    None,
    Lz4,
    Zbic,
};

[[nodiscard]] std::string_view nso_compression_kind_name(NsoCompressionKind kind) noexcept;

enum class NsoHashStatus
{
    NotRequired,
    Verified,
};

[[nodiscard]] std::string_view nso_hash_status_name(NsoHashStatus status) noexcept;

struct MaterializedNsoSegment
{
    NsoSegmentKind kind;
    std::uint32_t memory_offset;
    std::vector<std::byte> bytes;
    NsoCompressionKind compression;
    NsoHashStatus hash_status;
};

struct NsoImage
{
    NsoHeader header;
    MaterializedNsoSegment text;
    MaterializedNsoSegment rodata;
    MaterializedNsoSegment data;
    std::uint64_t bss_memory_offset;
    std::vector<std::byte> bss;
};

inline constexpr std::size_t nso_default_max_segment_size =
    std::size_t{256U} * std::size_t{1024U} * std::size_t{1024U};
inline constexpr std::size_t nso_default_max_total_image_size =
    std::size_t{512U} * std::size_t{1024U} * std::size_t{1024U};

struct NsoMaterializationLimits
{
    std::size_t max_segment_size = nso_default_max_segment_size;
    std::size_t max_total_image_size = nso_default_max_total_image_size;
};

[[nodiscard]] Result<NsoImage> materialize_nso(
    std::span<const std::byte> file_bytes,
    const NsoHeader& header,
    const NsoMaterializationLimits& limits = {});

} // namespace switchrecomp::format
