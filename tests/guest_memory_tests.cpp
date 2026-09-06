#include "switchrecomp/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
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
using switchrecomp::memory::GuestMemory;
using switchrecomp::memory::GuestMemoryLimits;
using switchrecomp::memory::GuestMemoryPermissions;
using switchrecomp::memory::GuestRegionKind;

template <typename T> void require_error(const switchrecomp::Result<T>& result, ErrorCode code)
{
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == code);
}

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values)
    {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

} // namespace

TEST_CASE("Guest memory maps, describes, and reads an owned region")
{
    GuestMemory memory;
    const auto source = bytes({0x10U, 0x20U, 0x30U, 0x40U, 0x50U});
    REQUIRE(memory.map(0x1000U, source, GuestMemoryPermissions::Read, ".rodata",
                       GuestRegionKind::Rodata));

    REQUIRE(memory.region_count() == 1U);
    REQUIRE(memory.total_mapped_size() == source.size());
    const auto info = memory.region_at(0x1002U);
    REQUIRE(info);
    REQUIRE(info.value().base == 0x1000U);
    REQUIRE(info.value().size == source.size());
    REQUIRE(info.value().end() == 0x1005U);
    REQUIRE(info.value().permissions == GuestMemoryPermissions::Read);
    REQUIRE(info.value().kind == GuestRegionKind::Rodata);
    REQUIRE(info.value().name == ".rodata");

    std::array<std::byte, 1> first{};
    REQUIRE(memory.read(0x1000U, first));
    REQUIRE(first[0] == std::byte{0x10});

    std::array<std::byte, 1> last{};
    REQUIRE(memory.read(0x1004U, last));
    REQUIRE(last[0] == std::byte{0x50});

    std::array<std::byte, 2> middle{};
    REQUIRE(memory.read(0x1001U, middle));
    REQUIRE(middle == std::array<std::byte, 2>{std::byte{0x20}, std::byte{0x30}});

    std::array<std::byte, 5> complete{};
    REQUIRE(memory.read(0x1000U, complete));
    REQUIRE(std::vector<std::byte>(complete.begin(), complete.end()) == source);
}

TEST_CASE("Guest memory writes only to writable regions")
{
    GuestMemory memory;
    REQUIRE(memory.map(0x2000U, 4U, GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       ".data", GuestRegionKind::Data));
    const auto update = bytes({0xaaU, 0xbbU});
    REQUIRE(memory.write(0x2001U, update));

    std::array<std::byte, 4> data{};
    REQUIRE(memory.read(0x2000U, data));
    REQUIRE(data ==
            std::array<std::byte, 4>{std::byte{0}, std::byte{0xaa}, std::byte{0xbb}, std::byte{0}});

    REQUIRE(memory.map(0x3000U, bytes({0x11U, 0x22U}), GuestMemoryPermissions::Read, ".rodata",
                       GuestRegionKind::Rodata));
    require_error(memory.write(0x3000U, update), ErrorCode::PermissionDenied);

    REQUIRE(memory.map(0x4000U, bytes({0x90U, 0x91U}),
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute, ".text",
                       GuestRegionKind::Text));
    require_error(memory.write(0x4000U, update), ErrorCode::PermissionDenied);
}

TEST_CASE("Guest memory exposes permissions and execute state")
{
    GuestMemory memory;
    REQUIRE(memory.map(0x1000U, bytes({0x01U}),
                       GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute, ".text",
                       GuestRegionKind::Text));
    REQUIRE(memory.map(0x2000U, bytes({0x02U}), GuestMemoryPermissions::Read, ".rodata",
                       GuestRegionKind::Rodata));
    REQUIRE(memory.map(0x3000U, 2U, GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       ".bss", GuestRegionKind::Bss));

    REQUIRE(memory.is_executable(0x1000U).value());
    REQUIRE_FALSE(memory.is_executable(0x2000U).value());
    REQUIRE_FALSE(memory.is_executable(0x3000U).value());
    REQUIRE(memory.permissions_at(0x1000U).value() ==
            (GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute));
    REQUIRE(memory.permissions_at(0x3000U).value() ==
            (GuestMemoryPermissions::Read | GuestMemoryPermissions::Write));
}

TEST_CASE("BSS mappings are zero-filled and writable")
{
    GuestMemory memory;
    REQUIRE(memory.map(0x5000U, 8U, GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       ".bss", GuestRegionKind::Bss));
    std::array<std::byte, 8> initial{};
    REQUIRE(memory.read(0x5000U, initial));
    REQUIRE(std::all_of(initial.begin(), initial.end(),
                        [](const std::byte value) { return value == std::byte{0}; }));

    const auto value = bytes({0x12U, 0x34U});
    REQUIRE(memory.write(0x5003U, value));
    std::array<std::byte, 2> written{};
    REQUIRE(memory.read(0x5003U, written));
    REQUIRE(written == std::array<std::byte, 2>{std::byte{0x12}, std::byte{0x34}});
}

