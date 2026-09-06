#include "switchrecomp/format/nso.hpp"

#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/common/sha256.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <lz4.h>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace switchrecomp::format
{

namespace
{

constexpr std::uint32_t kKnownFlagsMask = 0xffU;
constexpr std::uint32_t kSupportedVersion = 0U;
constexpr std::uint32_t kTextCompressedFlag = 1U << 0U;
constexpr std::uint32_t kRoDataCompressedFlag = 1U << 1U;
constexpr std::uint32_t kDataCompressedFlag = 1U << 2U;
constexpr std::uint32_t kTextHashFlag = 1U << 3U;
constexpr std::uint32_t kRoDataHashFlag = 1U << 4U;
constexpr std::uint32_t kDataHashFlag = 1U << 5U;
constexpr std::uint32_t kExecuteOnlyFlag = 1U << 6U;
constexpr std::uint32_t kZbicFlag = 1U << 7U;
constexpr std::uint64_t kAddressSpaceEnd = std::uint64_t{1} << 32U;

[[nodiscard]] std::string hex_value(std::uint64_t value, std::size_t digits)
{
    constexpr char digits_table[] = "0123456789abcdef";
    std::size_t required_digits = 1U;
    while (required_digits < 16U && value >= (std::uint64_t{1} << (required_digits * 4U)))
    {
        ++required_digits;
    }
    const auto width = std::max(digits, required_digits);
    std::string result;
    result.reserve(2U + width);
    result = "0x";
    for (std::size_t index = width; index > 0U; --index)
    {
        const auto shift = static_cast<unsigned int>((index - 1U) * 4U);
        result.push_back(digits_table[(value >> shift) & 0x0fU]);
    }
    return result;
}

[[nodiscard]] Result<NsoHeader> failure(ErrorCode code, std::string message)
{
    return Result<NsoHeader>::failure(make_error(code, std::move(message)));
}

class FieldReader
{
  public:
    explicit FieldReader(const BinaryReader& reader) : reader_(reader) {}

    bool read_u32(std::uint32_t& destination, std::size_t offset, std::string_view field)
    {
        const auto value = reader_.read_u32_le(offset);
        if (!value)
        {
            set_error(value.error(), field);
            return false;
        }
        destination = value.value();
        return true;
    }

    template <std::size_t Size>
    bool read_bytes(std::array<std::byte, Size>& destination, std::size_t offset,
                    std::string_view field)
    {
        const auto value = reader_.slice(offset, Size);
        if (!value)
        {
            set_error(value.error(), field);
            return false;
        }
        std::copy(value.value().begin(), value.value().end(), destination.begin());
        return true;
    }

    [[nodiscard]] const Error& error() const noexcept
    {
        return error_.value();
    }

  private:
    void set_error(const Error& source, std::string_view field)
    {
        if (!error_.has_value())
        {
            error_ = make_error(source.code, "failed to read NSO0 " + std::string(field) + ": " +
                                                 source.message);
        }
    }

    const BinaryReader& reader_;
    std::optional<Error> error_;
};

struct FileInterval
{
    std::string_view name;
    std::size_t begin;
    std::size_t end;
    bool non_empty;
};

struct MemoryInterval
{
    std::string_view name;
    std::uint64_t begin;
    std::uint64_t end;
    bool non_empty;
};

[[nodiscard]] Result<std::size_t> checked_file_end(std::uint32_t offset, std::uint32_t size,
                                                   std::string_view name)
{
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (static_cast<std::uint64_t>(offset) > maximum || static_cast<std::uint64_t>(size) > maximum)
    {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ArithmeticOverflow,
            std::string(name) + " file range offset + size overflows the host size type"));
    }

    const auto end = checked_add(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    if (!end)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::ArithmeticOverflow,
                       std::string(name) + " file range offset + size overflows"));
    }
    return end;
}

