#include "switchrecomp/format/nso.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

struct SegmentValues
{
    std::uint32_t file_offset;
    std::uint32_t memory_offset;
    std::uint32_t memory_size;
    std::uint32_t stored_size;
};

class SyntheticNsoBuilder
{
  public:
    SyntheticNsoBuilder()
        : module_id_(pattern(0x10U)), text_hash_(pattern(0x40U)), rodata_hash_(pattern(0x60U)),
          data_hash_(pattern(0x80U))
    {
    }

    SyntheticNsoBuilder& set_version(std::uint32_t value)
    {
        version_ = value;
        return *this;
    }

    SyntheticNsoBuilder& set_flags(std::uint32_t value)
    {
        flags_ = value;
        return *this;
    }

    SyntheticNsoBuilder& set_segment(switchrecomp::format::NsoSegmentKind kind,
                                     std::uint32_t file_offset, std::uint32_t memory_offset,
                                     std::uint32_t memory_size, std::uint32_t stored_size)
    {
        SegmentValues* segment = &text_;
        switch (kind)
        {
        case switchrecomp::format::NsoSegmentKind::Text:
            segment = &text_;
            break;
        case switchrecomp::format::NsoSegmentKind::RoData:
            segment = &rodata_;
            break;
        case switchrecomp::format::NsoSegmentKind::Data:
            segment = &data_;
            break;
        }
        *segment = SegmentValues{file_offset, memory_offset, memory_size, stored_size};
        return *this;
    }

    SyntheticNsoBuilder& set_bss_size(std::uint32_t value)
    {
        bss_size_ = value;
        return *this;
    }

    SyntheticNsoBuilder& set_module_name(std::uint32_t offset, std::uint32_t size)
    {
        module_name_offset_ = offset;
        module_name_size_ = size;
        return *this;
    }

    SyntheticNsoBuilder& set_module_id(const std::array<std::byte, 32>& value)
    {
        module_id_ = value;
        return *this;
    }

    SyntheticNsoBuilder& set_hash(switchrecomp::format::NsoSegmentKind kind,
                                  const std::array<std::byte, 32>& value)
    {
        switch (kind)
        {
        case switchrecomp::format::NsoSegmentKind::Text:
            text_hash_ = value;
            break;
        case switchrecomp::format::NsoSegmentKind::RoData:
            rodata_hash_ = value;
            break;
        case switchrecomp::format::NsoSegmentKind::Data:
            data_hash_ = value;
            break;
        }
        return *this;
    }

    SyntheticNsoBuilder& set_embedded(std::uint32_t offset, std::uint32_t size)
    {
        embedded_ = {offset, size};
        return *this;
    }

    SyntheticNsoBuilder& set_dynstr(std::uint32_t offset, std::uint32_t size)
    {
        dynstr_ = {offset, size};
        return *this;
    }

    SyntheticNsoBuilder& set_dynsym(std::uint32_t offset, std::uint32_t size)
    {
        dynsym_ = {offset, size};
        return *this;
    }

    [[nodiscard]] std::vector<std::byte> build(std::size_t file_size = 0x130U) const
    {
        std::vector<std::byte> bytes(file_size);
        if (file_size < switchrecomp::format::nso_header_size)
        {
            return bytes;
        }

        bytes[0x000U] = std::byte{'N'};
        bytes[0x001U] = std::byte{'S'};
        bytes[0x002U] = std::byte{'O'};
        bytes[0x003U] = std::byte{'0'};
        write_u32(bytes, 0x004U, version_);
        write_u32(bytes, 0x008U, 0U);
        write_u32(bytes, 0x00cU, flags_);
        write_segment(bytes, 0x010U, text_);
        write_u32(bytes, 0x01cU, module_name_offset_);
        write_segment(bytes, 0x020U, rodata_);
        write_u32(bytes, 0x02cU, module_name_size_);
        write_segment(bytes, 0x030U, data_);
        write_u32(bytes, 0x03cU, bss_size_);
        copy_bytes(bytes, 0x040U, module_id_);
        write_u32(bytes, 0x060U, text_.stored_size);
        write_u32(bytes, 0x064U, rodata_.stored_size);
        write_u32(bytes, 0x068U, data_.stored_size);
        copy_bytes(bytes, 0x06cU, std::array<std::byte, 0x1cU>{});
        write_u32(bytes, 0x088U, embedded_.offset);
        write_u32(bytes, 0x08cU, embedded_.size);
        write_u32(bytes, 0x090U, dynstr_.offset);
        write_u32(bytes, 0x094U, dynstr_.size);
        write_u32(bytes, 0x098U, dynsym_.offset);
        write_u32(bytes, 0x09cU, dynsym_.size);
        copy_bytes(bytes, 0x0a0U, text_hash_);
        copy_bytes(bytes, 0x0c0U, rodata_hash_);
        copy_bytes(bytes, 0x0e0U, data_hash_);
        return bytes;
    }