TEST_CASE("Guest memory rejects all overlapping ranges but accepts adjacent ranges")
{
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 6> overlaps{
        std::pair{0x1800U, 0x100U},  // partial overlap at the beginning
        std::pair{0x1f00U, 0x200U},  // one-byte/partial overlap at the end
        std::pair{0x1000U, 0x1000U}, // same range
        std::pair{0x0U, 0x3000U},    // containing range
        std::pair{0x1100U, 0x100U},  // contained range
        std::pair{0x1000U, 0x1U},    // same base
    };
    for (const auto& [base, size] : overlaps)
    {
        GuestMemory memory;
        REQUIRE(memory.map(0x1000U, 0x1000U, GuestMemoryPermissions::Read, "original"));
        require_error(memory.map(base, size, GuestMemoryPermissions::Read, "overlap"),
                      ErrorCode::InvalidArgument);
        REQUIRE(memory.region_count() == 1U);
        REQUIRE(memory.total_mapped_size() == 0x1000U);
    }

    GuestMemory adjacent;
    REQUIRE(adjacent.map(0x1000U, bytes({0x11U}), GuestMemoryPermissions::Read, "first"));
    REQUIRE(adjacent.map(0x1001U, bytes({0x22U}), GuestMemoryPermissions::Read, "second"));
    REQUIRE(adjacent.region_count() == 2U);
    std::array<std::byte, 1> first{};
    std::array<std::byte, 1> second{};
    REQUIRE(adjacent.read(0x1000U, first));
    REQUIRE(adjacent.read(0x1001U, second));
    REQUIRE(first[0] == std::byte{0x11});
    REQUIRE(second[0] == std::byte{0x22});
}

TEST_CASE("Guest memory rejects overflow and unmapped accesses")
{
    GuestMemory memory;
    require_error(memory.map(std::numeric_limits<std::uint64_t>::max() - 0x10U, 0x20U,
                             GuestMemoryPermissions::Read, "overflow"),
                  ErrorCode::ArithmeticOverflow);
    REQUIRE(memory.map(std::numeric_limits<std::uint64_t>::max(), 0U, GuestMemoryPermissions::Read,
                       "empty"));
    REQUIRE(memory.region_count() == 0U);

    REQUIRE(memory.map(0x1000U, 0x10U, GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       "mapped"));
    std::array<std::byte, 1> byte{};
    require_error(memory.read(0x0fffU, byte), ErrorCode::UnmappedMemory);
    require_error(memory.read(0x1010U, byte), ErrorCode::UnmappedMemory);
    std::array<std::byte, 2> partially_unmapped{};
    require_error(memory.read(0x100fU, partially_unmapped), ErrorCode::UnmappedMemory);
    std::array<std::byte, 9> crossing{};
    require_error(memory.read(0x1008U, crossing), ErrorCode::UnmappedMemory);
    require_error(memory.write(0x1008U, crossing), ErrorCode::UnmappedMemory);
}

TEST_CASE("Guest memory requires one mapping to contain each access")
{
    GuestMemory memory;
    REQUIRE(memory.map(0x1000U, bytes({0x01U, 0x02U}), GuestMemoryPermissions::Read, "first"));
    REQUIRE(memory.map(0x1002U, bytes({0x03U, 0x04U}), GuestMemoryPermissions::Read, "second"));
    std::array<std::byte, 4> destination{};
    require_error(memory.read(0x1000U, destination), ErrorCode::UnmappedMemory);
    REQUIRE(memory.read(0x1000U, std::span<std::byte>(destination).subspan(0U, 2U)));
}

TEST_CASE("Guest memory owns backing storage after source lifetime ends")
{
    GuestMemory memory;
    {
        auto source = bytes({0x41U, 0x42U, 0x43U});
        REQUIRE(memory.map(0x7000U, source, GuestMemoryPermissions::Read, "owned"));
        source[0] = std::byte{0xff};
    }
    std::array<std::byte, 3> destination{};
    REQUIRE(memory.read(0x7000U, destination));
    REQUIRE(destination ==
            std::array<std::byte, 3>{std::byte{0x41}, std::byte{0x42}, std::byte{0x43}});
}

TEST_CASE("Guest memory enforces region, total, and count limits")
{
    const GuestMemoryLimits limits{4U, 6U, 2U};
    GuestMemory memory(limits);
    require_error(memory.map(0x1000U, 5U, GuestMemoryPermissions::Read, "too-large"),
                  ErrorCode::ResourceLimit);
    REQUIRE(memory.map(0x1000U, 4U, GuestMemoryPermissions::Read, "first"));
    require_error(memory.map(0x2000U, 3U, GuestMemoryPermissions::Read, "too-total"),
                  ErrorCode::ResourceLimit);
    REQUIRE(memory.map(0x2000U, 2U, GuestMemoryPermissions::Read, "second"));
    require_error(memory.map(0x3000U, 1U, GuestMemoryPermissions::Read, "too-many"),
                  ErrorCode::ResourceLimit);
    REQUIRE(memory.region_count() == 2U);
    REQUIRE(memory.total_mapped_size() == 6U);

    GuestMemory no_regions(GuestMemoryLimits{4U, 4U, 0U});
    require_error(no_regions.map(0x1000U, 1U, GuestMemoryPermissions::Read, "disabled"),
                  ErrorCode::ResourceLimit);
}

TEST_CASE("Guest memory supports addresses above 4 GiB")
{
    constexpr std::uint64_t base = 0x7100000000ULL;
    GuestMemory memory;
    REQUIRE(memory.map(base, bytes({0xdeU, 0xadU, 0xbeU, 0xefU}), GuestMemoryPermissions::Read,
                       "high"));
    std::array<std::byte, 4> destination{};
    REQUIRE(memory.read(base, destination));
    REQUIRE(destination == std::array<std::byte, 4>{std::byte{0xde}, std::byte{0xad},
                                                    std::byte{0xbe}, std::byte{0xef}});
    REQUIRE(memory.region_at(base + 3U).value().base == base);
}

TEST_CASE("Empty reads and writes are explicit no-ops")
{
    GuestMemory memory;
    std::array<std::byte, 1> storage{};
    REQUIRE(memory.read(0xffffffffffffffffULL, std::span<std::byte>{}));
    REQUIRE(memory.write(0xffffffffffffffffULL, std::span<const std::byte>{}));
    REQUIRE(memory.region_count() == 0U);
    REQUIRE(storage[0] == std::byte{0});
}