[[nodiscard]] Result<FileInterval> validate_file_range(std::uint32_t offset, std::uint32_t size,
                                                       std::size_t file_size, std::string_view name,
                                                       bool reject_header_start)
{
    const auto end = checked_file_end(offset, size, name);
    if (!end)
    {
        return Result<FileInterval>::failure(end.error());
    }
    if (end.value() > file_size)
    {
        return Result<FileInterval>::failure(make_error(
            ErrorCode::OutOfBounds, std::string(name) + " payload range [" + hex_value(offset, 8U) +
                                        ", " + hex_value(end.value(), 8U) + ") exceeds file size " +
                                        hex_value(file_size, 8U)));
    }
    if (reject_header_start && size != 0U && offset < nso_header_size)
    {
        return Result<FileInterval>::failure(
            make_error(ErrorCode::InvalidFormat,
                       std::string(name) + " payload starts inside the 0x100-byte NSO0 header"));
    }
    return Result<FileInterval>::success(
        FileInterval{name, static_cast<std::size_t>(offset), end.value(), size != 0U});
}

[[nodiscard]] bool overlaps(const FileInterval& left, const FileInterval& right) noexcept
{
    return left.non_empty && right.non_empty && left.begin < right.end && right.begin < left.end;
}

[[nodiscard]] Result<MemoryInterval> validate_memory_range(std::uint32_t offset, std::uint32_t size,
                                                           std::string_view name)
{
    const auto end = checked_file_end(offset, size, name);
    if (!end)
    {
        return Result<MemoryInterval>::failure(end.error());
    }

    const auto wide_end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(size);
    if (wide_end > kAddressSpaceEnd)
    {
        return Result<MemoryInterval>::failure(
            make_error(ErrorCode::ArithmeticOverflow,
                       std::string(name) + " memory range overflows 32-bit address space"));
    }

    return Result<MemoryInterval>::success(
        MemoryInterval{name, static_cast<std::uint64_t>(offset), wide_end, size != 0U});
}

[[nodiscard]] bool overlaps(const MemoryInterval& left, const MemoryInterval& right) noexcept
{
    return left.non_empty && right.non_empty && left.begin < right.end && right.begin < left.end;
}

[[nodiscard]] Result<void> validate_relative_range(const NsoRelativeRange& range,
                                                   std::uint32_t rodata_size, std::string_view name)
{
    const auto end = checked_file_end(range.offset, range.size, name);
    if (!end)
    {
        return Result<void>::failure(end.error());
    }
    if (end.value() > rodata_size)
    {
        return Result<void>::failure(make_error(
            ErrorCode::OutOfBounds,
            std::string(name) + " range offset=" + hex_value(range.offset, 8U) + " size=" +
                hex_value(range.size, 8U) + " exceeds rodata size " + hex_value(rodata_size, 8U)));
    }
    return Result<void>::success();
}

template <typename T>
[[nodiscard]] Result<T> materialization_failure(NsoSegmentKind kind, ErrorCode code,
                                                 std::string message)
{
    return Result<T>::failure(make_error(
        code, "failed to materialize ." + std::string(nso_segment_kind_name(kind)) + ": " +
                  std::move(message)));
}

[[nodiscard]] Result<std::span<const std::byte>> stored_segment_bytes(
    std::span<const std::byte> file_bytes, const NsoSegment& segment)
{
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (static_cast<std::uint64_t>(segment.file_offset) > maximum ||
        static_cast<std::uint64_t>(segment.stored_size) > maximum)
    {
        return Result<std::span<const std::byte>>::failure(make_error(
            ErrorCode::ArithmeticOverflow,
            "stored source range offset + size overflows the host size type"));
    }

    const auto end = checked_add(static_cast<std::size_t>(segment.file_offset),
                                 static_cast<std::size_t>(segment.stored_size));
    if (!end)
    {
        return Result<std::span<const std::byte>>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "stored source range offset + size overflows"));
    }
    if (end.value() > file_bytes.size())
    {
        return Result<std::span<const std::byte>>::failure(make_error(
            ErrorCode::OutOfBounds, "stored source range [" + hex_value(segment.file_offset, 8U) +
                                        ", " + hex_value(end.value(), 8U) +
                                        ") exceeds the input file size " +
                                        hex_value(file_bytes.size(), 8U)));
    }
    return Result<std::span<const std::byte>>::success(
        file_bytes.subspan(static_cast<std::size_t>(segment.file_offset),
                           static_cast<std::size_t>(segment.stored_size)));
}

