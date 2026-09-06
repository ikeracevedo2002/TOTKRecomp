#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/format/elf_dynamic.hpp"
#include "switchrecomp/format/elf_rela.hpp"
#include "switchrecomp/format/mod0.hpp"
#include "switchrecomp/format/module_metadata.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using switchrecomp::ErrorCode;
using switchrecomp::Result;
using switchrecomp::format::DynamicInfo;
using switchrecomp::format::DynamicParseLimits;
using switchrecomp::format::DynamicTag;
using switchrecomp::format::Mod0ParseOptions;
using switchrecomp::format::ModuleMetadataParseOptions;
using switchrecomp::memory::GuestAddress;
using switchrecomp::memory::GuestMemory;
using switchrecomp::memory::GuestMemoryPermissions;

template <typename T> void require_error(const Result<T>& result, ErrorCode code)
{
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == code);
}

void put_u32(GuestMemory& memory, GuestAddress address, std::uint32_t value)
{
    std::array<std::byte, sizeof(value)> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    REQUIRE(memory.write(address, bytes));
}

void put_i32(GuestMemory& memory, GuestAddress address, std::int32_t value)
{
    put_u32(memory, address, std::bit_cast<std::uint32_t>(value));
}

void put_u64(GuestMemory& memory, GuestAddress address, std::uint64_t value)
{
    std::array<std::byte, sizeof(value)> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    REQUIRE(memory.write(address, bytes));
}

void put_bytes(GuestMemory& memory, GuestAddress address, std::span<const std::byte> bytes)
{
    REQUIRE(memory.write(address, bytes));
}

struct Fixture
{
    static constexpr GuestAddress base = 0x1000U;
    GuestMemory memory;
    GuestAddress mod0_address;
    GuestAddress dynamic_address;

    explicit Fixture(GuestAddress mod0_offset = 0x100U, GuestAddress dynamic_offset = 0x200U)
        : memory(), mod0_address(base + mod0_offset), dynamic_address(base + dynamic_offset)
    {
        std::vector<std::byte> bytes(0x5000U);
        const auto mapped = memory.map(base, bytes,
                                       GuestMemoryPermissions::Read |
                                           GuestMemoryPermissions::Write,
                                       "synthetic module");
        if (!mapped)
        {
            throw std::runtime_error(mapped.error().message);
        }

        put_u32(memory, base, 0U); // ModuleStart.reserved.
        put_u32(memory, base + 4U, static_cast<std::uint32_t>(mod0_offset));
        put_u32(memory, mod0_address, switchrecomp::format::mod0_magic);
        put_i32(memory, mod0_address + 0x04U,
                static_cast<std::int32_t>(dynamic_offset) - static_cast<std::int32_t>(mod0_offset));
        put_i32(memory, mod0_address + 0x08U,
                static_cast<std::int32_t>(0x700U) - static_cast<std::int32_t>(mod0_offset));
        put_i32(memory, mod0_address + 0x0cU,
                static_cast<std::int32_t>(0x800U) - static_cast<std::int32_t>(mod0_offset));
        put_i32(memory, mod0_address + 0x10U,
                static_cast<std::int32_t>(0x900U) - static_cast<std::int32_t>(mod0_offset));
        put_i32(memory, mod0_address + 0x14U,
                static_cast<std::int32_t>(0x920U) - static_cast<std::int32_t>(mod0_offset));
        put_i32(memory, mod0_address + 0x18U,
                static_cast<std::int32_t>(0xa00U) - static_cast<std::int32_t>(mod0_offset));

        const std::array<std::byte, 4> strings{
            std::byte{0}, std::byte{'x'}, std::byte{0}, std::byte{0}};
        put_bytes(memory, base + 0xc00U, strings);

        const std::vector<std::pair<std::int64_t, std::uint64_t>> entries{
            {static_cast<std::int64_t>(DynamicTag::DT_NEEDED), 1U},
            {static_cast<std::int64_t>(DynamicTag::DT_STRTAB), 0xc00U},
            {static_cast<std::int64_t>(DynamicTag::DT_STRSZ), strings.size()},
            {static_cast<std::int64_t>(DynamicTag::DT_SYMTAB), 0xd00U},
            {static_cast<std::int64_t>(DynamicTag::DT_SYMENT),
             switchrecomp::format::elf64_sym_size},
            {static_cast<std::int64_t>(DynamicTag::DT_RELA), 0xe00U},
            {static_cast<std::int64_t>(DynamicTag::DT_RELASZ), 2U * 24U},
            {static_cast<std::int64_t>(DynamicTag::DT_RELAENT),
             switchrecomp::format::elf64_rela_size},
            {static_cast<std::int64_t>(DynamicTag::DT_JMPREL), 0xf00U},
            {static_cast<std::int64_t>(DynamicTag::DT_PLTRELSZ), 24U},
            {static_cast<std::int64_t>(DynamicTag::DT_PLTREL),
             static_cast<std::uint64_t>(DynamicTag::DT_RELA)},
            {0x70000000, 0x1234U},
            {static_cast<std::int64_t>(DynamicTag::DT_NULL), 0U},
        };
        for (std::size_t index = 0U; index < entries.size(); ++index)
        {
            put_u64(memory, dynamic_address + index * 16U,
                    std::bit_cast<std::uint64_t>(entries[index].first));
            put_u64(memory, dynamic_address + index * 16U + 8U, entries[index].second);
        }

        put_rela(base + 0xe00U, 0x1100U, (std::uint64_t{3U} << 32U) | 0x101U, -7);
        put_rela(base + 0xe18U, 0x1120U, (std::uint64_t{4U} << 32U) | 0x102U, 11);
        put_rela(base + 0xf00U, 0x1130U, (std::uint64_t{5U} << 32U) | 0x103U, 2);
    }

