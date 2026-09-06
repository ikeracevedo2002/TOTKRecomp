#include "switchrecomp/common/sha256.hpp"
#include "switchrecomp/format/nso.hpp"
#include "switchrecomp/loader/nso_guest_loader.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <lz4.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using switchrecomp::ErrorCode;
using switchrecomp::Result;
using switchrecomp::format::NsoCompressionKind;
using switchrecomp::format::NsoHeader;
using switchrecomp::format::NsoHashStatus;
using switchrecomp::format::NsoImage;
using switchrecomp::format::NsoSegmentKind;

struct SegmentSpec
{
    std::vector<std::byte> bytes;
    bool compressed = false;
    bool hash_required = false;
    std::optional<std::uint32_t> declared_memory_size;
    std::optional<std::uint32_t> declared_stored_size;
    std::array<std::byte, 32> hash{};
    bool hash_overridden = false;
};

class SyntheticNsoImageBuilder
{
  public:
    SyntheticNsoImageBuilder& set_segment(NsoSegmentKind kind, std::vector<std::byte> bytes)
    {
        spec(kind).bytes = std::move(bytes);
        return *this;
    }

    SyntheticNsoImageBuilder& set_compressed(NsoSegmentKind kind, bool value = true)
    {
        spec(kind).compressed = value;
        return *this;
    }

    SyntheticNsoImageBuilder& set_hash_required(NsoSegmentKind kind, bool value = true)
    {
        spec(kind).hash_required = value;
        return *this;
    }

    SyntheticNsoImageBuilder& set_declared_memory_size(NsoSegmentKind kind,
                                                       std::uint32_t value)
    {
        spec(kind).declared_memory_size = value;
        return *this;
    }

    SyntheticNsoImageBuilder& set_declared_stored_size(NsoSegmentKind kind,
                                                       std::uint32_t value)
    {
        spec(kind).declared_stored_size = value;
        return *this;
    }

    SyntheticNsoImageBuilder& set_hash(NsoSegmentKind kind,
                                       const std::array<std::byte, 32>& value)
    {
        spec(kind).hash = value;
        spec(kind).hash_overridden = true;
        return *this;
    }

    SyntheticNsoImageBuilder& set_zbic(bool value = true)
    {
        zbic_ = value;
        return *this;
    }

    SyntheticNsoImageBuilder& set_bss_size(std::uint32_t value)
    {
        bss_size_ = value;
        return *this;
    }

    [[nodiscard]] std::vector<std::byte> build() const
    {
        const std::array<std::vector<std::byte>, 3> payloads{
            encode(text_), encode(rodata_), encode(data_)};
        const std::array<std::size_t, 3> file_offsets{
            switchrecomp::format::nso_header_size,
            switchrecomp::format::nso_header_size + payloads[0].size(),
            switchrecomp::format::nso_header_size + payloads[0].size() + payloads[1].size()};
        const auto file_end = file_offsets[2] + payloads[2].size();
        const auto file_size = std::max<std::size_t>(0x130U, file_end);
        std::vector<std::byte> result(file_size);

        result[0x000U] = std::byte{'N'};
        result[0x001U] = std::byte{'S'};
        result[0x002U] = std::byte{'O'};
        result[0x003U] = std::byte{'0'};
        write_u32(result, 0x004U, 0U);
        write_u32(result, 0x008U, 0U);
        write_u32(result, 0x00cU, flags());
        write_segment(result, 0x010U, text_, file_offsets[0], 0x1000U);
        write_u32(result, 0x01cU, 0x130U);
        write_segment(result, 0x020U, rodata_, file_offsets[1], 0x2000U);
        write_u32(result, 0x02cU, 0U);
        write_segment(result, 0x030U, data_, file_offsets[2], 0x3000U);
        write_u32(result, 0x03cU, bss_size_);
        write_u32(result, 0x060U, stored_size(text_, payloads[0]));
        write_u32(result, 0x064U, stored_size(rodata_, payloads[1]));
        write_u32(result, 0x068U, stored_size(data_, payloads[2]));
        write_u32(result, 0x088U, 0U);
        write_u32(result, 0x08cU, 0U);
        write_u32(result, 0x090U, 0U);
        write_u32(result, 0x094U, 0U);
        write_u32(result, 0x098U, 0U);
        write_u32(result, 0x09cU, 0U);
        copy_hash(result, 0x0a0U, text_);
        copy_hash(result, 0x0c0U, rodata_);
        copy_hash(result, 0x0e0U, data_);

        copy_payload(result, file_offsets[0], payloads[0]);
        copy_payload(result, file_offsets[1], payloads[1]);
        copy_payload(result, file_offsets[2], payloads[2]);
        return result;
    }

