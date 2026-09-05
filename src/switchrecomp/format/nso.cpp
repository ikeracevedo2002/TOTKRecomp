#include "switchrecomp/format/nso.hpp"

#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>

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

} // namespace switchrecomp::format