    void put_rela(GuestAddress address, std::uint64_t offset, std::uint64_t info,
                  std::int64_t addend)
    {
        put_u64(memory, address, offset);
        put_u64(memory, address + 8U, info);
        put_u64(memory, address + 16U, std::bit_cast<std::uint64_t>(addend));
    }
};

} // namespace

TEST_CASE("MOD0 discovery and dynamic metadata resolve the loaded guest image")
{
    Fixture fixture;
    const auto discovered = switchrecomp::format::discover_mod0(fixture.memory, Fixture::base);
    REQUIRE(discovered);
    REQUIRE(discovered.value().has_value());
    REQUIRE(discovered.value().value() == fixture.mod0_address);

    const auto metadata = switchrecomp::format::parse_module_metadata(
        fixture.memory, Fixture::base);
    REQUIRE(metadata);
    REQUIRE(metadata.value().mod0.has_value());
    REQUIRE(metadata.value().dynamic.has_value());
    REQUIRE(metadata.value().mod0->dynamic_address == fixture.dynamic_address);
    REQUIRE(metadata.value().mod0->bss_start_address == Fixture::base + 0x700U);
    REQUIRE(metadata.value().mod0->bss_end_address == Fixture::base + 0x800U);
    REQUIRE(metadata.value().mod0->module_object_address == Fixture::base + 0xa00U);

    const auto& dynamic = metadata.value().dynamic.value();
    REQUIRE(dynamic.entry_count == 13U);
    REQUIRE(dynamic.needed == std::vector<std::uint64_t>{1U});
    REQUIRE(dynamic.strtab->module_offset == 0xc00U);
    REQUIRE(dynamic.strtab->address == Fixture::base + 0xc00U);
    REQUIRE(dynamic.symtab->address == Fixture::base + 0xd00U);
    REQUIRE(dynamic.rela_count == 2U);
    REQUIRE(dynamic.jmprel_count == 1U);
    REQUIRE(dynamic.entries.back().tag == static_cast<std::int64_t>(DynamicTag::DT_NULL));
}

TEST_CASE("MOD0 signed relative offsets support negative values")
{
    Fixture fixture(0x400U, 0x200U);
    const auto mod0 = switchrecomp::format::parse_mod0(fixture.memory, fixture.mod0_address);
    REQUIRE(mod0);
    REQUIRE(mod0.value().dynamic_offset == -0x200);
    REQUIRE(mod0.value().dynamic_address == fixture.dynamic_address);

    const auto metadata = switchrecomp::format::parse_module_metadata(
        fixture.memory, Fixture::base);
    REQUIRE(metadata);
    REQUIRE(metadata.value().dynamic->address == fixture.dynamic_address);
}