    [[nodiscard]] static std::vector<std::byte> pattern(std::size_t size, unsigned int seed)
    {
        std::vector<std::byte> result(size);
        for (std::size_t index = 0U; index < result.size(); ++index)
        {
            result[index] = static_cast<std::byte>((seed + index) & 0xffU);
        }
        return result;
    }

  private:
    [[nodiscard]] SegmentSpec& spec(NsoSegmentKind kind)
    {
        switch (kind)
        {
        case NsoSegmentKind::Text:
            return text_;
        case NsoSegmentKind::RoData:
            return rodata_;
        case NsoSegmentKind::Data:
            return data_;
        }
        return text_;
    }

    [[nodiscard]] static std::uint32_t as_u32(std::size_t value)
    {
        if (value > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error("synthetic fixture value does not fit in uint32_t");
        }
        return static_cast<std::uint32_t>(value);
    }

    [[nodiscard]] static std::vector<std::byte> encode(const SegmentSpec& segment)
    {
        if (!segment.compressed || segment.bytes.empty())
        {
            return segment.bytes;
        }
        const auto input_size = as_u32(segment.bytes.size());
        if (input_size > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        {
            throw std::runtime_error("synthetic LZ4 input is too large");
        }
        const int bound = LZ4_compressBound(static_cast<int>(input_size));
        if (bound <= 0)
        {
            throw std::runtime_error("LZ4_compressBound failed for synthetic fixture");
        }
        std::vector<std::byte> result(static_cast<std::size_t>(bound));
        const int compressed = LZ4_compress_default(
            reinterpret_cast<const char*>(segment.bytes.data()),
            reinterpret_cast<char*>(result.data()), static_cast<int>(input_size), bound);
        if (compressed <= 0)
        {
            throw std::runtime_error("LZ4_compress_default failed for synthetic fixture");
        }
        result.resize(static_cast<std::size_t>(compressed));
        return result;
    }

    [[nodiscard]] std::uint32_t flags() const noexcept
    {
        std::uint32_t result = zbic_ ? (1U << 7U) : 0U;
        if (text_.compressed)
        {
            result |= 1U << 0U;
        }
        if (rodata_.compressed)
        {
            result |= 1U << 1U;
        }
        if (data_.compressed)
        {
            result |= 1U << 2U;
        }
        if (text_.hash_required)
        {
            result |= 1U << 3U;
        }
        if (rodata_.hash_required)
        {
            result |= 1U << 4U;
        }
        if (data_.hash_required)
        {
            result |= 1U << 5U;
        }
        return result;
    }

    [[nodiscard]] static std::uint32_t memory_size(const SegmentSpec& segment)
    {
        return segment.declared_memory_size.value_or(as_u32(segment.bytes.size()));
    }

    [[nodiscard]] static std::uint32_t stored_size(const SegmentSpec& segment,
                                                   const std::vector<std::byte>& payload)
    {
        return segment.declared_stored_size.value_or(as_u32(payload.size()));
    }

    static void write_u32(std::vector<std::byte>& bytes, std::size_t offset,
                          std::uint32_t value)
    {
        bytes[offset + 0U] = static_cast<std::byte>(value & 0xffU);
        bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
        bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
        bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
    }

    static void write_segment(std::vector<std::byte>& bytes, std::size_t offset,
                              const SegmentSpec& segment, std::size_t file_offset,
                              std::uint32_t memory_offset)
    {
        write_u32(bytes, offset + 0x00U, as_u32(file_offset));
        write_u32(bytes, offset + 0x04U, memory_offset);
        write_u32(bytes, offset + 0x08U, memory_size(segment));
    }

    static void copy_hash(std::vector<std::byte>& bytes, std::size_t offset,
                          const SegmentSpec& segment)
    {
        std::array<std::byte, 32> hash = segment.hash;
        if (!segment.hash_overridden)
        {
            const auto digest = switchrecomp::sha256_bytes(segment.bytes);
            if (!digest)
            {
                throw std::runtime_error("SHA-256 failed for synthetic fixture");
            }
            hash = digest.value().bytes;
        }
        std::copy(hash.begin(), hash.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    static void copy_payload(std::vector<std::byte>& bytes, std::size_t offset,
                             const std::vector<std::byte>& payload)
    {
        std::copy(payload.begin(), payload.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    SegmentSpec text_;
    SegmentSpec rodata_;
    SegmentSpec data_;
    bool zbic_ = false;
    std::uint32_t bss_size_ = 0U;
};

template <typename T>
void require_error(const Result<T>& result, ErrorCode code, std::string_view message_fragment)
{
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == code);
    REQUIRE(result.error().message.find(message_fragment) != std::string::npos);
}

struct ParsedFixture
{
    std::vector<std::byte> bytes;
    NsoHeader header;
};

[[nodiscard]] ParsedFixture parse_fixture(SyntheticNsoImageBuilder builder)
{
    auto bytes = builder.build();
    const auto header = switchrecomp::format::parse_nso_header(bytes);
    if (!header)
    {
        throw std::runtime_error(header.error().message);
    }
    return ParsedFixture{std::move(bytes), header.value()};
}

[[nodiscard]] NsoImage materialize_fixture(SyntheticNsoImageBuilder builder)
{
    auto fixture = parse_fixture(std::move(builder));
    const auto image = switchrecomp::format::materialize_nso(fixture.bytes, fixture.header);
    if (!image)
    {
        throw std::runtime_error(image.error().message);
    }
    return image.value();
}

} // namespace

TEST_CASE("NSO materialization owns uncompressed sections and zero-fills BSS")
{
    const auto text = SyntheticNsoImageBuilder::pattern(17U, 0x10U);
    const auto rodata = SyntheticNsoImageBuilder::pattern(11U, 0x50U);
    const auto data = SyntheticNsoImageBuilder::pattern(13U, 0x90U);
    auto fixture = parse_fixture(SyntheticNsoImageBuilder{}
                                     .set_segment(NsoSegmentKind::Text, text)
                                     .set_segment(NsoSegmentKind::RoData, rodata)
                                     .set_segment(NsoSegmentKind::Data, data)
                                     .set_bss_size(19U));
    const auto image = switchrecomp::format::materialize_nso(fixture.bytes, fixture.header);

    REQUIRE(image);
    REQUIRE(image.value().text.bytes == text);
    REQUIRE(image.value().rodata.bytes == rodata);
    REQUIRE(image.value().data.bytes == data);
    REQUIRE(image.value().text.bytes.size() == text.size());
    REQUIRE(image.value().rodata.bytes.size() == rodata.size());
    REQUIRE(image.value().data.bytes.size() == data.size());
    REQUIRE(image.value().text.memory_offset == 0x1000U);
    REQUIRE(image.value().rodata.memory_offset == 0x2000U);
    REQUIRE(image.value().data.memory_offset == 0x3000U);
    REQUIRE(image.value().bss_memory_offset == 0x3000U + data.size());
    REQUIRE(image.value().bss.size() == 19U);
    REQUIRE(std::all_of(image.value().bss.begin(), image.value().bss.end(),
                        [](std::byte value) { return value == std::byte{0}; }));
    REQUIRE(image.value().text.compression == NsoCompressionKind::None);
    REQUIRE(image.value().rodata.compression == NsoCompressionKind::None);
    REQUIRE(image.value().data.compression == NsoCompressionKind::None);
    REQUIRE(image.value().text.hash_status == NsoHashStatus::NotRequired);
    REQUIRE(image.value().rodata.hash_status == NsoHashStatus::NotRequired);
    REQUIRE(image.value().data.hash_status == NsoHashStatus::NotRequired);

    fixture.bytes[fixture.header.text.file_offset] = std::byte{0xff};
    REQUIRE(image.value().text.bytes.front() == text.front());
}

TEST_CASE("Each NSO segment supports safe LZ4 materialization")
{
    const std::array<NsoSegmentKind, 3> kinds{NsoSegmentKind::Text, NsoSegmentKind::RoData,
                                               NsoSegmentKind::Data};
    for (const auto kind : kinds)
    {
        const auto expected = SyntheticNsoImageBuilder::pattern(257U, 0x20U);
        const auto image = materialize_fixture(
            SyntheticNsoImageBuilder{}.set_segment(kind, expected).set_compressed(kind));
        const auto& materialized = kind == NsoSegmentKind::Text
                                       ? image.text
                                       : (kind == NsoSegmentKind::RoData ? image.rodata : image.data);
        REQUIRE(materialized.bytes == expected);
        REQUIRE(materialized.compression == NsoCompressionKind::Lz4);
        REQUIRE(materialized.hash_status == NsoHashStatus::NotRequired);
    }
}

TEST_CASE("NSO materialization supports mixed and all-compressed images")
{
    const auto text = SyntheticNsoImageBuilder::pattern(80U, 0x11U);
    const auto rodata = SyntheticNsoImageBuilder::pattern(96U, 0x22U);
    const auto data = SyntheticNsoImageBuilder::pattern(112U, 0x33U);
    const auto mixed = materialize_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, text)
            .set_segment(NsoSegmentKind::RoData, rodata)
            .set_segment(NsoSegmentKind::Data, data)
            .set_compressed(NsoSegmentKind::Text)
            .set_compressed(NsoSegmentKind::Data));
    REQUIRE(mixed.text.bytes == text);
    REQUIRE(mixed.rodata.bytes == rodata);
    REQUIRE(mixed.data.bytes == data);
    REQUIRE(mixed.text.compression == NsoCompressionKind::Lz4);
    REQUIRE(mixed.rodata.compression == NsoCompressionKind::None);
    REQUIRE(mixed.data.compression == NsoCompressionKind::Lz4);

    const auto all = materialize_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, text)
            .set_segment(NsoSegmentKind::RoData, rodata)
            .set_segment(NsoSegmentKind::Data, data)
            .set_compressed(NsoSegmentKind::Text)
            .set_compressed(NsoSegmentKind::RoData)
            .set_compressed(NsoSegmentKind::Data));
    REQUIRE(all.text.bytes == text);
    REQUIRE(all.rodata.bytes == rodata);
    REQUIRE(all.data.bytes == data);
    REQUIRE(all.text.compression == NsoCompressionKind::Lz4);
    REQUIRE(all.rodata.compression == NsoCompressionKind::Lz4);
    REQUIRE(all.data.compression == NsoCompressionKind::Lz4);
}

