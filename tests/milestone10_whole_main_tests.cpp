#include "switchrecomp/analysis/whole_module.hpp"
#include "switchrecomp/runtime/dispatcher.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;

[[nodiscard]] analysis::FunctionSeed seed(std::uint64_t entry,
                                          FunctionDiscoverySource source,
                                          FunctionConfidence confidence)
{
    return analysis::FunctionSeed{entry, source, confidence, std::nullopt, std::nullopt, {}};
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

[[nodiscard]] memory::GuestMemory make_code(std::uint64_t base,
                                             std::span<const std::byte> code)
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(base, code, memory::GuestMemoryPermissions::Read |
                                      memory::GuestMemoryPermissions::Execute,
                        "m10.text", memory::GuestRegionKind::Text));
    return memory;
}

[[nodiscard]] analysis::ModuleAnalysisInput input_for(const memory::GuestMemory& memory,
                                                       std::uint64_t base,
                                                       std::uint64_t size,
                                                       std::vector<analysis::FunctionSeed> seeds)
{
    analysis::ModuleIdentity identity;
    identity.module = "synthetic-main";
    identity.build_id = "synthetic-build";
    identity.input_sha256 = "synthetic-sha256";
    identity.guest_base = base;
    identity.executable_ranges.push_back(analysis::GuestAddressRange{base, size});
    identity.translator_version = version;
    return analysis::ModuleAnalysisInput{std::move(identity), &memory, std::move(seeds)};
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    REQUIRE(offset + 4U <= bytes.size());
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void write_i32(std::vector<std::byte>& bytes, std::size_t offset, std::int32_t value)
{
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
}

void write_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
{
    REQUIRE(offset + 8U <= bytes.size());
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
    write_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] std::vector<std::byte> make_synthetic_nso()
{
    constexpr std::size_t text_file = 0x100U;
    constexpr std::size_t rodata_file = 0x300U;
    constexpr std::size_t data_file = 0x400U;
    std::vector<std::byte> file(0x410U, std::byte{0});
    file[0] = std::byte{'N'};
    file[1] = std::byte{'S'};
    file[2] = std::byte{'O'};
    file[3] = std::byte{'0'};
    write_u32(file, 0x004U, 0U);
    write_u32(file, 0x008U, 0U);
    write_u32(file, 0x00cU, 0U);
    write_u32(file, 0x010U, static_cast<std::uint32_t>(text_file));
    write_u32(file, 0x014U, 0U);
    write_u32(file, 0x018U, 0x200U);
    write_u32(file, 0x01cU, 0U);
    write_u32(file, 0x020U, static_cast<std::uint32_t>(rodata_file));
    write_u32(file, 0x024U, 0x200U);
    write_u32(file, 0x028U, 0x100U);
    write_u32(file, 0x02cU, 0U);
    write_u32(file, 0x030U, static_cast<std::uint32_t>(data_file));
    write_u32(file, 0x034U, 0x300U);
    write_u32(file, 0x038U, 0x10U);
    write_u32(file, 0x03cU, 0U);
    for (std::size_t index = 0U; index < 32U; ++index)
    {
        file[0x040U + index] = static_cast<std::byte>(index + 1U);
    }
    write_u32(file, 0x060U, 0x200U);
    write_u32(file, 0x064U, 0x100U);
    write_u32(file, 0x068U, 0x10U);
    write_u32(file, 0x088U, 0U);
    write_u32(file, 0x08cU, 0U);
    write_u32(file, 0x090U, 0U);
    write_u32(file, 0x094U, 0U);
    write_u32(file, 0x098U, 0U);
    write_u32(file, 0x09cU, 0U);

    std::vector<std::byte> text(0x200U, std::byte{0});
    write_u32(text, 0x000U, 0U);
    write_u32(text, 0x004U, 0x20U);
    write_u32(text, 0x020U, format::mod0_magic);
    write_i32(text, 0x024U, 0x1e0);
    write_i32(text, 0x028U, 0x2e0);
    write_i32(text, 0x02cU, 0x2e0);
    write_i32(text, 0x030U, 0x2e0);
    write_i32(text, 0x034U, 0x2e0);
    write_i32(text, 0x038U, 0x2e0);
    write_u32(text, 0x100U, 0x94000008U); // bl 0x120
    write_u32(text, 0x104U, 0xd65f03c0U); // ret
    write_u32(text, 0x120U, 0x8b010000U); // add x0, x0, x1
    write_u32(text, 0x124U, 0xd65f03c0U); // ret
    std::copy(text.begin(), text.end(), file.begin() + static_cast<std::ptrdiff_t>(text_file));

    write_u64(file, rodata_file, 0U); // DT_NULL
    write_u64(file, rodata_file + 8U, 0U);
    return file;
}

std::uint32_t dispatcher_stub(runtime::CpuState* cpu, runtime::RuntimeContext* runtime)
{
    if (cpu != nullptr && runtime != nullptr) cpu->x[0] = 0x55U;
    return 7U;
}

} // namespace