TEST_CASE("A valid guest image can omit MOD0 without becoming a parse error")
{
    GuestMemory memory;
    std::vector<std::byte> bytes(0x100U);
    REQUIRE(memory.map(Fixture::base, bytes, GuestMemoryPermissions::Read, "text"));

    const auto discovered = switchrecomp::format::discover_mod0(memory, Fixture::base);
    REQUIRE(discovered);
    REQUIRE_FALSE(discovered.value().has_value());
    const auto metadata = switchrecomp::format::parse_module_metadata(memory, Fixture::base);
    REQUIRE(metadata);
    REQUIRE_FALSE(metadata.value().mod0.has_value());
    REQUIRE_FALSE(metadata.value().dynamic.has_value());
}

TEST_CASE("MOD0 magic mismatch is reported as a structured format error")
{
    Fixture fixture;
    put_u32(fixture.memory, fixture.mod0_address, 0xdeadbeefU);
    require_error(switchrecomp::format::discover_mod0(fixture.memory, Fixture::base),
                  ErrorCode::InvalidFormat);
    require_error(switchrecomp::format::parse_mod0(fixture.memory, fixture.mod0_address),
                  ErrorCode::InvalidFormat);
}

TEST_CASE("MOD0 truncation and cross-region headers are rejected")
{
    GuestMemory memory;
    std::vector<std::byte> bytes(0x10U);
    REQUIRE(memory.map(0x2000U, bytes, GuestMemoryPermissions::Read, "truncated MOD0"));
    require_error(switchrecomp::format::parse_mod0(memory, 0x2000U), ErrorCode::UnmappedMemory);

    GuestMemory split;
    std::vector<std::byte> first(0x10U);
    std::vector<std::byte> second(0x10U);
    REQUIRE(split.map(0x3000U, first, GuestMemoryPermissions::Read, "MOD0 first"));
    REQUIRE(split.map(0x3010U, second, GuestMemoryPermissions::Read, "MOD0 second"));
    require_error(switchrecomp::format::parse_mod0(split, 0x3000U), ErrorCode::UnmappedMemory);
}

TEST_CASE("MOD0 signed offset underflow and overflow are checked")
{
    GuestMemory underflow;
    std::vector<std::byte> bytes(switchrecomp::format::mod0_header_size);
    REQUIRE(underflow.map(0x2000U, bytes,
                          GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                          "underflow MOD0"));
    put_u32(underflow, 0x2000U, switchrecomp::format::mod0_magic);
    put_i32(underflow, 0x2004U, std::numeric_limits<std::int32_t>::min());
    require_error(switchrecomp::format::parse_mod0(underflow, 0x2000U),
                  ErrorCode::ArithmeticUnderflow);

    GuestMemory overflow;
    constexpr GuestAddress high_base = std::numeric_limits<GuestAddress>::max() - 0x1cU;
    REQUIRE(overflow.map(high_base, bytes,
                         GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                         "overflow MOD0"));
    put_u32(overflow, high_base, switchrecomp::format::mod0_magic);
    put_i32(overflow, high_base + 4U, 0x20);
    require_error(switchrecomp::format::parse_mod0(overflow, high_base),
                  ErrorCode::ArithmeticOverflow);
}

TEST_CASE("Dynamic parser preserves unknown tags and allows repeated DT_NEEDED")
{
    Fixture fixture;
    put_u64(fixture.memory, fixture.dynamic_address + 12U * 16U,
            static_cast<std::uint64_t>(DynamicTag::DT_NEEDED));
    put_u64(fixture.memory, fixture.dynamic_address + 12U * 16U + 8U, 2U);
    put_u64(fixture.memory, fixture.dynamic_address + 13U * 16U,
            static_cast<std::uint64_t>(DynamicTag::DT_NULL));
    put_u64(fixture.memory, fixture.dynamic_address + 13U * 16U + 8U, 0U);

    const auto parsed = switchrecomp::format::parse_dynamic(
        fixture.memory, Fixture::base, fixture.dynamic_address);
    REQUIRE(parsed);
    REQUIRE(parsed.value().needed == std::vector<std::uint64_t>{1U, 2U});
    REQUIRE(parsed.value().entries[11U].tag == 0x70000000);
}