[[nodiscard]] Result<std::vector<std::byte>> allocate_bytes(std::size_t size,
                                                              NsoSegmentKind kind,
                                                              std::string_view purpose)
{
    try
    {
        return Result<std::vector<std::byte>>::success(std::vector<std::byte>(size));
    }
    catch (const std::bad_alloc&)
    {
        return materialization_failure<std::vector<std::byte>>(
            kind, ErrorCode::ResourceLimit,
            "could not allocate " + std::string(purpose) + " buffer of " +
                hex_value(size, 1U));
    }
}

[[nodiscard]] Result<std::vector<std::byte>> copy_uncompressed_segment(
    std::span<const std::byte> source, const NsoSegment& segment)
{
    if (segment.stored_size != segment.memory_size)
    {
        return materialization_failure<std::vector<std::byte>>(
            segment.kind, ErrorCode::SizeMismatch,
            "uncompressed stored size " + hex_value(segment.stored_size, 8U) +
                " differs from memory size " + hex_value(segment.memory_size, 8U));
    }

    auto output = allocate_bytes(static_cast<std::size_t>(segment.memory_size), segment.kind,
                                  "materialized");
    if (!output)
    {
        return output;
    }
    auto result = std::move(output).value();
    std::copy(source.begin(), source.end(), result.begin());
    return Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] Result<std::vector<std::byte>> decompress_lz4_segment(
    std::span<const std::byte> source, const NsoSegment& segment)
{
    const auto maximum_lz4_integer = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (source.size() > maximum_lz4_integer ||
        static_cast<std::size_t>(segment.memory_size) > maximum_lz4_integer)
    {
        return materialization_failure<std::vector<std::byte>>(
            segment.kind, ErrorCode::DecompressionFailed,
            "stored or materialized size cannot be represented by the LZ4 API's int parameters");
    }

    if (segment.memory_size == 0U)
    {
        if (!source.empty())
        {
            return materialization_failure<std::vector<std::byte>>(
                segment.kind, ErrorCode::DecompressionFailed,
                "compressed input is non-empty but the declared decompressed size is zero");
        }
        return Result<std::vector<std::byte>>::success(std::vector<std::byte>{});
    }
    if (source.empty())
    {
        return materialization_failure<std::vector<std::byte>>(
            segment.kind, ErrorCode::DecompressionFailed,
            "compressed source is empty for a non-empty segment");
    }

    auto output = allocate_bytes(static_cast<std::size_t>(segment.memory_size), segment.kind,
                                 "decompressed");
    if (!output)
    {
        return output;
    }
    auto result = std::move(output).value();
    const auto decoded = LZ4_decompress_safe(
        reinterpret_cast<const char*>(source.data()), reinterpret_cast<char*>(result.data()),
        static_cast<int>(source.size()), static_cast<int>(result.size()));
    if (decoded < 0)
    {
        return materialization_failure<std::vector<std::byte>>(
            segment.kind, ErrorCode::DecompressionFailed,
            "LZ4 decompressor returned malformed input or output exceeded the destination");
    }
    if (static_cast<std::size_t>(decoded) != result.size())
    {
        return materialization_failure<std::vector<std::byte>>(
            segment.kind, ErrorCode::SizeMismatch,
            "expected " + hex_value(result.size(), 1U) + " decompressed bytes, got " +
                hex_value(static_cast<std::size_t>(decoded), 1U));
    }
    return Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] Result<void> verify_segment_hash(std::span<const std::byte> materialized,
                                               const NsoSegment& segment)
{
    if (!segment.hash_required)
    {
        return Result<void>::success();
    }

    const auto actual = sha256_bytes(materialized);
    if (!actual)
    {
        return Result<void>::failure(make_error(
            actual.error().code,
            "failed to verify ." + std::string(nso_segment_kind_name(segment.kind)) +
                ": SHA-256 calculation failed: " + actual.error().message));
    }

    if (actual.value().bytes != segment.hash)
    {
        const Sha256Digest expected{segment.hash};
        return Result<void>::failure(make_error(
            ErrorCode::HashMismatch,
            "failed to verify ." + std::string(nso_segment_kind_name(segment.kind)) +
                ": SHA-256 mismatch (expected " + sha256_to_hex(expected) + ", actual " +
                sha256_to_hex(actual.value()) + ")"));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::size_t> host_size(std::uint32_t value, std::string_view name)
{
    if (static_cast<std::uint64_t>(value) >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ArithmeticOverflow,
            std::string(name) + " does not fit in the host size type"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(value));
}

[[nodiscard]] Result<void> validate_materialization_limits(
    const NsoHeader& header, const NsoMaterializationLimits& limits)
{
    const std::array<std::pair<std::uint32_t, std::string_view>, 4> sizes{
        std::pair{header.text.memory_size, ".text"},
        std::pair{header.rodata.memory_size, ".rodata"},
        std::pair{header.data.memory_size, ".data"},
        std::pair{header.bss_size, "BSS"},
    };

    std::array<std::size_t, sizes.size()> host_sizes{};
    for (std::size_t index = 0U; index < sizes.size(); ++index)
    {
        const auto converted = host_size(sizes[index].first, sizes[index].second);
        if (!converted)
        {
            return Result<void>::failure(converted.error());
        }
        host_sizes[index] = converted.value();
        if (host_sizes[index] > limits.max_segment_size)
        {
            return Result<void>::failure(make_error(
                ErrorCode::ResourceLimit,
                std::string("requested ") + std::string(sizes[index].second) +
                    " materialized size " + hex_value(host_sizes[index], 1U) +
                    " exceeds the configured per-segment limit " +
                    hex_value(limits.max_segment_size, 1U)));
        }
    }

    std::size_t total = 0U;
    for (const auto size : host_sizes)
    {
        const auto next = checked_add(total, size);
        if (!next)
        {
            return Result<void>::failure(make_error(
                ErrorCode::ArithmeticOverflow,
                "total materialized NSO image size overflows the host size type"));
        }
        total = next.value();
    }
    if (total > limits.max_total_image_size)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceLimit,
            "requested total materialized NSO image size " + hex_value(total, 1U) +
                " exceeds the configured limit " + hex_value(limits.max_total_image_size, 1U)));
    }
    return Result<void>::success();
}

