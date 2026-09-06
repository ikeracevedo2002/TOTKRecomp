#include "switchrecomp/format/dynamic_symbols.hpp"
#include "switchrecomp/format/elf_rela.hpp"
#include "switchrecomp/loader/relocation_processor.hpp"
#include "switchrecomp/loader/symbol_resolver.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using switchrecomp::ErrorCode;
using switchrecomp::format::DynamicInfo;
using switchrecomp::format::DynamicPointer;
using switchrecomp::format::DynamicSymbolTable;
using switchrecomp::format::DynamicStringTable;
using switchrecomp::format::Relocation;
using switchrecomp::format::RelaEntry;
using switchrecomp::format::SymbolBinding;
using switchrecomp::format::SymbolType;
using switchrecomp::format::SymbolVisibility;
using switchrecomp::loader::SymbolResolver;
using switchrecomp::memory::GuestAddress;
using switchrecomp::memory::GuestMemory;
using switchrecomp::memory::GuestMemoryPermissions;

void put_u32(GuestMemory& memory, GuestAddress address, std::uint32_t value)
{
    std::array<std::byte, sizeof(value)> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    REQUIRE(memory.write(address, bytes));
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

void put_symbol(GuestMemory& memory, GuestAddress address, std::uint32_t name,
                std::uint8_t binding, std::uint8_t type, std::uint8_t visibility,
                std::uint16_t section, std::uint64_t value, std::uint64_t size)
{
    put_u32(memory, address, name);
    std::array<std::byte, 2> info_other{static_cast<std::byte>((binding << 4U) | type),
                                        static_cast<std::byte>(visibility)};
    put_bytes(memory, address + 4U, info_other);
    std::array<std::byte, 2> section_bytes{
        static_cast<std::byte>(section & 0xffU), static_cast<std::byte>((section >> 8U) & 0xffU)};
    put_bytes(memory, address + 6U, section_bytes);
    put_u64(memory, address + 8U, value);
    put_u64(memory, address + 16U, size);
}

struct Fixture
{
    static constexpr GuestAddress module_base = 0x1000U;
    static constexpr GuestAddress string_address = 0x1100U;
    static constexpr GuestAddress symbol_address = 0x1200U;
    static constexpr GuestAddress hash_address = 0x1400U;
    GuestMemory memory;
    DynamicInfo dynamic;

    Fixture() : memory()
    {
        REQUIRE(memory.map(module_base, 0x3000U,
                           GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                           "module"));
        const std::array<std::byte, 24> strings{
            std::byte{0}, std::byte{'f'}, std::byte{'o'}, std::byte{'o'}, std::byte{0},
            std::byte{'b'}, std::byte{'a'}, std::byte{'r'}, std::byte{0}, std::byte{'m'},
            std::byte{'i'}, std::byte{'s'}, std::byte{'s'}, std::byte{'i'}, std::byte{'n'},
            std::byte{'g'}, std::byte{0}, std::byte{'h'}, std::byte{'i'}, std::byte{'d'},
            std::byte{'d'}, std::byte{'e'}, std::byte{'n'}, std::byte{0}};
        put_bytes(memory, string_address, strings);

        // SysV hash: nbucket=1, nchain=5, one bucket and five chain words.
        put_u32(memory, hash_address, 1U);
        put_u32(memory, hash_address + 4U, 5U);
        put_u32(memory, hash_address + 8U, 1U);
        for (std::uint32_t index = 0U; index < 5U; ++index)
        {
            put_u32(memory, hash_address + 12U + index * 4U, index == 4U ? 1U : 0U);
        }

        put_symbol(memory, symbol_address, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
        put_symbol(memory, symbol_address + 24U, 1U, 1U, 2U, 0U, 1U, 0x500U, 0x40U);
        put_symbol(memory, symbol_address + 48U, 5U, 2U, 2U, 0U, 0U, 0U, 0U);
        put_symbol(memory, symbol_address + 72U, 9U, 1U, 2U, 0U, 0U, 0U, 0U);
        put_symbol(memory, symbol_address + 96U, 17U, 0U, 1U, 2U, 2U, 0x40U, 4U);

        dynamic.module_base = module_base;
        dynamic.strtab = DynamicPointer{string_address - module_base, string_address};
        dynamic.strsz = strings.size();
        dynamic.symtab = DynamicPointer{symbol_address - module_base, symbol_address};
        dynamic.syment = switchrecomp::format::elf64_sym_size;
        dynamic.hash = DynamicPointer{hash_address - module_base, hash_address};
    }
};

template <typename T> void require_error(const switchrecomp::Result<T>& result, ErrorCode code)
{
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == code);
}

} // namespace

