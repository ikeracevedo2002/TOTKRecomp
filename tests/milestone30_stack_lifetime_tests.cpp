#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

template <typename T> void require_error(const Result<T>& result, ErrorCode code)
{
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == code);
}

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size() * 4U);
    for (const auto value : values)
    {
        result.push_back(static_cast<std::byte>(value & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    }
    return result;
}

[[nodiscard]] analysis::FunctionSeed seed(memory::GuestAddress address)
{
    return analysis::FunctionSeed{address, analysis::FunctionDiscoverySource::AnalystSeed,
                                  analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "synthetic M30 function"};
}

struct Fixture
{
    memory::GuestMemory memory;
    analysis::FinalizedFunctionMap function_map;
};

[[nodiscard]] Result<Fixture> make_fixture(memory::GuestAddress base,
                                           memory::GuestMemoryLimits limits = {})
{
    Fixture fixture{memory::GuestMemory(limits), {}};
    const auto code = words({0xd503201fU, 0xd65f03c0U});
    const auto mapped = fixture.memory.map(
        base, code, memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m30.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());

    analysis::ModuleIdentity identity;
    identity.module = "synthetic-m30";
    identity.build_id = "synthetic-m30-build";
    identity.input_sha256 = "synthetic-m30-sha";
    identity.translator_version = version;
    identity.guest_base = base;
    identity.executable_ranges.push_back({base, 8U});
    identity.entry_points.push_back({base, analysis::EntryPointKind::DynamicInit,
                                     "synthetic M30 entry", analysis::FunctionConfidence::High,
                                     false, "generation-scoped stack lifetime"});
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &fixture.memory, {seed(base)}});
    if (!map) return Result<Fixture>::failure(map.error());
    fixture.function_map = std::move(map).value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] Result<execution::ExecutionSessionResult> run_generation(
    Fixture& fixture, memory::GuestAddress entry, execution::ExecutionSessionOptions options = {})
{
    const auto selected = execution::select_entry(
        fixture.function_map.identity(), execution::EntrySelectionKind::DynamicInit);
    if (!selected) return Result<execution::ExecutionSessionResult>::failure(selected.error());
    auto selection = selected.value();
    selection.address = entry;
    {
        execution::ExecutionSession session(fixture.memory, fixture.function_map, {},
                                            std::move(options));
        const auto run = session.run(selection);
        if (!run) return Result<execution::ExecutionSessionResult>::failure(run.error());
        return Result<execution::ExecutionSessionResult>::success(std::move(run).value());
    }
}

[[nodiscard]] std::size_t count_named_regions(const memory::GuestMemory& memory,
                                              std::string_view name)
{
    std::size_t count = 0U;
    for (const auto& region : memory.regions())
        if (region.name == name) ++count;
    return count;
}

} // namespace

TEST_CASE("M30 owned mappings release exactly one whole region and reject stale tokens")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x100U, memory::GuestMemoryPermissions::Read, "static"));
    const auto first = memory.map_owned(0x3000U, 0x100U,
                                        memory::GuestMemoryPermissions::Read |
                                            memory::GuestMemoryPermissions::Write,
                                        "owned.first");
    const auto middle = memory.map_owned(0x5000U, 0x100U, memory::GuestMemoryPermissions::Read,
                                         "owned.middle");
    const auto top = memory.map_owned(0x7000U, 0x100U, memory::GuestMemoryPermissions::Read,
                                      "owned.top");
    REQUIRE(first);
    REQUIRE(middle);
    REQUIRE(top);
    REQUIRE(memory.region_count() == 4U);
    REQUIRE(memory.total_mapped_size() == 0x400U);

    REQUIRE(memory.release_owned(middle.value()));
    REQUIRE(memory.region_count() == 3U);
    REQUIRE(memory.regions()[0].base == 0x1000U);
    REQUIRE(memory.regions()[1].base == 0x3000U);
    REQUIRE(memory.regions()[2].base == 0x7000U);
    require_error(memory.release_owned(middle.value()), ErrorCode::InvalidArgument);
    REQUIRE(memory.region_count() == 3U);
    REQUIRE(memory.total_mapped_size() == 0x300U);

    REQUIRE(memory.release_owned(first.value()));
    REQUIRE(memory.release_owned(top.value()));
    REQUIRE(memory.region_count() == 1U);
    REQUIRE(memory.region_at(0x1000U));
    REQUIRE_FALSE(memory.region_at(0x3000U));
    REQUIRE(memory.accounting().owned_mappings_reclaimed == 3U);
}