[[nodiscard]] NsoCompressionKind compression_kind(const NsoHeader& header,
                                                  const NsoSegment& segment) noexcept
{
    if (!segment.compressed)
    {
        return NsoCompressionKind::None;
    }
    return header.use_zbic_compression ? NsoCompressionKind::Zbic : NsoCompressionKind::Lz4;
}

[[nodiscard]] Result<MaterializedNsoSegment> materialize_segment(
    std::span<const std::byte> file_bytes, const NsoHeader& header, const NsoSegment& segment)
{
    const auto source = stored_segment_bytes(file_bytes, segment);
    if (!source)
    {
        return materialization_failure<MaterializedNsoSegment>(
            segment.kind, source.error().code, source.error().message);
    }

    const auto compression = compression_kind(header, segment);
    if (compression == NsoCompressionKind::Zbic)
    {
        return materialization_failure<MaterializedNsoSegment>(
            segment.kind, ErrorCode::UnsupportedCompression,
            "ZBIC compression was requested by the NSO header; this implementation intentionally "
            "does not support ZBIC yet");
    }

    auto bytes = segment.compressed ? decompress_lz4_segment(source.value(), segment)
                                    : copy_uncompressed_segment(source.value(), segment);
    if (!bytes)
    {
        return Result<MaterializedNsoSegment>::failure(bytes.error());
    }

    const auto hash = verify_segment_hash(bytes.value(), segment);
    if (!hash)
    {
        return Result<MaterializedNsoSegment>::failure(hash.error());
    }

    return Result<MaterializedNsoSegment>::success(
        MaterializedNsoSegment{segment.kind,
                               segment.memory_offset,
                               std::move(bytes).value(),
                               compression,
                               segment.hash_required ? NsoHashStatus::Verified
                                                      : NsoHashStatus::NotRequired});
}

} // namespace