TEST_CASE("Dynamic parser rejects duplicate singleton tags")
{
    Fixture fixture;
    put_u64(fixture.memory, fixture.dynamic_address + 12U * 16U,
            static_cast<std::uint64_t>(DynamicTag::DT_STRTAB));
    put_u64(fixture.memory, fixture.dynamic_address + 12U * 16U + 8U, 0xc00U);
    put_u64(fixture.memory, fixture.dynamic_address + 13U * 16U,
            static_cast<std::uint64_t>(DynamicTag::DT_NULL));
    put_u64(fixture.memory, fixture.dynamic_address + 13U * 16U + 8U, 0U);
    require_error(switchrecomp::format::parse_dynamic(fixture.memory, Fixture::base,
                                                      fixture.dynamic_address),
                  ErrorCode::InvalidFormat);
}

TEST_CASE("Dynamic parser enforces DT_NULL and table boundary semantics")
{
    GuestMemory memory;
    std::vector<std::byte> bytes(0x30U);
    REQUIRE(memory.map(0x4000U, bytes,
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       "dynamic boundary"));
    for (std::size_t index = 0U; index < 3U; ++index)
    {
        put_u64(memory, 0x4000U + index * 16U, 0x70000000U + index);
        put_u64(memory, 0x4000U + index * 16U + 8U, index);
    }
    const DynamicParseLimits low_limit{2U, 10U, 1024U};
    require_error(switchrecomp::format::parse_dynamic(memory, 0U, 0x4000U, low_limit),
                  ErrorCode::ResourceLimit);

    GuestMemory exact;
    std::vector<std::byte> exact_bytes(0x20U);
    REQUIRE(exact.map(0x5000U, exact_bytes,
                      GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                      "exact dynamic"));
    put_u64(exact, 0x5000U, 0x70000000U);
    put_u64(exact, 0x5008U, 0U);
    put_u64(exact, 0x5010U, static_cast<std::uint64_t>(DynamicTag::DT_NULL));
    put_u64(exact, 0x5018U, 0U);
    const auto parsed = switchrecomp::format::parse_dynamic(exact, 0U, 0x5000U);
    REQUIRE(parsed);
    REQUIRE(parsed.value().entry_count == 2U);
}

TEST_CASE("Dynamic entries crossing mappings and dynamic pointer overflow fail safely")
{
    GuestMemory split;
    std::vector<std::byte> bytes(0x10U);
    REQUIRE(split.map(0x6000U, bytes,
                      GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                      "split dynamic"));
    put_u64(split, 0x6008U, static_cast<std::uint64_t>(DynamicTag::DT_NULL));
    require_error(switchrecomp::format::parse_dynamic(split, 0U, 0x6008U),
                  ErrorCode::UnmappedMemory);

    GuestMemory mapped;
    std::vector<std::byte> entry(0x20U);
    REQUIRE(mapped.map(0x7000U, entry,
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       "overflow dynamic"));
    put_u64(mapped, 0x7000U, static_cast<std::uint64_t>(DynamicTag::DT_STRTAB));
    put_u64(mapped, 0x7008U, 1U);
    put_u64(mapped, 0x7010U, static_cast<std::uint64_t>(DynamicTag::DT_NULL));
    put_u64(mapped, 0x7018U, 0U);
    require_error(switchrecomp::format::parse_dynamic(
                      mapped, std::numeric_limits<GuestAddress>::max(), 0x7000U),
                  ErrorCode::ArithmeticOverflow);
}

TEST_CASE("Dynamic metadata validates string, symbol, RELA and JMPREL ranges")
{
    Fixture fixture;
    auto update_value = [&](std::size_t index, std::uint64_t value)
    { put_u64(fixture.memory, fixture.dynamic_address + index * 16U + 8U, value); };

    update_value(2U, 3U); // DT_STRSZ too small for DT_NEEDED offset 1? still valid.
    const auto valid = switchrecomp::format::parse_module_metadata(
        fixture.memory, Fixture::base);
    REQUIRE(valid);

    update_value(4U, 16U);
    require_error(switchrecomp::format::parse_module_metadata(fixture.memory, Fixture::base),
                  ErrorCode::InvalidFormat);

    update_value(4U, switchrecomp::format::elf64_sym_size);
    update_value(6U, 25U);
    require_error(switchrecomp::format::parse_module_metadata(fixture.memory, Fixture::base),
                  ErrorCode::InvalidFormat);

    update_value(6U, 48U);
    update_value(10U, static_cast<std::uint64_t>(DynamicTag::DT_REL));
    require_error(switchrecomp::format::parse_module_metadata(fixture.memory, Fixture::base),
                  ErrorCode::Unsupported);

    update_value(10U, static_cast<std::uint64_t>(DynamicTag::DT_RELA));
    update_value(1U, 0x6000U);
    require_error(switchrecomp::format::parse_module_metadata(fixture.memory, Fixture::base),
                  ErrorCode::UnmappedMemory);
}