TEST_CASE("M30 ownership is memory-scoped and static mappings are not releasable")
{
    memory::GuestMemory memory;
    const auto stale = memory.map_owned(0x2000U, 0x100U, memory::GuestMemoryPermissions::Read,
                                        "owned.original");
    REQUIRE(stale);
    REQUIRE(memory.release_owned(stale.value()));
    REQUIRE(memory.map(0x2000U, 0x100U, memory::GuestMemoryPermissions::Read, "static.replacement"));
    const auto before = memory.accounting();
    require_error(memory.release_owned(stale.value()), ErrorCode::InvalidArgument);
    REQUIRE(memory.accounting().live_mapped_bytes == before.live_mapped_bytes);
    REQUIRE(memory.accounting().live_region_count == before.live_region_count);
    REQUIRE(memory.region_at(0x2000U).value().name == "static.replacement");

    memory::GuestMemory foreign;
    const auto foreign_token = foreign.map_owned(0x2000U, 0x100U,
                                                 memory::GuestMemoryPermissions::Read,
                                                 "foreign.owned");
    memory::GuestMemory other;
    const auto other_token = other.map_owned(0x2000U, 0x100U,
                                             memory::GuestMemoryPermissions::Read, "other.owned");
    REQUIRE(foreign_token);
    REQUIRE(other_token);
    require_error(other.release_owned(foreign_token.value()), ErrorCode::InvalidArgument);
    REQUIRE(other.region_count() == 1U);
    REQUIRE(other.release_owned(other_token.value()));
    REQUIRE(foreign.release_owned(foreign_token.value()));
}

TEST_CASE("M30 refuses to copy live owned mappings without an ownership transfer")
{
    memory::GuestMemory memory;
    const auto token = memory.map_owned(0x2000U, 0x100U, memory::GuestMemoryPermissions::Read,
                                        "owned");
    REQUIRE(token);
    REQUIRE_THROWS_AS(([&]() { memory::GuestMemory copy(memory); }()), std::logic_error);
    REQUIRE(memory.region_count() == 1U);
    REQUIRE(memory.accounting().live_owned_mappings == 1U);
    REQUIRE(memory.release_owned(token.value()));
}

TEST_CASE("M30 mapping failures are transactional and zero initialization is repeatable")
{
    memory::GuestMemory memory(memory::GuestMemoryLimits{0x1000U, 0x1100U, 2U});
    REQUIRE(memory.map(0x1000U, 0x100U, memory::GuestMemoryPermissions::Read, "static"));
    const auto before = memory.accounting();
    const auto failed = memory.map_owned(0x3000U, 0x1001U,
                                         memory::GuestMemoryPermissions::Read, "too-large");
    require_error(failed, ErrorCode::ResourceLimit);
    REQUIRE(failed.error().message.find("requested 4097") != std::string::npos);
    REQUIRE(memory.accounting().live_mapped_bytes == before.live_mapped_bytes);
    REQUIRE(memory.accounting().live_region_count == before.live_region_count);
    REQUIRE(memory.accounting().owned_mappings_created == before.owned_mappings_created);
    REQUIRE(memory.accounting().virtual_address_high_water == before.virtual_address_high_water);

    const auto first = memory.map_owned(0x3000U, 0x1000U,
                                        memory::GuestMemoryPermissions::Read |
                                            memory::GuestMemoryPermissions::Write,
                                        "owned.zero.first");
    REQUIRE(first);
    const std::array<std::byte, 3> non_zero{std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}};
    REQUIRE(memory.write(0x3000U, non_zero));
    std::array<std::byte, 3> readback{};
    REQUIRE(memory.read(0x3000U, readback));
    REQUIRE(readback == non_zero);
    REQUIRE(memory.release_owned(first.value()));

    const auto second = memory.map_owned(0x3000U, 0x1000U,
                                         memory::GuestMemoryPermissions::Read |
                                             memory::GuestMemoryPermissions::Write,
                                         "owned.zero.second");
    REQUIRE(second);
    readback.fill(std::byte{0xff});
    REQUIRE(memory.read(0x3000U, readback));
    REQUIRE(readback == std::array<std::byte, 3>{std::byte{0}, std::byte{0}, std::byte{0}});
    REQUIRE(memory.release_owned(second.value()));
    const auto accounting = memory.accounting();
    REQUIRE(accounting.owned_mappings_created == 2U);
    REQUIRE(accounting.owned_mappings_reclaimed == 2U);
    REQUIRE(accounting.live_owned_mappings == 0U);
    REQUIRE(accounting.peak_live_owned_mappings == 1U);
    REQUIRE(accounting.peak_live_owned_bytes == 0x1000U);
    REQUIRE(accounting.cumulative_owned_bytes == 0x2000U);

    memory::GuestMemory total_limited(memory::GuestMemoryLimits{0x1000U, 0x10ffU, 2U});
    REQUIRE(total_limited.map(0x7000U, 0x100U, memory::GuestMemoryPermissions::Read, "static"));
    const auto total_before = total_limited.accounting();
    const auto total_failed = total_limited.map_owned(
        0x9000U, 0x1000U, memory::GuestMemoryPermissions::Read, "too-total");
    require_error(total_failed, ErrorCode::ResourceLimit);
    REQUIRE(total_failed.error().message.find("consumed 4352") != std::string::npos);
    REQUIRE(total_failed.error().message.find("limit 4351") != std::string::npos);
    REQUIRE(total_limited.accounting().live_mapped_bytes == total_before.live_mapped_bytes);
    REQUIRE(total_limited.accounting().live_region_count == total_before.live_region_count);
}