TEST_CASE("M10 fixed-point discovery records direct BL provenance and ignores unreachable data")
{
    const auto code = words({0x94000008U, 0xd65f03c0U, 0xd503201fU, 0xd503201fU,
                             0xd503201fU, 0xd503201fU, 0xd503201fU, 0xd503201fU,
                             0x8b010000U, 0xd65f03c0U});
    const auto memory = make_code(0x1000U, code);
    const auto input = input_for(memory, 0x1000U, code.size(),
                                 {seed(0x1000U, FunctionDiscoverySource::ModuleEntry,
                                       FunctionConfidence::Confirmed)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    REQUIRE(map.value().frozen());
    REQUIRE(map.value().functions().size() == 2U);
    const auto* called = map.value().find(0x1020U);
    REQUIRE(called != nullptr);
    REQUIRE(called->primary_source == FunctionDiscoverySource::DirectCall);
    REQUIRE(called->confidence == FunctionConfidence::High);
    REQUIRE_FALSE(map.value().find(0x1008U));
    REQUIRE(map.value().functions()[0].direct_calls == std::vector<std::uint64_t>{0x1020U});

    analysis::TranslationOptions diagnostic_options;
    diagnostic_options.mode = analysis::TranslationMode::Diagnostic;
    const auto translated = analysis::translate_module(input, diagnostic_options);
    REQUIRE(translated);
    REQUIRE(translated.value().coverage.functions_translated == 2U);
    REQUIRE(translated.value().coverage.direct_calls == 1U);
    REQUIRE(translated.value().strict_success);
}

TEST_CASE("M10 function map reports boundary conflicts without choosing silently")
{
    const auto code = words({0xd503201fU, 0xd503201fU, 0xd65f03c0U, 0xd503201fU});
    const auto memory = make_code(0x2000U, code);
    const auto input = input_for(
        memory, 0x2000U, code.size(),
        {seed(0x2000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x2004U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    REQUIRE(map.value().conflicts().size() == 1U);
    REQUIRE(map.value().functions().at(0).translation_status == analysis::TranslationStatus::Conflict);
    REQUIRE(map.value().functions().at(1).translation_status == analysis::TranslationStatus::Conflict);
}

TEST_CASE("M10 conflicts normalize canonical identity when range order is reversed")
{
    const auto code = words({0xd65f03c0U, 0xd65f03c0U, 0x17fffffeU});
    const auto memory = make_code(0x6000U, code);
    const auto first = input_for(
        memory, 0x6000U, code.size(),
        {seed(0x6008U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x6004U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low)});
    const auto reversed = input_for(
        memory, 0x6000U, code.size(),
        {seed(0x6004U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low),
         seed(0x6008U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)});

    const auto first_map = analysis::FunctionMapBuilder::build(first);
    const auto reversed_map = analysis::FunctionMapBuilder::build(reversed);
    REQUIRE(first_map);
    REQUIRE(reversed_map);
    REQUIRE(first_map.value().conflicts().empty());
    REQUIRE(analysis::render_function_map_json(first_map.value()) ==
            analysis::render_function_map_json(reversed_map.value()));
}

TEST_CASE("M10 conflict calculation distinguishes adjacency, containment, and duplicate ranges")
{
    const auto adjacent_code = words({0xd65f03c0U, 0xd65f03c0U});
    const auto adjacent_memory = make_code(0x7000U, adjacent_code);
    const auto adjacent = input_for(
        adjacent_memory, 0x7000U, adjacent_code.size(),
        {seed(0x7000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x7004U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low)});
    const auto adjacent_map = analysis::FunctionMapBuilder::build(adjacent);
    REQUIRE(adjacent_map);
    REQUIRE(adjacent_map.value().conflicts().empty());

    const auto duplicate_code = words({0x14000003U, 0x17ffffffU, 0x17fffffeU,
                                       0xd65f03c0U});
    const auto duplicate_memory = make_code(0x8000U, duplicate_code);
    const auto duplicate = input_for(
        duplicate_memory, 0x8000U, duplicate_code.size(),
        {seed(0x8004U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x8008U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low)});
    const auto duplicate_map = analysis::FunctionMapBuilder::build(duplicate);
    REQUIRE(duplicate_map);
    REQUIRE(duplicate_map.value().conflicts().size() == 1U);
    REQUIRE(duplicate_map.value().conflicts().front().first_range.base == 0x8000U);
    REQUIRE(duplicate_map.value().conflicts().front().second_range.base == 0x8000U);
    REQUIRE(duplicate_map.value().conflicts().front().first_range.size ==
            duplicate_map.value().conflicts().front().second_range.size);
}

TEST_CASE("M10 conflict calculation emits complete three-way and chain overlap sets")
{
    const auto three_way_code = words({0x14000004U, 0x17ffffffU, 0x17fffffeU,
                                       0x17fffffdU, 0xd65f03c0U});
    const auto three_way_memory = make_code(0x9000U, three_way_code);
    const auto three_way = input_for(
        three_way_memory, 0x9000U, three_way_code.size(),
        {seed(0x9004U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x9008U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low),
         seed(0x900cU, FunctionDiscoverySource::AnalystSeed, FunctionConfidence::Medium)});
    const auto three_way_map = analysis::FunctionMapBuilder::build(three_way);
    REQUIRE(three_way_map);
    REQUIRE(three_way_map.value().conflicts().size() == 3U);
    REQUIRE(three_way_map.value().conflicts()[0].first_function == 0x9004U);
    REQUIRE(three_way_map.value().conflicts()[0].second_function == 0x9008U);
    REQUIRE(three_way_map.value().conflicts()[1].first_function == 0x9004U);
    REQUIRE(three_way_map.value().conflicts()[1].second_function == 0x900cU);
    REQUIRE(three_way_map.value().conflicts()[2].first_function == 0x9008U);
    REQUIRE(three_way_map.value().conflicts()[2].second_function == 0x900cU);

    const auto chain_code = words({0x14000001U, 0xd65f03c0U, 0xd503201fU,
                                   0xd503201fU, 0xd65f03c0U, 0xd503201fU,
                                   0x17fffffbU});
    const auto chain_memory = make_code(0xa000U, chain_code);
    const auto chain = input_for(
        chain_memory, 0xa000U, chain_code.size(),
        {seed(0xa000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0xa010U, FunctionDiscoverySource::AnalystSeed, FunctionConfidence::Medium),
         seed(0xa018U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low)});
    const auto chain_map = analysis::FunctionMapBuilder::build(chain);
    REQUIRE(chain_map);
    REQUIRE(chain_map.value().conflicts().size() == 1U);
    REQUIRE(chain_map.value().conflicts()[0].first_function == 0xa000U);
    REQUIRE(chain_map.value().conflicts()[0].second_function == 0xa018U);
}

TEST_CASE("M10 late direct-call discovery contributes final boundary conflicts")
{
    const auto code = words({0x94000002U, 0xd65f03c0U, 0x17fffffeU});
    const auto memory = make_code(0xb000U, code);
    const auto input = input_for(
        memory, 0xb000U, code.size(),
        {seed(0xb000U, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    REQUIRE(map.value().find(0xb008U) != nullptr);
    REQUIRE(map.value().find(0xb008U)->translation_status == analysis::TranslationStatus::Analyzed);
    REQUIRE(std::any_of(map.value().find(0xb008U)->evidence.begin(),
                        map.value().find(0xb008U)->evidence.end(), [](const auto& evidence) {
                            return evidence.source == FunctionDiscoverySource::DirectCall;
                        }));
    REQUIRE(map.value().conflicts().empty());
}

TEST_CASE("M10.2 precise ownership normalizes decoded instruction spans")
{
    const auto contiguous = analysis::normalize_code_ranges(
        std::vector<memory::GuestAddress>{0x1008U, 0x1000U, 0x1004U, 0x1004U});
    REQUIRE(contiguous);
    REQUIRE(contiguous.value() ==
            std::vector<analysis::GuestAddressRange>{{0x1000U, 0x0cU}});

    const auto disconnected = analysis::normalize_code_ranges(
        std::vector<memory::GuestAddress>{0x5004U, 0x1000U, 0x5000U, 0x1004U});
    REQUIRE(disconnected);
    REQUIRE(disconnected.value() ==
            std::vector<analysis::GuestAddressRange>{{0x1000U, 0x08U}, {0x5000U, 0x08U}});

    const auto gap = analysis::normalize_code_ranges(
        std::vector<memory::GuestAddress>{0x1000U, 0x1008U});
    REQUIRE(gap);
    REQUIRE(gap.value() ==
            std::vector<analysis::GuestAddressRange>{{0x1000U, 0x04U}, {0x1008U, 0x04U}});

    const auto adjacent = analysis::normalize_code_ranges(
        std::vector<analysis::GuestAddressRange>{{0x1000U, 0x04U}, {0x1004U, 0x04U}});
    REQUIRE(adjacent);
    REQUIRE(adjacent.value() ==
            std::vector<analysis::GuestAddressRange>{{0x1000U, 0x08U}});

    const auto overflow = analysis::normalize_code_ranges(
        std::vector<memory::GuestAddress>{std::numeric_limits<memory::GuestAddress>::max() - 3U});
    REQUIRE_FALSE(overflow);
    REQUIRE(overflow.error().code == ErrorCode::InvalidGuestAddress);
}

TEST_CASE("M10.2 exact ownership conflicts preserve islands and reject envelope overlap")
{
    const auto disjoint = analysis::intersect_owned_ranges(
        std::vector<analysis::GuestAddressRange>{{0x1000U, 0x10U}, {0x5000U, 0x10U}},
        std::vector<analysis::GuestAddressRange>{{0x3000U, 0x10U}});
    REQUIRE(disjoint);
    REQUIRE(disjoint.value().empty());
    REQUIRE_FALSE(analysis::owned_ranges_overlap(
        std::vector<analysis::GuestAddressRange>{{0x1000U, 0x10U}},
        std::vector<analysis::GuestAddressRange>{{0x1010U, 0x10U}}));

    const auto islands = analysis::intersect_owned_ranges(
        std::vector<analysis::GuestAddressRange>{{0x1000U, 0x20U}, {0x2000U, 0x20U}},
        std::vector<analysis::GuestAddressRange>{{0x1010U, 0x20U}, {0x2010U, 0x20U}});
    REQUIRE(islands);
    REQUIRE(islands.value() ==
            std::vector<analysis::GuestAddressRange>{{0x1010U, 0x10U}, {0x2010U, 0x10U}});

    const auto true_overlap = analysis::intersect_owned_ranges(
        std::vector<analysis::GuestAddressRange>{{0x1000U, 0x20U}},
        std::vector<analysis::GuestAddressRange>{{0x1010U, 0x20U}});
    REQUIRE(true_overlap);
    REQUIRE(true_overlap.value() ==
            std::vector<analysis::GuestAddressRange>{{0x1010U, 0x10U}});
}

TEST_CASE("M10.2 convex envelope overlap does not create a function conflict")
{
    std::vector<std::byte> code(0x4004U, std::byte{0});
    write_u32(code, 0x0000U, 0x14000800U); // 0x1000 -> 0x3000
    write_u32(code, 0x1000U, 0xd65f03c0U); // gap function at 0x2000
    write_u32(code, 0x2000U, 0xd65f03c0U); // disconnected high block
    const auto memory = make_code(0x1000U, code);
    const auto input = input_for(
        memory, 0x1000U, code.size(),
        {seed(0x1000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x2000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    const auto* first = map.value().find(0x1000U);
    const auto* second = map.value().find(0x2000U);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first->owned_code_ranges ==
            std::vector<analysis::GuestAddressRange>{{0x1000U, 0x04U}, {0x3000U, 0x04U}});
    REQUIRE(second->owned_code_ranges ==
            std::vector<analysis::GuestAddressRange>{{0x2000U, 0x04U}});
    REQUIRE((first->range_begin < second->range_end &&
             second->range_begin < first->range_end));
    REQUIRE(map.value().conflicts().empty());
    REQUIRE_FALSE(analysis::function_owns_address(*first, 0x2000U));
    REQUIRE(map.value().find_owners(0x2000U).size() == 1U);
}

TEST_CASE("M10.2 precise conflict records contain all overlap islands")
{
    std::vector<std::byte> code(0x4004U, std::byte{0});
    write_u32(code, 0x0000U, 0x14000800U); // 0x1000 -> 0x3000
    write_u32(code, 0x1000U, 0x14000400U); // 0x2000 -> 0x3000
    write_u32(code, 0x2000U, 0x14000800U); // 0x3000 -> 0x5000
    write_u32(code, 0x4000U, 0xd65f03c0U); // 0x5000
    const auto memory = make_code(0x1000U, code);
    const auto input = input_for(
        memory, 0x1000U, code.size(),
        {seed(0x1000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x2000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    REQUIRE(map.value().conflicts().size() == 1U);
    REQUIRE(map.value().conflicts().front().overlap_ranges ==
            std::vector<analysis::GuestAddressRange>{{0x3000U, 0x04U}, {0x5000U, 0x04U}});
    REQUIRE(map.value().find_owners(0x3000U).size() == 2U);
    REQUIRE(map.value().find_owners(0x4000U).empty());
}

TEST_CASE("M10.2 known unconditional branches become function transfers")
{
    std::vector<std::byte> code(0x14U, std::byte{0});
    write_u32(code, 0x0000U, 0x14000004U); // 0x1000 -> 0x1010
    write_u32(code, 0x0010U, 0xd65f03c0U);
    const auto memory = make_code(0x1000U, code);

    const auto internal = analysis::FunctionMapBuilder::build(input_for(
        memory, 0x1000U, code.size(),
        {seed(0x1000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)}));
    REQUIRE(internal);
    REQUIRE(internal.value().find(0x1000U)->cfg->blocks.contains(0x1010U));
    REQUIRE(internal.value().find(0x1000U)->cfg->blocks.at(0x1000U).successors.front().kind ==
            analysis::EdgeKind::Branch);

    const auto external_input = input_for(
        memory, 0x1000U, code.size(),
        {seed(0x1000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x1010U, FunctionDiscoverySource::DynamicSymbol, FunctionConfidence::Confirmed)});
    const auto external = analysis::FunctionMapBuilder::build(external_input);
    REQUIRE(external);
    const auto& transfer = external.value().find(0x1000U)->cfg->blocks.at(0x1000U).successors.front();
    REQUIRE(transfer.kind == analysis::EdgeKind::FunctionTransfer);
    REQUIRE_FALSE(transfer.internal);
    REQUIRE_FALSE(external.value().find(0x1000U)->cfg->blocks.contains(0x1010U));
    REQUIRE(analysis::render_function_map_json(external.value()).find("function_transfer") !=
            std::string::npos);

    const auto self_code = make_code(0x2000U, words({0x14000000U}));
    const auto self = analysis::FunctionMapBuilder::build(input_for(
        self_code, 0x2000U, 4U,
        {seed(0x2000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)}));
    REQUIRE(self);
    REQUIRE(self.value().find(0x2000U)->cfg->blocks.at(0x2000U).successors.front().kind ==
            analysis::EdgeKind::Branch);
    REQUIRE(self.value().find(0x2000U)->cfg->blocks.at(0x2000U).successors.front().internal);
}

TEST_CASE("M10.2 late strong entries trigger boundary-aware re-analysis")
{
    std::vector<std::byte> code(0x38U, std::byte{0});
    write_u32(code, 0x0000U, 0x14000004U); // 0x1000 -> 0x1010
    write_u32(code, 0x0010U, 0xd65f03c0U); // weak seed target
    write_u32(code, 0x0030U, 0x97fffff8U); // 0x1030 BL 0x1010
    write_u32(code, 0x0034U, 0xd65f03c0U);
    const auto memory = make_code(0x1000U, code);
    const auto input = input_for(
        memory, 0x1000U, code.size(),
        {seed(0x1000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x1010U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low),
         seed(0x1030U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    const auto* caller = map.value().find(0x1000U);
    REQUIRE(caller != nullptr);
    REQUIRE_FALSE(caller->cfg->blocks.contains(0x1010U));
    REQUIRE(caller->cfg->blocks.at(0x1000U).successors.front().kind ==
            analysis::EdgeKind::FunctionTransfer);
    REQUIRE(std::any_of(map.value().find(0x1010U)->evidence.begin(),
                        map.value().find(0x1010U)->evidence.end(), [](const auto& evidence) {
                            return evidence.source == FunctionDiscoverySource::DirectCall;
                        }));
}

TEST_CASE("M10.2 text start remains an unverified entry candidate")
{
    analysis::PreparedModuleOptions options;
    options.module_name = "synthetic-main.nso";
    options.module_base = 0x100000U;
    const auto loaded = analysis::load_prepared_nso(make_synthetic_nso(), options);
    REQUIRE(loaded);
    REQUIRE(loaded.value().identity.metadata_schema_version == 2U);
    REQUIRE(loaded.value().identity.entry_points.size() == 1U);
    REQUIRE(loaded.value().identity.entry_points.front().kind ==
            analysis::EntryPointKind::TextStartCandidate);
    REQUIRE_FALSE(loaded.value().identity.entry_points.front().verified_runtime_entry);
    REQUIRE(loaded.value().seeds.front().source == FunctionDiscoverySource::TextStartCandidate);
    REQUIRE(loaded.value().seeds.front().confidence == FunctionConfidence::Low);

    analysis::TranslationOptions translation_options;
    translation_options.mode = analysis::TranslationMode::Diagnostic;
    const auto translated = analysis::translate_module(loaded.value(), translation_options);
    REQUIRE(translated);
    const auto report = analysis::render_translation_report_json(translated.value());
    REQUIRE(report.find("\"verified_process_entry\": null") != std::string::npos);
    REQUIRE(report.find("text_start_candidate") != std::string::npos);
    REQUIRE(report.find("\"verified_runtime_entry\": false") != std::string::npos);
}

TEST_CASE("M10 discovery rejects invalid seeds and enforces bounded fixed-point growth")
{
    const auto code = words({0x94000004U, 0xd65f03c0U, 0xd503201fU,
                             0x8b010000U, 0xd65f03c0U});
    const auto memory = make_code(0x3000U, code);
    auto invalid = input_for(memory, 0x3000U, code.size(),
                             {seed(0x3002U, FunctionDiscoverySource::ManualOverride,
                                   FunctionConfidence::Manual)});
    REQUIRE_FALSE(analysis::FunctionMapBuilder::build(invalid));
    REQUIRE(analysis::FunctionMapBuilder::build(invalid).error().code == ErrorCode::InvalidGuestAddress);

    auto limited = input_for(memory, 0x3000U, code.size(),
                             {seed(0x3000U, FunctionDiscoverySource::ModuleEntry,
                                   FunctionConfidence::Confirmed)});
    analysis::FunctionMapOptions options;
    options.budgets.max_functions = 1U;
    const auto result = analysis::FunctionMapBuilder::build(limited, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::AnalysisBudgetExceeded);
}

TEST_CASE("M10 strict and diagnostic translation distinguish unsupported instructions")
{
    const auto code = words({0xc87f7c20U, 0xd65f03c0U, 0x8b010000U, 0xd65f03c0U});
    const auto memory = make_code(0x4000U, code);
    const auto input = input_for(
        memory, 0x4000U, code.size(),
        {seed(0x4000U, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed),
         seed(0x4008U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)});

    const auto strict = analysis::translate_module(input);
    REQUIRE(strict);
    REQUIRE_FALSE(strict.value().strict_success);
    REQUIRE(strict.value().coverage.functions_unsupported == 1U);
    REQUIRE(strict.value().coverage.functions_translated == 0U);
    REQUIRE(strict.value().functions.size() == 2U);
    REQUIRE(strict.value().functions[1].status == analysis::TranslationStatus::Excluded);

    analysis::TranslationOptions diagnostic_options;
    diagnostic_options.mode = analysis::TranslationMode::Diagnostic;
    const auto diagnostic = analysis::translate_module(input, diagnostic_options);
    REQUIRE(diagnostic);
    REQUIRE_FALSE(diagnostic.value().strict_success);
    REQUIRE(diagnostic.value().coverage.functions_unsupported == 1U);
    REQUIRE(diagnostic.value().coverage.functions_translated == 1U);
    REQUIRE(diagnostic.value().coverage.unsupported_instructions == 1U);
    REQUIRE(diagnostic.value().coverage.top_unsupported.front().instruction == "ldxp");
}

TEST_CASE("M10 frozen guest dispatcher validates guest addresses and is safe for concurrent reads")
{
    const auto code = words({0xd503201fU, 0xd65f03c0U});
    const auto memory = make_code(0x5000U, code);
    runtime::GuestFunctionRegistry registry(memory);
    REQUIRE(registry.add(runtime::FunctionRegistration{0x5000U, 0x5000U, 0x5008U,
                                                       "synthetic-main", dispatcher_stub}));
    REQUIRE(registry.freeze());
    REQUIRE(registry.frozen());
    REQUIRE(registry.lookup(0x5000U));
    REQUIRE_FALSE(registry.lookup(0x5004U));
    REQUIRE(registry.lookup(0x5002U).error().code == ErrorCode::InvalidGuestAddress);
    REQUIRE(registry.lookup(0x6000U).error().code == ErrorCode::UnknownGuestFunction);

    runtime::CpuState cpu{};
    runtime::RuntimeContext context{const_cast<memory::GuestMemory*>(&memory)};
    REQUIRE(registry.dispatch(0x5000U, cpu, context));
    REQUIRE(cpu.x[0] == 0x55U);
    REQUIRE(registry.add(runtime::FunctionRegistration{0x5004U, 0x5000U, 0x5008U,
                                                       "synthetic-main", dispatcher_stub}).error().code ==
            ErrorCode::FunctionRegistryFrozen);

    std::atomic<unsigned int> failures{0U};
    std::vector<std::thread> workers;
    for (unsigned int index = 0U; index < 8U; ++index)
    {
        workers.emplace_back([&registry, &failures] {
            for (unsigned int iteration = 0U; iteration < 1000U; ++iteration)
            {
                if (!registry.lookup(0x5000U)) ++failures;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(failures.load() == 0U);
}

TEST_CASE("M10 synthetic NSO crosses loading, MOD0, dynamic metadata, discovery and reporting")
{
    constexpr std::uint64_t module_base = 0x100000U;
    auto bytes = make_synthetic_nso();
    analysis::PreparedModuleOptions options;
    options.module_name = "synthetic-main.nso";
    options.module_base = module_base;
    options.seed_text_entry = false;
    options.seeds.push_back(analysis::FunctionSeed{
        module_base + 0x100U, FunctionDiscoverySource::AnalystSeed,
        FunctionConfidence::Manual, std::nullopt, std::nullopt, "synthetic entry"});
    const auto loaded = analysis::load_prepared_nso(bytes, options);
    if (!loaded) UNSCOPED_INFO(loaded.error().message);
    REQUIRE(loaded);
    REQUIRE(loaded.value().metadata.mod0.has_value());
    REQUIRE(loaded.value().metadata.dynamic.has_value());
    REQUIRE_FALSE(loaded.value().symbols.has_value());
    REQUIRE(loaded.value().identity.executable_ranges.size() == 1U);

    analysis::TranslationOptions translation_options;
    translation_options.mode = analysis::TranslationMode::Diagnostic;
    const auto result = analysis::translate_module(loaded.value(), translation_options);
    REQUIRE(result);
    REQUIRE(result.value().coverage.functions_discovered == 2U);
    REQUIRE(result.value().coverage.functions_translated == 2U);
    REQUIRE(result.value().coverage.direct_calls == 1U);
    REQUIRE(result.value().coverage.bytes_analyzed > 0U);
    const auto first_report = analysis::render_translation_report_json(result.value());
    const auto second_report = analysis::render_translation_report_json(result.value());
    REQUIRE(first_report == second_report);
    REQUIRE(first_report.find("/workspace") == std::string::npos);
    REQUIRE(first_report.find("\"bytes_analyzed\"") != std::string::npos);
    REQUIRE(first_report.find("synthetic-main.nso") != std::string::npos);
}