std::string_view nso_segment_kind_name(NsoSegmentKind kind) noexcept
{
    switch (kind)
    {
    case NsoSegmentKind::Text:
        return "text";
    case NsoSegmentKind::RoData:
        return "rodata";
    case NsoSegmentKind::Data:
        return "data";
    }
    return "unknown";
}

const NsoSegment& NsoHeader::segment(NsoSegmentKind kind) const noexcept
{
    switch (kind)
    {
    case NsoSegmentKind::Text:
        return text;
    case NsoSegmentKind::RoData:
        return rodata;
    case NsoSegmentKind::Data:
        return data;
    }
    return text;
}

bool NsoHeader::is_compressed(NsoSegmentKind kind) const noexcept
{
    return segment(kind).compressed;
}

bool NsoHeader::has_hash(NsoSegmentKind kind) const noexcept
{
    return segment(kind).hash_required;
}

Result<NsoHeader> parse_nso_header(std::span<const std::byte> file_bytes)
{
    if (file_bytes.size() < nso_header_size)
    {
        return failure(ErrorCode::OutOfBounds,
                       "NSO0 header requires at least 256 bytes, file has " +
                           std::to_string(file_bytes.size()));
    }

    const BinaryReader reader(file_bytes);
    const auto magic = reader.slice(0U, 4U);
    if (!magic)
    {
        return failure(ErrorCode::OutOfBounds,
                       "failed to read NSO0 magic: " + magic.error().message);
    }
    constexpr std::array<std::byte, 4> expected_magic{std::byte{'N'}, std::byte{'S'},
                                                      std::byte{'O'}, std::byte{'0'}};
    if (!std::equal(magic.value().begin(), magic.value().end(), expected_magic.begin()))
    {
        return failure(ErrorCode::InvalidFormat, "invalid NSO0 magic: expected 4e534f30");
    }

    NsoHeader header{};
    std::uint32_t text_file_offset = 0U;
    std::uint32_t text_memory_offset = 0U;
    std::uint32_t text_memory_size = 0U;
    std::uint32_t rodata_file_offset = 0U;
    std::uint32_t rodata_memory_offset = 0U;
    std::uint32_t rodata_memory_size = 0U;
    std::uint32_t data_file_offset = 0U;
    std::uint32_t data_memory_offset = 0U;
    std::uint32_t data_memory_size = 0U;
    std::uint32_t text_stored_size = 0U;
    std::uint32_t rodata_stored_size = 0U;
    std::uint32_t data_stored_size = 0U;

    FieldReader fields(reader);
    const bool fields_read =
        fields.read_u32(header.version, 0x004U, "version") &&
        fields.read_u32(header.reserved, 0x008U, "reserved") &&
        fields.read_u32(header.flags, 0x00cU, "flags") &&
        fields.read_u32(text_file_offset, 0x010U, "text file offset") &&
        fields.read_u32(text_memory_offset, 0x014U, "text memory offset") &&
        fields.read_u32(text_memory_size, 0x018U, "text memory size") &&
        fields.read_u32(header.module_name_offset, 0x01cU, "module name offset") &&
        fields.read_u32(rodata_file_offset, 0x020U, "rodata file offset") &&
        fields.read_u32(rodata_memory_offset, 0x024U, "rodata memory offset") &&
        fields.read_u32(rodata_memory_size, 0x028U, "rodata memory size") &&
        fields.read_u32(header.module_name_size, 0x02cU, "module name size") &&
        fields.read_u32(data_file_offset, 0x030U, "data file offset") &&
        fields.read_u32(data_memory_offset, 0x034U, "data memory offset") &&
        fields.read_u32(data_memory_size, 0x038U, "data memory size") &&
        fields.read_u32(header.bss_size, 0x03cU, "bss size") &&
        fields.read_bytes(header.module_id, 0x040U, "module ID") &&
        fields.read_u32(text_stored_size, 0x060U, "text stored size") &&
        fields.read_u32(rodata_stored_size, 0x064U, "rodata stored size") &&
        fields.read_u32(data_stored_size, 0x068U, "data stored size") &&
        fields.read_bytes(header.reserved_region, 0x06cU, "reserved region") &&
        fields.read_u32(header.embedded.offset, 0x088U, "embedded offset") &&
        fields.read_u32(header.embedded.size, 0x08cU, "embedded size") &&
        fields.read_u32(header.dynstr.offset, 0x090U, "DynStr offset") &&
        fields.read_u32(header.dynstr.size, 0x094U, "DynStr size") &&
        fields.read_u32(header.dynsym.offset, 0x098U, "DynSym offset") &&
        fields.read_u32(header.dynsym.size, 0x09cU, "DynSym size") &&
        fields.read_bytes(header.text.hash, 0x0a0U, "text hash") &&
        fields.read_bytes(header.rodata.hash, 0x0c0U, "rodata hash") &&
        fields.read_bytes(header.data.hash, 0x0e0U, "data hash");
    if (!fields_read)
    {
        return failure(fields.error().code, fields.error().message);
    }

    if (header.version != kSupportedVersion)
    {
        return failure(ErrorCode::Unsupported,
                       "unsupported NSO0 version " + std::to_string(header.version));
    }

    const auto unknown_flags = header.flags & ~kKnownFlagsMask;
    if (unknown_flags != 0U)
    {
        return failure(ErrorCode::Unsupported,
                       "unsupported NSO0 flag bits: " + hex_value(unknown_flags, 8U));
    }

    header.text = NsoSegment{NsoSegmentKind::Text,
                             text_file_offset,
                             text_memory_offset,
                             text_memory_size,
                             text_stored_size,
                             (header.flags & kTextCompressedFlag) != 0U,
                             (header.flags & kTextHashFlag) != 0U,
                             header.text.hash};
    header.rodata = NsoSegment{NsoSegmentKind::RoData,
                               rodata_file_offset,
                               rodata_memory_offset,
                               rodata_memory_size,
                               rodata_stored_size,
                               (header.flags & kRoDataCompressedFlag) != 0U,
                               (header.flags & kRoDataHashFlag) != 0U,
                               header.rodata.hash};
    header.data = NsoSegment{NsoSegmentKind::Data,
                             data_file_offset,
                             data_memory_offset,
                             data_memory_size,
                             data_stored_size,
                             (header.flags & kDataCompressedFlag) != 0U,
                             (header.flags & kDataHashFlag) != 0U,
                             header.data.hash};
    header.execute_only_memory = (header.flags & kExecuteOnlyFlag) != 0U;
    header.use_zbic_compression = (header.flags & kZbicFlag) != 0U;

    const auto module_name = validate_file_range(header.module_name_offset, header.module_name_size,
                                                 file_bytes.size(), "module name", true);
    if (!module_name)
    {
        return failure(module_name.error().code, module_name.error().message);
    }

    const auto text_file = validate_file_range(header.text.file_offset, header.text.stored_size,
                                               file_bytes.size(), "text", true);
    if (!text_file)
    {
        return failure(text_file.error().code, text_file.error().message);
    }
    const auto rodata_file = validate_file_range(
        header.rodata.file_offset, header.rodata.stored_size, file_bytes.size(), "rodata", true);
    if (!rodata_file)
    {
        return failure(rodata_file.error().code, rodata_file.error().message);
    }
    const auto data_file = validate_file_range(header.data.file_offset, header.data.stored_size,
                                               file_bytes.size(), "data", true);
    if (!data_file)
    {
        return failure(data_file.error().code, data_file.error().message);
    }

    if (!header.text.compressed && header.text.stored_size != header.text.memory_size)
    {
        return failure(ErrorCode::SizeMismatch,
                       "uncompressed text stored size " + hex_value(header.text.stored_size, 8U) +
                           " differs from memory size " + hex_value(header.text.memory_size, 8U));
    }
    if (!header.rodata.compressed && header.rodata.stored_size != header.rodata.memory_size)
    {
        return failure(ErrorCode::SizeMismatch, "uncompressed rodata stored size " +
                                                    hex_value(header.rodata.stored_size, 8U) +
                                                    " differs from memory size " +
                                                    hex_value(header.rodata.memory_size, 8U));
    }
    if (!header.data.compressed && header.data.stored_size != header.data.memory_size)
    {
        return failure(ErrorCode::SizeMismatch,
                       "uncompressed data stored size " + hex_value(header.data.stored_size, 8U) +
                           " differs from memory size " + hex_value(header.data.memory_size, 8U));
    }

    const std::array<FileInterval, 4> file_ranges{module_name.value(), text_file.value(),
                                                  rodata_file.value(), data_file.value()};
    for (std::size_t left = 0U; left < file_ranges.size(); ++left)
    {
        for (std::size_t right = left + 1U; right < file_ranges.size(); ++right)
        {
            if (overlaps(file_ranges[left], file_ranges[right]))
            {
                return failure(ErrorCode::InvalidFormat, std::string(file_ranges[left].name) +
                                                             " file range overlaps " +
                                                             std::string(file_ranges[right].name));
            }
        }
    }

    const auto text_memory =
        validate_memory_range(header.text.memory_offset, header.text.memory_size, "text");
    if (!text_memory)
    {
        return failure(text_memory.error().code, text_memory.error().message);
    }
    const auto rodata_memory =
        validate_memory_range(header.rodata.memory_offset, header.rodata.memory_size, "rodata");
    if (!rodata_memory)
    {
        return failure(rodata_memory.error().code, rodata_memory.error().message);
    }
    const auto data_memory =
        validate_memory_range(header.data.memory_offset, header.data.memory_size, "data");
    if (!data_memory)
    {
        return failure(data_memory.error().code, data_memory.error().message);
    }

    const std::array<MemoryInterval, 3> memory_ranges{text_memory.value(), rodata_memory.value(),
                                                      data_memory.value()};
    for (std::size_t left = 0U; left < memory_ranges.size(); ++left)
    {
        for (std::size_t right = left + 1U; right < memory_ranges.size(); ++right)
        {
            if (overlaps(memory_ranges[left], memory_ranges[right]))
            {
                return failure(ErrorCode::InvalidFormat,
                               std::string(memory_ranges[left].name) + " memory range overlaps " +
                                   std::string(memory_ranges[right].name));
            }
        }
    }

    const auto data_memory_end = checked_add(static_cast<std::size_t>(header.data.memory_offset),
                                             static_cast<std::size_t>(header.data.memory_size));
    if (!data_memory_end)
    {
        return failure(ErrorCode::ArithmeticOverflow,
                       "data memory range overflows while calculating the BSS start");
    }
    const auto bss_end =
        checked_add(data_memory_end.value(), static_cast<std::size_t>(header.bss_size));
    if (!bss_end || static_cast<std::uint64_t>(bss_end.value()) > kAddressSpaceEnd)
    {
        return failure(ErrorCode::ArithmeticOverflow,
                       "data memory range plus BSS size overflows 32-bit address space");
    }

    const auto embedded =
        validate_relative_range(header.embedded, header.rodata.memory_size, "embedded");
    if (!embedded)
    {
        return failure(embedded.error().code, embedded.error().message);
    }
    const auto dynstr =
        validate_relative_range(header.dynstr, header.rodata.memory_size, "rodata DynStr");
    if (!dynstr)
    {
        return failure(dynstr.error().code, dynstr.error().message);
    }
    const auto dynsym =
        validate_relative_range(header.dynsym, header.rodata.memory_size, "rodata DynSym");
    if (!dynsym)
    {
        return failure(dynsym.error().code, dynsym.error().message);
    }

    return Result<NsoHeader>::success(std::move(header));
}