TEST_CASE("Requested NSO hashes verify materialized bytes, including after LZ4")
{
    const auto text = SyntheticNsoImageBuilder::pattern(300U, 0x41U);
    const auto rodata = SyntheticNsoImageBuilder::pattern(301U, 0x42U);
    const auto data = SyntheticNsoImageBuilder::pattern(302U, 0x43U);
    const auto image = materialize_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, text)
            .set_segment(NsoSegmentKind::RoData, rodata)
            .set_segment(NsoSegmentKind::Data, data)
            .set_compressed(NsoSegmentKind::Text)
            .set_compressed(NsoSegmentKind::RoData)
            .set_compressed(NsoSegmentKind::Data)
            .set_hash_required(NsoSegmentKind::Text)
            .set_hash_required(NsoSegmentKind::RoData)
            .set_hash_required(NsoSegmentKind::Data));

    REQUIRE(image.text.hash_status == NsoHashStatus::Verified);
    REQUIRE(image.rodata.hash_status == NsoHashStatus::Verified);
    REQUIRE(image.data.hash_status == NsoHashStatus::Verified);
}

TEST_CASE("A requested hash mismatch fails for the offending segment")
{
    const std::array<NsoSegmentKind, 3> kinds{NsoSegmentKind::Text, NsoSegmentKind::RoData,
                                               NsoSegmentKind::Data};
    for (const auto kind : kinds)
    {
        auto wrong_hash = std::array<std::byte, 32>{};
        wrong_hash.fill(std::byte{0xa5});
        const auto fixture = parse_fixture(
            SyntheticNsoImageBuilder{}
                .set_segment(kind, SyntheticNsoImageBuilder::pattern(64U, 0x77U))
                .set_hash_required(kind)
                .set_hash(kind, wrong_hash));
        const auto result = switchrecomp::format::materialize_nso(fixture.bytes, fixture.header);
        require_error(result, ErrorCode::HashMismatch,
                      kind == NsoSegmentKind::Text
                          ? ".text"
                          : (kind == NsoSegmentKind::RoData ? ".rodata" : ".data"));
    }
}