    [[nodiscard]] static std::array<std::byte, 32> pattern(unsigned int start)
    {
        std::array<std::byte, 32> result{};
        for (std::size_t index = 0U; index < result.size(); ++index)
        {
            result[index] = static_cast<std::byte>(start + static_cast<unsigned int>(index));
        }
        return result;
    }

  private:
    static void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
    {
        bytes[offset + 0U] = static_cast<std::byte>(value & 0xffU);
        bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
        bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
        bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
    }

    static void write_segment(std::vector<std::byte>& bytes, std::size_t offset,
                              const SegmentValues& segment)
    {
        write_u32(bytes, offset + 0x00U, segment.file_offset);
        write_u32(bytes, offset + 0x04U, segment.memory_offset);
        write_u32(bytes, offset + 0x08U, segment.memory_size);
    }

    template <std::size_t Size>
    static void copy_bytes(std::vector<std::byte>& bytes, std::size_t offset,
                           const std::array<std::byte, Size>& source)
    {
        std::copy(source.begin(), source.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    std::uint32_t version_ = 0U;
    std::uint32_t flags_ = 0U;
    SegmentValues text_{0x100U, 0x1000U, 0x10U, 0x10U};
    SegmentValues rodata_{0x110U, 0x2000U, 0x10U, 0x10U};
    SegmentValues data_{0x120U, 0x3000U, 0x10U, 0x10U};
    std::uint32_t bss_size_ = 0U;
    std::uint32_t module_name_offset_ = 0x130U;
    std::uint32_t module_name_size_ = 0U;
    std::array<std::byte, 32> module_id_;
    std::array<std::byte, 32> text_hash_;
    std::array<std::byte, 32> rodata_hash_;
    std::array<std::byte, 32> data_hash_;
    switchrecomp::format::NsoRelativeRange embedded_{0U, 0U};
    switchrecomp::format::NsoRelativeRange dynstr_{0U, 0U};
    switchrecomp::format::NsoRelativeRange dynsym_{0U, 0U};
};

template <typename T>
void require_error(const switchrecomp::Result<T>& result, switchrecomp::ErrorCode code,
                   std::string_view message_fragment)
{
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == code);
    REQUIRE(result.error().message.find(message_fragment) != std::string::npos);
}

} // namespace

TEST_CASE("valid minimal NSO0 header parses")
{
    const auto result = switchrecomp::format::parse_nso_header(SyntheticNsoBuilder{}.build());

    REQUIRE(result);
    REQUIRE(result.value().version == 0U);
    REQUIRE(result.value().reserved == 0U);
    REQUIRE(result.value().bss_size == 0U);
    REQUIRE(result.value().module_name_offset == 0x130U);
    REQUIRE(result.value().module_name_size == 0U);
    REQUIRE(result.value().text.file_offset == 0x100U);
    REQUIRE(result.value().rodata.file_offset == 0x110U);
    REQUIRE(result.value().data.file_offset == 0x120U);
}

TEST_CASE("NSO0 segment metadata, IDs, and hashes are copied exactly")
{
    const auto module_id = SyntheticNsoBuilder::pattern(0xa0U);
    const auto text_hash = SyntheticNsoBuilder::pattern(0xc0U);
    const auto rodata_hash = SyntheticNsoBuilder::pattern(0xd0U);
    const auto data_hash = SyntheticNsoBuilder::pattern(0xe0U);
    const auto result =
        SyntheticNsoBuilder{}
            .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x140U, 0x4000U, 0x20U, 0x20U)
            .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x160U, 0x5000U, 0x30U,
                         0x30U)
            .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x190U, 0x6000U, 0x40U, 0x40U)
            .set_module_id(module_id)
            .set_hash(switchrecomp::format::NsoSegmentKind::Text, text_hash)
            .set_hash(switchrecomp::format::NsoSegmentKind::RoData, rodata_hash)
            .set_hash(switchrecomp::format::NsoSegmentKind::Data, data_hash)
            .build(0x1d0U);
    const auto parsed = switchrecomp::format::parse_nso_header(result);

    REQUIRE(parsed);
    REQUIRE(parsed.value().text.memory_offset == 0x4000U);
    REQUIRE(parsed.value().text.memory_size == 0x20U);
    REQUIRE(parsed.value().text.stored_size == 0x20U);
    REQUIRE(parsed.value().rodata.memory_offset == 0x5000U);
    REQUIRE(parsed.value().rodata.memory_size == 0x30U);
    REQUIRE(parsed.value().data.memory_offset == 0x6000U);
    REQUIRE(parsed.value().data.memory_size == 0x40U);
    REQUIRE(parsed.value().module_id == module_id);
    REQUIRE(parsed.value().text.hash == text_hash);
    REQUIRE(parsed.value().rodata.hash == rodata_hash);
    REQUIRE(parsed.value().data.hash == data_hash);
    REQUIRE(switchrecomp::format::module_id_hex(parsed.value()).size() == 64U);
    REQUIRE(switchrecomp::format::module_id_hex(parsed.value()).front() == 'a');
}