TEST_CASE("M30 thousands of controlled generations keep live storage bounded")
{
    constexpr memory::GuestAddress base = 0x1000U;
    constexpr std::size_t generations = 2000U;
    auto fixture = make_fixture(base, memory::GuestMemoryLimits{0x1000U, 0x1008U, 2U});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.synthetic_stack_size = 0x1000U;
    options.stack_guard_gap = 0x1000U;

    std::optional<memory::GuestAddress> first_stack;
    std::optional<memory::GuestAddress> second_stack;
    for (std::size_t generation = 0U; generation < generations; ++generation)
    {
        const auto result = run_generation(fixture.value(), base, options);
        REQUIRE(result);
        REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
        if (generation == 0U) first_stack = result.value().stack_base;
        if (generation == 1U) second_stack = result.value().stack_base;
        const auto accounting = fixture.value().memory.accounting();
        REQUIRE(accounting.live_mapped_bytes == 8U);
        REQUIRE(accounting.live_region_count == 1U);
        REQUIRE(accounting.live_owned_mappings == 0U);
        REQUIRE(count_named_regions(fixture.value().memory, "synthetic.controlled.stack") == 0U);
    }
    REQUIRE(first_stack);
    REQUIRE(second_stack);
    REQUIRE(second_stack.value() - first_stack.value() == 0x2000U);
    const auto accounting = fixture.value().memory.accounting();
    REQUIRE(accounting.owned_mappings_created == generations);
    REQUIRE(accounting.owned_mappings_reclaimed == generations);
    REQUIRE(accounting.peak_live_owned_mappings == 1U);
    REQUIRE(accounting.peak_live_owned_bytes == 0x1000U);
    REQUIRE(accounting.cumulative_owned_bytes == generations * 0x1000U);
    REQUIRE(accounting.peak_live_mapped_bytes == 0x1008U);
    REQUIRE(accounting.virtual_address_high_water == first_stack.value() +
                                                         generations * 0x2000U + 0x1000U -
                                                         0x2000U);
}