TEST_CASE("Corrupt and truncated LZ4 sources fail without partial image success")
{
    auto fixture = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(512U, 0x12U))
            .set_compressed(NsoSegmentKind::Text));
    fixture.bytes[fixture.header.text.file_offset] = std::byte{0};
    const auto corrupt = switchrecomp::format::materialize_nso(fixture.bytes, fixture.header);
    require_error(corrupt, ErrorCode::DecompressionFailed, "failed to materialize .text");

    auto truncated_stream = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(512U, 0x34U))
            .set_compressed(NsoSegmentKind::Text));
    REQUIRE(truncated_stream.header.text.stored_size > 1U);
    truncated_stream.header.text.stored_size -= 1U;
    const auto truncated =
        switchrecomp::format::materialize_nso(truncated_stream.bytes, truncated_stream.header);
    REQUIRE_FALSE(truncated);
    REQUIRE(truncated.error().message.find(".text") != std::string::npos);
}

TEST_CASE("Materialization rechecks stored source bounds")
{
    auto fixture = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(32U, 0x45U)));
    REQUIRE(fixture.header.text.stored_size > 0U);
    fixture.bytes.resize(static_cast<std::size_t>(fixture.header.text.file_offset) +
                         fixture.header.text.stored_size - 1U);
    const auto result = switchrecomp::format::materialize_nso(fixture.bytes, fixture.header);
    require_error(result, ErrorCode::OutOfBounds, "stored source range");
}