std::string module_id_hex(const NsoHeader& header)
{
    constexpr char digits_table[] = "0123456789abcdef";
    std::string result;
    result.reserve(header.module_id.size() * 2U);
    for (const auto byte : header.module_id)
    {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(digits_table[(value >> 4U) & 0x0fU]);
        result.push_back(digits_table[value & 0x0fU]);
    }
    return result;
}

std::string_view nso_compression_kind_name(NsoCompressionKind kind) noexcept
{
    switch (kind)
    {
    case NsoCompressionKind::None:
        return "none";
    case NsoCompressionKind::Lz4:
        return "lz4";
    case NsoCompressionKind::Zbic:
        return "zbic";
    }
    return "unknown";
}

std::string_view nso_hash_status_name(NsoHashStatus status) noexcept
{
    switch (status)
    {
    case NsoHashStatus::NotRequired:
        return "not-required";
    case NsoHashStatus::Verified:
        return "verified";
    }
    return "unknown";
}

Result<NsoImage> materialize_nso(std::span<const std::byte> file_bytes, const NsoHeader& header,
                                 const NsoMaterializationLimits& limits)
{
    const auto limit_check = validate_materialization_limits(header, limits);
    if (!limit_check)
    {
        return Result<NsoImage>::failure(limit_check.error());
    }

    auto text = materialize_segment(file_bytes, header, header.text);
    if (!text)
    {
        return Result<NsoImage>::failure(text.error());
    }
    auto rodata = materialize_segment(file_bytes, header, header.rodata);
    if (!rodata)
    {
        return Result<NsoImage>::failure(rodata.error());
    }
    auto data = materialize_segment(file_bytes, header, header.data);
    if (!data)
    {
        return Result<NsoImage>::failure(data.error());
    }

    const auto bss_offset = checked_add(static_cast<std::size_t>(header.data.memory_offset),
                                        static_cast<std::size_t>(header.data.memory_size));
    if (!bss_offset || static_cast<std::uint64_t>(bss_offset.value()) > kAddressSpaceEnd)
    {
        return Result<NsoImage>::failure(make_error(
            ErrorCode::ArithmeticOverflow,
            "failed to materialize BSS: data memory range overflows the address space"));
    }
    const auto bss_end = checked_add(bss_offset.value(), static_cast<std::size_t>(header.bss_size));
    if (!bss_end || static_cast<std::uint64_t>(bss_end.value()) > kAddressSpaceEnd)
    {
        return Result<NsoImage>::failure(make_error(
            ErrorCode::ArithmeticOverflow,
            "failed to materialize BSS: data memory range plus BSS size overflows the address space"));
    }

    auto bss = allocate_bytes(static_cast<std::size_t>(header.bss_size), NsoSegmentKind::Data,
                              "BSS");
    if (!bss)
    {
        return Result<NsoImage>::failure(bss.error());
    }
    auto zero_filled_bss = std::move(bss).value();
    std::fill(zero_filled_bss.begin(), zero_filled_bss.end(), std::byte{0});

    return Result<NsoImage>::success(
        NsoImage{header,
                 std::move(text).value(),
                 std::move(rodata).value(),
                 std::move(data).value(),
                 static_cast<std::uint64_t>(bss_offset.value()),
                 std::move(zero_filled_bss)});
}

} // namespace switchrecomp::format