TEST_CASE("M30 keeps a completed stack alive through the session and reclaims it after return")
{
    constexpr memory::GuestAddress base = 0x9000U;
    auto fixture = make_fixture(base, memory::GuestMemoryLimits{0x2000U, 0x3008U, 2U});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.synthetic_stack_size = 0x2000U;
    options.stack_guard_gap = 0x1000U;

    const auto selected = execution::select_entry(
        fixture.value().function_map.identity(), execution::EntrySelectionKind::DynamicInit);
    REQUIRE(selected);
    memory::GuestAddress completed_stack = 0U;
    {
        execution::ExecutionSession session(fixture.value().memory, fixture.value().function_map,
                                            {}, options);
        const auto result = session.run(selected.value());
        REQUIRE(result);
        REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
        completed_stack = result.value().stack_base;
        REQUIRE(fixture.value().memory.region_at(result.value().stack_base));
        const std::array<std::byte, 2> marker{std::byte{0x5a}, std::byte{0xa5}};
        REQUIRE(fixture.value().memory.write(result.value().stack_base, marker));
        std::array<std::byte, 2> readback{};
        REQUIRE(fixture.value().memory.read(result.value().stack_base, readback));
        REQUIRE(readback == marker);
        REQUIRE(fixture.value().memory.accounting().live_owned_mappings == 1U);
    }
    REQUIRE_FALSE(fixture.value().memory.region_at(completed_stack));
    REQUIRE(fixture.value().memory.accounting().live_owned_mappings == 0U);
    REQUIRE(fixture.value().memory.accounting().owned_mappings_reclaimed == 1U);
}

TEST_CASE("M30 typed stops still release the generation stack")
{
    auto fixture = make_fixture(0x20000U, memory::GuestMemoryLimits{0x1000U, 0x2008U, 2U});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.synthetic_stack_size = 0x1000U;
    options.stack_guard_gap = 0x1000U;
    options.budgets.max_events = 3U;
    const auto result = run_generation(fixture.value(), 0x20000U, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EventLimitExceeded);
    REQUIRE(fixture.value().memory.accounting().live_owned_mappings == 0U);
    REQUIRE(fixture.value().memory.accounting().owned_mappings_created == 1U);
    REQUIRE(fixture.value().memory.accounting().owned_mappings_reclaimed == 1U);
}

TEST_CASE("M30 infrastructure errors after stack mapping still release the stack")
{
    constexpr auto maximum = std::numeric_limits<memory::GuestAddress>::max();
    constexpr auto base = maximum - 0x2007U;
    auto fixture = make_fixture(base, memory::GuestMemoryLimits{0x1000U, 0x1008U, 2U});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.synthetic_stack_size = 0x1000U;
    options.stack_guard_gap = 0U;

    const auto selected = execution::select_entry(
        fixture.value().function_map.identity(), execution::EntrySelectionKind::DynamicInit);
    REQUIRE(selected);
    {
        execution::ExecutionSession session(fixture.value().memory, fixture.value().function_map,
                                            {}, options);
        const auto result = session.run(selected.value());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::ArithmeticOverflow);
        REQUIRE(fixture.value().memory.accounting().live_owned_mappings == 1U);
    }
    REQUIRE(fixture.value().memory.accounting().live_owned_mappings == 0U);
    REQUIRE(fixture.value().memory.accounting().owned_mappings_reclaimed == 1U);
    REQUIRE(fixture.value().memory.region_count() == 1U);
}

TEST_CASE("M30 rejects persistent virtual stack cursor overflow before mapping")
{
    constexpr auto base = std::numeric_limits<memory::GuestAddress>::max() - 0x1007U;
    auto fixture = make_fixture(base, memory::GuestMemoryLimits{0x1000U, 0x1008U, 2U});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.synthetic_stack_size = 0x1000U;
    options.stack_guard_gap = 0x1000U;
    const auto result = run_generation(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::UnsupportedSemantic);
    REQUIRE(result.value().diagnostic.find("guard gap") != std::string::npos);
    REQUIRE(fixture.value().memory.accounting().live_owned_mappings == 0U);
    REQUIRE(fixture.value().memory.region_count() == 1U);
}

TEST_CASE("M30 rejects virtual stack alignment overflow before mapping")
{
    constexpr auto maximum = std::numeric_limits<memory::GuestAddress>::max();
    constexpr auto base = maximum - 0x0fU;
    auto fixture = make_fixture(base, memory::GuestMemoryLimits{0x1000U, 0x1008U, 2U});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.synthetic_stack_size = 0x1000U;
    options.stack_guard_gap = 0U;
    const auto result = run_generation(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::UnsupportedSemantic);
    REQUIRE(result.value().diagnostic.find("checked 64-bit addition overflow") != std::string::npos);
    REQUIRE(fixture.value().memory.accounting().live_owned_mappings == 0U);
    REQUIRE(fixture.value().memory.region_count() == 1U);
}