TEST_CASE("LZ4 decompressed size must exactly match declared memory size")
{
    auto larger_declaration = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(128U, 0x55U))
            .set_compressed(NsoSegmentKind::Text)
            .set_declared_memory_size(NsoSegmentKind::Text, 129U));
    const auto result =
        switchrecomp::format::materialize_nso(larger_declaration.bytes, larger_declaration.header);
    require_error(result, ErrorCode::SizeMismatch, "expected 0x81 decompressed bytes");
}

TEST_CASE("Uncompressed stored and memory sizes must match before copying")
{
    const auto bytes = SyntheticNsoImageBuilder::pattern(32U, 0x61U);
    const auto input = SyntheticNsoImageBuilder{}
                           .set_segment(NsoSegmentKind::RoData, bytes)
                           .set_declared_stored_size(NsoSegmentKind::RoData, 31U)
                           .build();
    const auto parsed = switchrecomp::format::parse_nso_header(input);
    require_error(parsed, ErrorCode::SizeMismatch, "uncompressed rodata");
}

TEST_CASE("ZBIC is reported as explicitly unsupported and never falls back to LZ4")
{
    const auto fixture = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(128U, 0x71U))
            .set_compressed(NsoSegmentKind::Text)
            .set_zbic());
    const auto result = switchrecomp::format::materialize_nso(fixture.bytes, fixture.header);
    require_error(result, ErrorCode::UnsupportedCompression, ".text");
    REQUIRE(result.error().message.find("ZBIC") != std::string::npos);
}

TEST_CASE("Materialization limits reject oversized segments before allocation")
{
    const auto fixture = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(32U, 0x81U)));
    const switchrecomp::format::NsoMaterializationLimits limits{31U, 1024U};
    const auto result =
        switchrecomp::format::materialize_nso(fixture.bytes, fixture.header, limits);
    require_error(result, ErrorCode::ResourceLimit, ".text");
}

TEST_CASE("Materialization limits include BSS and reject oversized totals")
{
    const auto fixture = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(8U, 0x91U))
            .set_segment(NsoSegmentKind::RoData, SyntheticNsoImageBuilder::pattern(8U, 0x92U))
            .set_segment(NsoSegmentKind::Data, SyntheticNsoImageBuilder::pattern(8U, 0x93U))
            .set_bss_size(8U));
    const switchrecomp::format::NsoMaterializationLimits exact{8U, 32U};
    REQUIRE(switchrecomp::format::materialize_nso(fixture.bytes, fixture.header, exact));

    const switchrecomp::format::NsoMaterializationLimits over{8U, 31U};
    const auto result =
        switchrecomp::format::materialize_nso(fixture.bytes, fixture.header, over);
    require_error(result, ErrorCode::ResourceLimit, "total materialized NSO image size");
}

TEST_CASE("Zero-sized uncompressed sections and BSS are deterministic")
{
    const auto fixture = parse_fixture(SyntheticNsoImageBuilder{}.set_bss_size(0U));
    const auto image = switchrecomp::format::materialize_nso(fixture.bytes, fixture.header);
    REQUIRE(image);
    REQUIRE(image.value().text.bytes.empty());
    REQUIRE(image.value().rodata.bytes.empty());
    REQUIRE(image.value().data.bytes.empty());
    REQUIRE(image.value().bss.empty());
    REQUIRE(image.value().text.compression == NsoCompressionKind::None);
    REQUIRE(image.value().text.hash_status == NsoHashStatus::NotRequired);
}

