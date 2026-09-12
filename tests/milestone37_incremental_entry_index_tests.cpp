#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;

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

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::vector<std::byte> minimal_nso()
{
    constexpr std::size_t text_file = 0x100U;
    constexpr std::size_t rodata_file = 0x200U;
    constexpr std::size_t data_file = 0x300U;
    std::vector<std::byte> file(0x310U, std::byte{0});
    file[0] = std::byte{'N'};
    file[1] = std::byte{'S'};
    file[2] = std::byte{'O'};
    file[3] = std::byte{'0'};
    write_u32(file, 0x10U, text_file);
    write_u32(file, 0x14U, 0U);
    write_u32(file, 0x18U, 0x20U);
    write_u32(file, 0x20U, rodata_file);
    write_u32(file, 0x24U, 0x100U);
    write_u32(file, 0x28U, 0x10U);
    write_u32(file, 0x30U, data_file);
    write_u32(file, 0x34U, 0x200U);
    write_u32(file, 0x38U, 0x10U);
    write_u32(file, 0x3cU, 0U);
    for (std::size_t index = 0U; index < 32U; ++index)
        file[0x40U + index] = static_cast<std::byte>(index + 1U);
    write_u32(file, 0x60U, 0x20U);
    write_u32(file, 0x64U, 0x10U);
    write_u32(file, 0x68U, 0x10U);
    const auto code = words({0xd65f03c0U, 0xd65f03c0U, 0xd65f03c0U});
    std::copy(code.begin(), code.end(),
              file.begin() + static_cast<std::ptrdiff_t>(text_file + 8U));
    return file;
}

[[nodiscard]] analysis::FinalizedFunctionMap make_map(
    const memory::GuestMemory& memory, std::string module, memory::GuestAddress base,
    memory::GuestSize executable_size, std::initializer_list<memory::GuestAddress> entries)
{
    analysis::ModuleIdentity identity;
    identity.module = std::move(module);
    identity.build_id = "synthetic-build";
    identity.input_sha256 = "synthetic-sha";
    identity.guest_base = base;
    identity.guest_base_provenance = analysis::ModuleBaseProvenance::ExplicitAnalysisBase;
    identity.translator_version = version;
    identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, executable_size});

    std::vector<analysis::FunctionSeed> seeds;
    for (const auto entry : entries)
    {
        identity.entry_points.push_back({entry, analysis::EntryPointKind::DynamicInit,
                                         "synthetic M37 entry", FunctionConfidence::High, false,
                                         "synthetic process entry"});
        seeds.push_back({entry, FunctionDiscoverySource::ModuleEntry,
                         FunctionConfidence::Confirmed, std::nullopt, std::nullopt,
                         "synthetic M37 entry"});
    }
    const auto result = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &memory, std::move(seeds)});
    REQUIRE(result);
    return std::move(result).value();
}

} // namespace

TEST_CASE("M37 replacing one module preserves immutable untouched maps and lookups")
{
    const auto bytes = minimal_nso();
    const std::array<analysis::ProcessModuleInput, 2U> inputs{
        analysis::ProcessModuleInput{"main", bytes, 0x100000U},
        analysis::ProcessModuleInput{"provider", bytes, 0x200000U}};
    analysis::ProcessImageOptions image_options;
    image_options.primary_module = "main";
    image_options.provider_search_complete = true;
    image_options.module_options.seed_text_entry = false;
    const auto image = analysis::load_process_image(inputs, image_options);
    REQUIRE(image);

    std::vector<analysis::FinalizedFunctionMap> maps;
    maps.push_back(make_map(image.value().memory(), "main", 0x100000U, 0x20U,
                            {0x100008U}));
    maps.push_back(make_map(image.value().memory(), "provider", 0x200000U, 0x20U,
                            {0x200008U}));
    const auto process = analysis::ProcessFunctionMap::build(std::move(maps));
    REQUIRE(process);

    const auto* provider_before = process.value().map_for(0x200008U);
    const auto* main_before = process.value().map_for(0x100008U);
    REQUIRE(provider_before != nullptr);
    REQUIRE(main_before != nullptr);

    auto replacement = make_map(image.value().memory(), "main", 0x100000U, 0x20U,
                                {0x100008U, 0x10000cU});
    const auto refined = analysis::ProcessFunctionMap::replace_module(
        process.value(), "main", std::move(replacement));
    REQUIRE(refined);

    REQUIRE(refined.value().find(0x100008U) != nullptr);
    REQUIRE(refined.value().find(0x10000cU) != nullptr);
    REQUIRE(refined.value().find(0x200008U) != nullptr);
    REQUIRE(refined.value().map_for(0x200008U) == provider_before);
    REQUIRE(refined.value().map_for(0x100008U) != main_before);
    REQUIRE(process.value().find(0x10000cU) == nullptr);
    REQUIRE(process.value().find(0x200008U) != nullptr);
}

TEST_CASE("M37 changed module layouts use the complete index construction path")
{
    const auto bytes = minimal_nso();
    const std::array<analysis::ProcessModuleInput, 2U> inputs{
        analysis::ProcessModuleInput{"main", bytes, 0x100000U},
        analysis::ProcessModuleInput{"provider", bytes, 0x300000U}};
    analysis::ProcessImageOptions image_options;
    image_options.primary_module = "main";
    image_options.provider_search_complete = true;
    image_options.module_options.seed_text_entry = false;
    const auto image = analysis::load_process_image(inputs, image_options);
    REQUIRE(image);

    std::vector<analysis::FinalizedFunctionMap> maps;
    maps.push_back(make_map(image.value().memory(), "main", 0x100000U, 0x20U,
                            {0x100008U}));
    maps.push_back(make_map(image.value().memory(), "provider", 0x300000U, 0x20U,
                            {0x300008U}));
    const auto process = analysis::ProcessFunctionMap::build(std::move(maps));
    REQUIRE(process);

    auto replacement = make_map(image.value().memory(), "main", 0x100000U, 0x1cU,
                                {0x100008U});
    const auto refined = analysis::ProcessFunctionMap::replace_module(
        process.value(), "main", std::move(replacement));
    REQUIRE(refined);
    REQUIRE(refined.value().find(0x100008U) != nullptr);
    REQUIRE(refined.value().find(0x300008U) != nullptr);
}