TEST_CASE("dynamic strings are bounded, binary-safe, and NUL terminated")
{
    Fixture fixture;
    const auto strings = DynamicStringTable::parse(fixture.memory, fixture.dynamic);
    REQUIRE(strings);
    REQUIRE(strings.value().get(0U).value().empty());
    REQUIRE(strings.value().get(1U).value() == "foo");
    REQUIRE(strings.value().get(5U).value() == "bar");
    require_error(strings.value().get(fixture.dynamic.strsz.value()),
                  ErrorCode::StringTableOutOfBounds);
    require_error(strings.value().get(1000U), ErrorCode::StringTableOutOfBounds);

    const std::array<std::byte, 3> unterminated{std::byte{'x'}, std::byte{'y'}, std::byte{'z'}};
    GuestMemory bad_memory;
    REQUIRE(bad_memory.map(0x8000U, unterminated, GuestMemoryPermissions::Read, "strings"));
    DynamicInfo bad = fixture.dynamic;
    bad.strtab = DynamicPointer{0U, 0x8000U};
    bad.strsz = unterminated.size();
    const auto bad_strings = DynamicStringTable::parse(bad_memory, bad);
    REQUIRE(bad_strings);
    require_error(bad_strings.value().get(0U), ErrorCode::UnterminatedSymbolName);
}

TEST_CASE("dynamic symbols use hash-derived bounds and preserve ELF metadata")
{
    Fixture fixture;
    const auto symbols = DynamicSymbolTable::parse(fixture.memory, fixture.dynamic);
    REQUIRE(symbols);
    REQUIRE(symbols.value().symbols.size() == 5U);
    REQUIRE(symbols.value().symbols[1].name == "foo");
    REQUIRE(symbols.value().symbols[1].binding == SymbolBinding::Global);
    REQUIRE(symbols.value().symbols[1].type == SymbolType::Function);
    REQUIRE(symbols.value().symbols[1].is_defined());
    REQUIRE(symbols.value().symbols[2].binding == SymbolBinding::Weak);
    REQUIRE_FALSE(symbols.value().symbols[2].is_defined());
    REQUIRE(symbols.value().symbols[4].visibility == SymbolVisibility::Hidden);
    REQUIRE(symbols.value().imports().size() == 3U);

    DynamicInfo no_hash = fixture.dynamic;
    no_hash.hash.reset();
    require_error(DynamicSymbolTable::parse(fixture.memory, no_hash), ErrorCode::Unsupported);
}