TEST_CASE("A zero-sized compressed section accepts an empty stored range only")
{
    const auto valid = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_compressed(NsoSegmentKind::Text)
            .set_segment(NsoSegmentKind::Text, {}));
    REQUIRE(switchrecomp::format::materialize_nso(valid.bytes, valid.header));

    const auto invalid = parse_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, {std::byte{0x00}})
            .set_compressed(NsoSegmentKind::Text)
            .set_declared_memory_size(NsoSegmentKind::Text, 0U));
    const auto result = switchrecomp::format::materialize_nso(invalid.bytes, invalid.header);
    require_error(result, ErrorCode::DecompressionFailed, "declared decompressed size is zero");
}

TEST_CASE("A materialized NSO loads into checked guest memory with expected permissions")
{
    const auto text = SyntheticNsoImageBuilder::pattern(17U, 0x10U);
    const auto rodata = SyntheticNsoImageBuilder::pattern(11U, 0x50U);
    const auto data = SyntheticNsoImageBuilder::pattern(13U, 0x90U);
    const auto image = materialize_fixture(SyntheticNsoImageBuilder{}
                                               .set_segment(NsoSegmentKind::Text, text)
                                               .set_segment(NsoSegmentKind::RoData, rodata)
                                               .set_segment(NsoSegmentKind::Data, data)
                                               .set_compressed(NsoSegmentKind::Text)
                                               .set_bss_size(19U));

    switchrecomp::memory::GuestMemory memory;
    const auto loaded = switchrecomp::loader::load_nso(
        image, memory, switchrecomp::loader::NsoGuestLoadOptions{0x10000000U});
    REQUIRE(loaded);
    REQUIRE(memory.region_count() == 4U);

    const auto module_base = 0x10000000ULL;
    const auto text_address = module_base + image.text.memory_offset;
    const auto rodata_address = module_base + image.rodata.memory_offset;
    const auto data_address = module_base + image.data.memory_offset;
    const auto bss_address = module_base + image.bss_memory_offset;

    std::vector<std::byte> actual_text(text.size());
    std::vector<std::byte> actual_rodata(rodata.size());
    std::vector<std::byte> actual_data(data.size());
    std::vector<std::byte> actual_bss(image.bss.size());
    REQUIRE(memory.read(text_address, actual_text));
    REQUIRE(memory.read(rodata_address, actual_rodata));
    REQUIRE(memory.read(data_address, actual_data));
    REQUIRE(memory.read(bss_address, actual_bss));
    REQUIRE(actual_text == text);
    REQUIRE(actual_rodata == rodata);
    REQUIRE(actual_data == data);
    REQUIRE(std::all_of(actual_bss.begin(), actual_bss.end(),
                        [](std::byte value) { return value == std::byte{0}; }));

    REQUIRE(memory.permissions_at(text_address).value() ==
            (switchrecomp::memory::GuestMemoryPermissions::Read |
             switchrecomp::memory::GuestMemoryPermissions::Execute));
    REQUIRE(memory.permissions_at(rodata_address).value() ==
            switchrecomp::memory::GuestMemoryPermissions::Read);
    REQUIRE(memory.permissions_at(data_address).value() ==
            (switchrecomp::memory::GuestMemoryPermissions::Read |
             switchrecomp::memory::GuestMemoryPermissions::Write));
    REQUIRE(memory.permissions_at(bss_address).value() ==
            (switchrecomp::memory::GuestMemoryPermissions::Read |
             switchrecomp::memory::GuestMemoryPermissions::Write));
    REQUIRE(memory.is_executable(text_address).value());
    REQUIRE_FALSE(memory.is_executable(rodata_address).value());
    REQUIRE_FALSE(memory.is_executable(data_address).value());
    REQUIRE_FALSE(memory.is_executable(bss_address).value());

    const auto patch = std::array<std::byte, 1>{std::byte{0xa5}};
    REQUIRE(memory.write(data_address, patch));
    REQUIRE(memory.write(bss_address, patch));
    REQUIRE_FALSE(memory.write(text_address, patch));
    REQUIRE_FALSE(memory.write(rodata_address, patch));
}