TEST_CASE("NSO0 flags expose compression, hash, execute-only, and ZBIC semantics")
{
    const auto result = switchrecomp::format::parse_nso_header(
        SyntheticNsoBuilder{}
            .set_flags(0xffU)
            .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x100U, 0x1000U, 0x20U, 0x10U)
            .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x110U, 0x2000U, 0x30U,
                         0x20U)
            .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x130U, 0x3000U, 0x40U, 0x30U)
            .build(0x160U));

    REQUIRE(result);
    REQUIRE(result.value().flags == 0xffU);
    REQUIRE(result.value().execute_only_memory);
    REQUIRE(result.value().use_zbic_compression);
    REQUIRE(result.value().is_compressed(switchrecomp::format::NsoSegmentKind::Text));
    REQUIRE(result.value().is_compressed(switchrecomp::format::NsoSegmentKind::RoData));
    REQUIRE(result.value().is_compressed(switchrecomp::format::NsoSegmentKind::Data));
    REQUIRE(result.value().has_hash(switchrecomp::format::NsoSegmentKind::Text));
    REQUIRE(result.value().has_hash(switchrecomp::format::NsoSegmentKind::RoData));
    REQUIRE(result.value().has_hash(switchrecomp::format::NsoSegmentKind::Data));
    REQUIRE(switchrecomp::format::is_compressed(result.value().text));
    REQUIRE(switchrecomp::format::has_hash(result.value().data));
}

TEST_CASE("valid NSO0 optional ranges, adjacent ranges, and BSS are accepted")
{
    const auto result = switchrecomp::format::parse_nso_header(SyntheticNsoBuilder{}
                                                                   .set_module_name(0x130U, 0x10U)
                                                                   .set_embedded(0x00U, 0x04U)
                                                                   .set_dynstr(0x04U, 0x04U)
                                                                   .set_dynsym(0x08U, 0x08U)
                                                                   .set_bss_size(0x20U)
                                                                   .build(0x140U));

    REQUIRE(result);
    REQUIRE(result.value().embedded.offset == 0x00U);
    REQUIRE(result.value().dynstr.size == 0x04U);
    REQUIRE(result.value().dynsym.offset == 0x08U);
    REQUIRE(result.value().bss_size == 0x20U);
}

TEST_CASE("NSO0 rejects a truncated header and all-zero input")
{
    require_error(switchrecomp::format::parse_nso_header(SyntheticNsoBuilder{}.build(0xffU)),
                  switchrecomp::ErrorCode::OutOfBounds, "requires at least 256 bytes");
    require_error(switchrecomp::format::parse_nso_header(std::vector<std::byte>(0x100U)),
                  switchrecomp::ErrorCode::InvalidFormat, "invalid NSO0 magic");
}

TEST_CASE("NSO0 rejects bad magic, unsupported versions, and unknown flags")
{
    auto bad_magic = SyntheticNsoBuilder{}.build();
    bad_magic[0] = std::byte{'X'};
    require_error(switchrecomp::format::parse_nso_header(bad_magic),
                  switchrecomp::ErrorCode::InvalidFormat, "invalid NSO0 magic");
    require_error(
        switchrecomp::format::parse_nso_header(SyntheticNsoBuilder{}.set_version(2U).build()),
        switchrecomp::ErrorCode::Unsupported, "unsupported NSO0 version 2");
    require_error(
        switchrecomp::format::parse_nso_header(SyntheticNsoBuilder{}.set_flags(0x100U).build()),
        switchrecomp::ErrorCode::Unsupported, "0x00000100");
}