TEST_CASE("RELA parser preserves signed addends and ELF64 info fields")
{
    Fixture fixture;
    const auto metadata = switchrecomp::format::parse_module_metadata(
        fixture.memory, Fixture::base);
    REQUIRE(metadata);
    const auto rela = switchrecomp::format::parse_rela_table(
        fixture.memory, metadata.value().dynamic.value());
    REQUIRE(rela);
    REQUIRE(rela.value().size() == 2U);
    REQUIRE(rela.value()[0].offset == 0x1100U);
    REQUIRE(rela.value()[0].target_address == Fixture::base + 0x1100U);
    REQUIRE(rela.value()[0].symbol_index() == 3U);
    REQUIRE(rela.value()[0].relocation_type() == 0x101U);
    REQUIRE(rela.value()[0].addend == -7);
    REQUIRE(rela.value()[1].addend == 11);

    const auto jmprel = switchrecomp::format::parse_jmprel_table(
        fixture.memory, metadata.value().dynamic.value());
    REQUIRE(jmprel);
    REQUIRE(jmprel.value().size() == 1U);
    REQUIRE(jmprel.value()[0].symbol_index() == 5U);
}

TEST_CASE("Parsing MOD0, Dynamic and RELA is transactional for GuestMemory")
{
    Fixture fixture;
    std::vector<std::byte> before(0x5000U);
    std::vector<std::byte> after(0x5000U);
    REQUIRE(fixture.memory.read(Fixture::base, before));

    const auto metadata = switchrecomp::format::parse_module_metadata(
        fixture.memory, Fixture::base);
    REQUIRE(metadata);
    REQUIRE(switchrecomp::format::parse_rela_table(
        fixture.memory, metadata.value().dynamic.value()));
    REQUIRE(switchrecomp::format::parse_jmprel_table(
        fixture.memory, metadata.value().dynamic.value()));
    REQUIRE(fixture.memory.read(Fixture::base, after));
    REQUIRE(before == after);
}

TEST_CASE("RELA target address arithmetic is checked even for structural parsing")
{
    GuestMemory memory;
    std::vector<std::byte> bytes(0x40U);
    REQUIRE(memory.map(0x8000U, bytes,
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       "RELA"));
    put_u64(memory, 0x8000U, 1U);
    put_u64(memory, 0x8008U, 0U);
    put_u64(memory, 0x8010U, 0U);

    DynamicInfo dynamic{};
    dynamic.module_base = std::numeric_limits<GuestAddress>::max();
    dynamic.rela = switchrecomp::format::DynamicPointer{0x8000U, 0x8000U};
    dynamic.rela_count = 1U;
    const auto parsed = switchrecomp::format::parse_rela_table(memory, dynamic);
    require_error(parsed, ErrorCode::ArithmeticOverflow);
}

TEST_CASE("All MOD0 and Dynamic parser errors retain diagnostic wording")
{
    Fixture fixture;
    put_u32(fixture.memory, fixture.mod0_address, 0U);
    const auto mod0 = switchrecomp::format::parse_mod0(fixture.memory, fixture.mod0_address);
    REQUIRE_FALSE(mod0);
    REQUIRE(mod0.error().message.find("MOD0 magic mismatch") != std::string_view::npos);

    put_u32(fixture.memory, fixture.mod0_address, switchrecomp::format::mod0_magic);
    put_u64(fixture.memory, fixture.dynamic_address + 6U * 16U + 8U, 25U);
    const auto dynamic = switchrecomp::format::parse_module_metadata(
        fixture.memory, Fixture::base);
    REQUIRE_FALSE(dynamic);
    REQUIRE(dynamic.error().message.find("not divisible") != std::string_view::npos);
}