TEST_CASE("NSO loader supports a zero module base and high rebased addresses")
{
    const auto image = materialize_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(4U, 0x01U))
            .set_segment(NsoSegmentKind::Data, SyntheticNsoImageBuilder::pattern(4U, 0x11U))
            .set_bss_size(4U));

    switchrecomp::memory::GuestMemory zero_base;
    REQUIRE(switchrecomp::loader::load_nso(image, zero_base,
                                           switchrecomp::loader::NsoGuestLoadOptions{0U}));
    REQUIRE(zero_base.region_at(image.text.memory_offset));
    REQUIRE(zero_base.region_at(image.bss_memory_offset));

    constexpr std::uint64_t high_base = 0x7100000000ULL;
    switchrecomp::memory::GuestMemory high_base_memory;
    REQUIRE(switchrecomp::loader::load_nso(image, high_base_memory,
                                           switchrecomp::loader::NsoGuestLoadOptions{high_base}));
    REQUIRE(high_base_memory.region_at(high_base + image.text.memory_offset));
    REQUIRE(high_base_memory.region_at(high_base + image.bss_memory_offset));
}

TEST_CASE("NSO loader rejects inconsistent guest metadata and preserves state on failure")
{
    auto image = materialize_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(8U, 0x21U))
            .set_segment(NsoSegmentKind::RoData, SyntheticNsoImageBuilder::pattern(8U, 0x31U))
            .set_segment(NsoSegmentKind::Data, SyntheticNsoImageBuilder::pattern(8U, 0x41U))
            .set_bss_size(8U));

    image.header.rodata.memory_offset = image.header.text.memory_offset + 1U;
    image.rodata.memory_offset = image.header.rodata.memory_offset;
    switchrecomp::memory::GuestMemory overlap_memory;
    const auto overlap = switchrecomp::loader::load_nso(image, overlap_memory);
    require_error(overlap, ErrorCode::InvalidArgument, "overlaps");
    REQUIRE(overlap_memory.region_count() == 0U);

    const auto valid_image = materialize_fixture(
        SyntheticNsoImageBuilder{}
            .set_segment(NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(8U, 0x51U))
            .set_segment(NsoSegmentKind::RoData, SyntheticNsoImageBuilder::pattern(8U, 0x61U))
            .set_segment(NsoSegmentKind::Data, SyntheticNsoImageBuilder::pattern(8U, 0x71U))
            .set_bss_size(8U));
    switchrecomp::memory::GuestMemory atomic_memory;
    const auto original = std::array<std::byte, 4>{std::byte{0xca}, std::byte{0xfe},
                                                   std::byte{0xba}, std::byte{0xbe}};
    REQUIRE(atomic_memory.map(0x3000U, original, switchrecomp::memory::GuestMemoryPermissions::Read,
                              "existing"));
    const auto failed = switchrecomp::loader::load_nso(valid_image, atomic_memory);
    require_error(failed, ErrorCode::InvalidArgument, "overlaps");
    REQUIRE(atomic_memory.region_count() == 1U);
    std::array<std::byte, 4> preserved{};
    REQUIRE(atomic_memory.read(0x3000U, preserved));
    REQUIRE(preserved == original);
}

TEST_CASE("NSO loader does not create phantom mappings for empty segments")
{
    const auto image = materialize_fixture(SyntheticNsoImageBuilder{}.set_bss_size(8U));
    switchrecomp::memory::GuestMemory memory;
    REQUIRE(switchrecomp::loader::load_nso(image, memory));
    REQUIRE(memory.region_count() == 1U);
    const auto bss = memory.region_at(image.bss_memory_offset);
    REQUIRE(bss);
    REQUIRE(bss.value().kind == switchrecomp::memory::GuestRegionKind::Bss);
    REQUIRE(bss.value().size == image.bss.size());
}

TEST_CASE("NSO loader rejects module-base address overflow")
{
    const auto image = materialize_fixture(SyntheticNsoImageBuilder{}.set_segment(
        NsoSegmentKind::Text, SyntheticNsoImageBuilder::pattern(4U, 0x81U)));
    switchrecomp::memory::GuestMemory memory;
    const auto result = switchrecomp::loader::load_nso(
        image, memory,
        switchrecomp::loader::NsoGuestLoadOptions{std::numeric_limits<std::uint64_t>::max()});
    require_error(result, ErrorCode::ArithmeticOverflow, "module base plus segment offset");
    REQUIRE(memory.region_count() == 0U);
}