TEST_CASE("GNU hash derives a finite dynsym bound")
{
    GuestMemory memory;
    REQUIRE(memory.map(0x9000U, 0x1000U,
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       "GNU hash module"));
    const std::array<std::byte, 5> strings{
        std::byte{0}, std::byte{'f'}, std::byte{'o'}, std::byte{'o'}, std::byte{0}};
    put_bytes(memory, 0x9100U, strings);
    put_symbol(memory, 0x9200U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
    put_symbol(memory, 0x9218U, 1U, 1U, 2U, 0U, 1U, 0x40U, 4U);
    put_u32(memory, 0x9300U, 1U);
    put_u32(memory, 0x9304U, 1U);
    put_u32(memory, 0x9308U, 1U);
    put_u32(memory, 0x930cU, 0U);
    put_u64(memory, 0x9310U, 0U);
    put_u32(memory, 0x9318U, 1U);
    put_u32(memory, 0x931cU, 1U);

    DynamicInfo dynamic{};
    dynamic.module_base = 0x9000U;
    dynamic.strtab = DynamicPointer{0x100U, 0x9100U};
    dynamic.strsz = strings.size();
    dynamic.symtab = DynamicPointer{0x200U, 0x9200U};
    dynamic.syment = switchrecomp::format::elf64_sym_size;
    dynamic.gnu_hash = DynamicPointer{0x300U, 0x9300U};
    const auto parsed = DynamicSymbolTable::parse(memory, dynamic);
    REQUIRE(parsed);
    REQUIRE(parsed.value().symbols.size() == 2U);
    REQUIRE(parsed.value().symbols[1].name == "foo");
}

TEST_CASE("symbol resolution distinguishes definitions, externals, and missing imports")
{
    Fixture fixture;
    const auto symbols = DynamicSymbolTable::parse(fixture.memory, fixture.dynamic);
    REQUIRE(symbols);
    SymbolResolver resolver(symbols.value(), Fixture::module_base);

    REQUIRE(resolver.resolve(1U).value().address == Fixture::module_base + 0x500U);
    REQUIRE(resolver.add_external("bar", 0x7200001000ULL));
    const auto bar = resolver.resolve(2U);
    REQUIRE(bar);
    REQUIRE(bar.value().resolved);
    REQUIRE(bar.value().address == 0x7200001000ULL);
    require_error(resolver.resolve(3U), ErrorCode::UndefinedStrongSymbol);
    REQUIRE(resolver.unresolved_imports().size() == 2U);
    require_error(resolver.add_external("bar", 0x1234U), ErrorCode::DuplicateExternalSymbol);
}

TEST_CASE("AArch64 RELA application is little endian, checked, and loader-authorized")
{
    Fixture fixture;
    REQUIRE(fixture.memory.map(0x5000U, 0x20U, GuestMemoryPermissions::Read, "relocation target"));
    const auto symbols = DynamicSymbolTable::parse(fixture.memory, fixture.dynamic);
    REQUIRE(symbols);
    SymbolResolver resolver(symbols.value(), Fixture::module_base);
    REQUIRE(resolver.add_external("bar", 0x7200001000ULL));

    const std::array<RelaEntry, 5> binary{
        RelaEntry{0U, 0U, 0U, 0},
        RelaEntry{0U, 0x5000U, 1027U, 0x20},
        RelaEntry{8U, 0x5008U, (std::uint64_t{1U} << 32U) | 257U, 5},
        RelaEntry{16U, 0x5010U, (std::uint64_t{2U} << 32U) | 1025U, -16},
        RelaEntry{24U, 0x5018U, (std::uint64_t{2U} << 32U) | 1026U, 0},
    };
    const auto semantic = switchrecomp::format::make_relocations(binary);
    REQUIRE(semantic);
    REQUIRE(switchrecomp::loader::apply_relocations(fixture.memory, semantic.value(), resolver));

    std::array<std::byte, 8> bytes{};
    REQUIRE(fixture.memory.read(0x5000U, bytes));
    REQUIRE(bytes == std::array<std::byte, 8>{std::byte{0x20}, std::byte{0x10}, std::byte{0},
                                               std::byte{0}, std::byte{0}, std::byte{0},
                                               std::byte{0}, std::byte{0}});
    REQUIRE(fixture.memory.read(0x5008U, bytes));
    REQUIRE(bytes == std::array<std::byte, 8>{std::byte{0x05}, std::byte{0x15}, std::byte{0},
                                               std::byte{0}, std::byte{0}, std::byte{0},
                                               std::byte{0}, std::byte{0}});
    REQUIRE(fixture.memory.write(0x5000U, bytes).error().code == ErrorCode::PermissionDenied);
}

TEST_CASE("relocation processing is atomic on validation or type failure")
{
    Fixture fixture;
    REQUIRE(fixture.memory.map(0x6000U, 0x0cU, GuestMemoryPermissions::Read, "target"));
    const auto symbols = DynamicSymbolTable::parse(fixture.memory, fixture.dynamic);
    REQUIRE(symbols);
    SymbolResolver resolver(symbols.value(), Fixture::module_base);

    const std::array<Relocation, 2> invalid{
        Relocation{0U, 0x6000U, 1027U, switchrecomp::format::AArch64RelocationType::Relative,
                   0U, 1},
        Relocation{8U, 0x6008U, 0xffffU, switchrecomp::format::AArch64RelocationType::Unknown,
                   0U, 0}};
    require_error(switchrecomp::loader::apply_relocations(fixture.memory, invalid, resolver),
                  ErrorCode::UnsupportedRelocationType);
    std::array<std::byte, 8> unchanged{};
    REQUIRE(fixture.memory.read(0x6000U, unchanged));
    REQUIRE(unchanged == std::array<std::byte, 8>{});

    const std::array<Relocation, 1> out_of_bounds{
        Relocation{0U, 0x6008U, 1027U, switchrecomp::format::AArch64RelocationType::Relative,
                   0U, 0}};
    require_error(switchrecomp::loader::apply_relocations(fixture.memory, out_of_bounds, resolver),
                  ErrorCode::RelocationTargetNotWritableDuringLoad);

    const std::array<Relocation, 1> overflow{
        Relocation{0U, std::numeric_limits<GuestAddress>::max() - 7U, 1027U,
                   switchrecomp::format::AArch64RelocationType::Relative, 0U, 0}};
    require_error(switchrecomp::loader::apply_relocations(fixture.memory, overflow, resolver),
                  ErrorCode::ArithmeticOverflow);
}