TEST_CASE("NSO0 rejects file ranges outside the input")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x120U, 0x1000U,
                                       0x20U, 0x20U)
                          .build(0x130U)),
                  switchrecomp::ErrorCode::OutOfBounds, "text payload range");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x130U,
                                       0x2000U, 0x20U, 0x20U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::OutOfBounds, "rodata payload range");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x140U, 0x3000U,
                                       0x20U, 0x20U)
                          .build(0x150U)),
                  switchrecomp::ErrorCode::OutOfBounds, "data payload range");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}.set_module_name(0x120U, 0x20U).build(0x130U)),
                  switchrecomp::ErrorCode::OutOfBounds, "module name payload range");
}

TEST_CASE("NSO0 detects file-range arithmetic overflow before slicing")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text,
                                       std::numeric_limits<std::uint32_t>::max(), 0x1000U, 0x10U,
                                       std::numeric_limits<std::uint32_t>::max())
                          .build()),
                  switchrecomp::ErrorCode::OutOfBounds, "text payload range");
}

TEST_CASE("NSO0 requires stored and memory sizes to match for uncompressed segments")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x100U, 0x1000U,
                                       0x20U, 0x10U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::SizeMismatch, "uncompressed text");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x110U,
                                       0x2000U, 0x20U, 0x10U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::SizeMismatch, "uncompressed rodata");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x120U, 0x3000U,
                                       0x20U, 0x10U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::SizeMismatch, "uncompressed data");
}

TEST_CASE("NSO0 rejects overlapping file ranges")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x100U, 0x1000U,
                                       0x20U, 0x20U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x110U,
                                       0x2000U, 0x10U, 0x10U)
                          .build(0x150U)),
                  switchrecomp::ErrorCode::InvalidFormat, "text file range overlaps rodata");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x100U, 0x1000U,
                                       0x20U, 0x20U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x110U, 0x3000U,
                                       0x10U, 0x10U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x140U,
                                       0x2000U, 0x10U, 0x10U)
                          .build(0x160U)),
                  switchrecomp::ErrorCode::InvalidFormat, "text file range overlaps data");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x110U,
                                       0x2000U, 0x20U, 0x20U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x120U, 0x3000U,
                                       0x10U, 0x10U)
                          .build(0x150U)),
                  switchrecomp::ErrorCode::InvalidFormat, "rodata file range overlaps data");
}

TEST_CASE("NSO0 rejects overlapping memory ranges")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x100U, 0x1000U,
                                       0x20U, 0x20U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x120U,
                                       0x1010U, 0x10U, 0x10U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x130U, 0x3000U,
                                       0x10U, 0x10U)
                          .build(0x150U)),
                  switchrecomp::ErrorCode::InvalidFormat, "text memory range overlaps rodata");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x100U, 0x1000U,
                                       0x20U, 0x20U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x120U,
                                       0x2000U, 0x10U, 0x10U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x130U, 0x1010U,
                                       0x10U, 0x10U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::InvalidFormat, "text memory range overlaps data");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::RoData, 0x110U,
                                       0x2000U, 0x20U, 0x20U)
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x130U, 0x2010U,
                                       0x10U, 0x10U)
                          .build(0x150U)),
                  switchrecomp::ErrorCode::InvalidFormat, "rodata memory range overlaps data");
}

TEST_CASE("NSO0 rejects segment and BSS memory overflow")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x100U,
                                       std::numeric_limits<std::uint32_t>::max(), 2U, 2U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::ArithmeticOverflow, "text memory range");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Data, 0x120U,
                                       0xfffffff0U, 0x10U, 0x10U)
                          .set_bss_size(1U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::ArithmeticOverflow, "plus BSS");
}

TEST_CASE("NSO0 rejects non-empty payloads inside the fixed header")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}
                          .set_segment(switchrecomp::format::NsoSegmentKind::Text, 0x80U, 0x1000U,
                                       0x10U, 0x10U)
                          .build(0x140U)),
                  switchrecomp::ErrorCode::InvalidFormat,
                  "starts inside the 0x100-byte NSO0 header");
}

TEST_CASE("NSO0 validates embedded, DynStr, and DynSym ranges against rodata")
{
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}.set_embedded(0x10U, 0x10U).build()),
                  switchrecomp::ErrorCode::OutOfBounds, "embedded range");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}.set_dynstr(0x10U, 0x10U).build()),
                  switchrecomp::ErrorCode::OutOfBounds, "rodata DynStr range");
    require_error(switchrecomp::format::parse_nso_header(
                      SyntheticNsoBuilder{}.set_dynsym(0x10U, 0x10U).build()),
                  switchrecomp::ErrorCode::OutOfBounds, "rodata DynSym range");
}
