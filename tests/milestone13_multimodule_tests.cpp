#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace switchrecomp;

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

[[nodiscard]] std::vector<std::byte> minimal_nso(
    std::initializer_list<std::uint32_t> code_words = {0U, 0U})
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
    const auto code = words(code_words);
    // The first eight bytes of a Switch text image are ModuleStart metadata.
    // Keep them zero for a synthetic no-MOD0 module and place test code after it.
    std::copy(code.begin(), code.end(), file.begin() + static_cast<std::ptrdiff_t>(text_file + 8U));
    return file;
}

[[nodiscard]] analysis::FinalizedFunctionMap make_map(const memory::GuestMemory& memory,
                                                       std::string module,
                                                       memory::GuestAddress base,
                                                       memory::GuestSize size)
{
    analysis::ModuleIdentity identity;
    identity.module = std::move(module);
    identity.build_id = "synthetic-build";
    identity.input_sha256 = "synthetic-sha";
    identity.guest_base = base;
    identity.guest_base_provenance = analysis::ModuleBaseProvenance::ExplicitAnalysisBase;
    identity.translator_version = version;
    identity.executable_ranges.push_back(analysis::GuestAddressRange{base, size});
    identity.entry_points.push_back(analysis::EntryPointEvidence{
        base, analysis::EntryPointKind::DynamicInit, "synthetic DT_INIT",
        analysis::FunctionConfidence::High, false, "synthetic process entry"});
    analysis::FunctionSeed seed{base, analysis::FunctionDiscoverySource::ModuleEntry,
                                analysis::FunctionConfidence::Confirmed, std::nullopt,
                                std::nullopt, "synthetic module entry"};
    const auto result = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &memory, {seed}});
    REQUIRE(result);
    return std::move(result).value();
}

TEST_CASE("M13 deterministic process layout is complete, ordered, and provenance-labelled")
{
    const auto first_bytes = minimal_nso();
    const auto second_bytes = minimal_nso();
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"provider", second_bytes, std::nullopt},
        analysis::ProcessModuleInput{"main", first_bytes, std::nullopt}};
    analysis::ProcessImageOptions options;
    options.primary_module = "main";
    options.provider_search_complete = true;
    const auto first = analysis::load_process_image(inputs, options);
    if (!first) UNSCOPED_INFO(error_code_name(first.error().code) << ": " << first.error().message);
    REQUIRE(first);
    const auto second = analysis::load_process_image(inputs, options);
    REQUIRE(second);
    REQUIRE(first.value().modules().size() == 2U);
    REQUIRE(first.value().modules()[0].identity.module == "main");
    REQUIRE(first.value().modules()[1].identity.module == "provider");
    REQUIRE(first.value().modules()[0].base_provenance ==
            analysis::ModuleBaseProvenance::DeterministicAnalysisLayout);
    REQUIRE(first.value().modules()[0].identity.guest_base ==
            second.value().modules()[0].identity.guest_base);
    REQUIRE(first.value().modules()[1].identity.guest_base ==
            second.value().modules()[1].identity.guest_base);
    REQUIRE_FALSE(first.value().modules()[0].identity.guest_base_verified);
    REQUIRE(first.value().modules()[0].identity.executable_ranges.size() == 1U);
    REQUIRE(first.value().summary().modules[0].segments.size() == 3U);
    REQUIRE(first.value().summary().modules[0].bss_size == 0U);
    REQUIRE(first.value().module_for_address(first.value().modules()[1].identity.guest_base, 4U) !=
            nullptr);

    const std::array<analysis::ProcessModuleInput, 1> explicit_input{
        analysis::ProcessModuleInput{"main", first_bytes, 0x500000U}};
    const auto explicit_process = analysis::load_process_image(explicit_input);
    REQUIRE(explicit_process);
    REQUIRE(explicit_process.value().modules()[0].base_provenance ==
            analysis::ModuleBaseProvenance::ExplicitAnalysisBase);
}

TEST_CASE("M13 process layout rejects overlap, overflow, and reserved-stack collision")
{
    const auto bytes = minimal_nso();
    const std::array<analysis::ProcessModuleInput, 2> overlap{
        analysis::ProcessModuleInput{"a", bytes, 0x100000U},
        analysis::ProcessModuleInput{"b", bytes, 0x100000U}};
    analysis::ProcessImageOptions overlap_options;
    overlap_options.primary_module = "a";
    auto result = analysis::load_process_image(overlap, overlap_options);
    if (result) UNSCOPED_INFO("overlap unexpectedly succeeded");
    else UNSCOPED_INFO(error_code_name(result.error().code) << ": " << result.error().message);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::ModuleAddressOverlap);

    const std::array<analysis::ProcessModuleInput, 1> overflow{
        analysis::ProcessModuleInput{"main", bytes,
                                     std::numeric_limits<std::uint64_t>::max() - 0xfffU}};
    result = analysis::load_process_image(overflow);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::ModuleLayoutOverflow);

    analysis::ProcessImageOptions options;
    options.reserved_stack_base = 0x7200000000ULL;
    options.reserved_stack_size = 0x10000U;
    const std::array<analysis::ProcessModuleInput, 1> stack_collision{
        analysis::ProcessModuleInput{"main", bytes, std::nullopt}};
    result = analysis::load_process_image(stack_collision, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::ModuleAddressOverlap);

    const std::array<analysis::ProcessModuleInput, 2> duplicate_names{
        analysis::ProcessModuleInput{"main", bytes, std::nullopt},
        analysis::ProcessModuleInput{"main", bytes, std::nullopt}};
    result = analysis::load_process_image(duplicate_names);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::DuplicateModuleIdentity);
}

TEST_CASE("M13 provider namespace uses the provider base and rejects hidden or undefined symbols")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x2000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "provider.text", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable provider;
    provider.symbols.push_back(format::DynamicSymbol{3U, 0U, "guest_fn",
                                                     format::SymbolBinding::Global,
                                                     format::SymbolType::Function,
                                                     format::SymbolVisibility::Default,
                                                     1U, 0x10U, 4U});
    provider.symbols.push_back(format::DynamicSymbol{4U, 0U, "hidden_fn",
                                                     format::SymbolBinding::Global,
                                                     format::SymbolType::Function,
                                                     format::SymbolVisibility::Hidden,
                                                     1U, 0x14U, 4U});
    provider.symbols.push_back(format::DynamicSymbol{5U, 0U, "guest_fn",
                                                     format::SymbolBinding::Global,
                                                     format::SymbolType::Function,
                                                     format::SymbolVisibility::Default,
                                                     0U, 0U, 0U});
    const std::array<analysis::ProcessSymbolSource, 1> sources{
        analysis::ProcessSymbolSource{"provider", 0x2000U, &provider}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build(memory, sources);
    REQUIRE(namespace_result);
    const auto resolved = namespace_result.value().lookup("guest_fn", true);
    REQUIRE(resolved.status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(resolved.selected_candidate);
    REQUIRE(resolved.candidates[resolved.selected_candidate.value()].address == 0x2010U);
    REQUIRE(namespace_result.value().lookup("hidden_fn", true).status ==
            analysis::ProviderResolutionStatus::NotFoundInSuppliedModules);
    REQUIRE(namespace_result.value().lookup("missing", false).status ==
            analysis::ProviderResolutionStatus::ProviderSearchIncomplete);

    REQUIRE(memory.map(0x3000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "provider2.text", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable provider2;
    provider2.symbols.push_back(format::DynamicSymbol{7U, 0U, "guest_fn",
                                                       format::SymbolBinding::Global,
                                                       format::SymbolType::Function,
                                                       format::SymbolVisibility::Default,
                                                       1U, 0x10U, 4U});
    const std::array<analysis::ProcessSymbolSource, 2> ambiguous_sources{
        analysis::ProcessSymbolSource{"provider", 0x2000U, &provider},
        analysis::ProcessSymbolSource{"provider2", 0x3000U, &provider2}};
    const auto ambiguous_namespace =
        analysis::ProcessSymbolNamespace::build(memory, ambiguous_sources);
    REQUIRE(ambiguous_namespace);
    const auto ambiguous = ambiguous_namespace.value().lookup("guest_fn", true);
    REQUIRE(ambiguous.status == analysis::ProviderResolutionStatus::AmbiguousGuestProvider);
    REQUIRE(ambiguous.candidates.size() == 2U);

    REQUIRE(memory.map(0x4000U, 0x40U, memory::GuestMemoryPermissions::Read,
                       "bad-provider", memory::GuestRegionKind::Rodata));
    format::DynamicSymbolTable bad_provider;
    bad_provider.symbols.push_back(format::DynamicSymbol{8U, 0U, "bad_fn",
                                                         format::SymbolBinding::Global,
                                                         format::SymbolType::Function,
                                                         format::SymbolVisibility::Default,
                                                         1U, 0x10U, 4U});
    const std::array<analysis::ProcessSymbolSource, 1> bad_sources{
        analysis::ProcessSymbolSource{"bad", 0x4000U, &bad_provider}};
    const auto invalid_namespace = analysis::ProcessSymbolNamespace::build(memory, bad_sources);
    REQUIRE_FALSE(invalid_namespace);
    REQUIRE(invalid_namespace.error().code == ErrorCode::InvalidProviderDefinition);
}

TEST_CASE("M13 BL through PLT-style BR reaches another module with tail-return semantics")
{
    constexpr memory::GuestAddress caller_base = 0x1000U;
    constexpr memory::GuestAddress provider_base = 0x2000U;
    constexpr memory::GuestAddress slot = 0x8000U;
    memory::GuestMemory memory;
    const auto caller_code = words({0x94000002U, 0xd65f03c0U, 0xd2900010U,
                                    0xf9400211U, 0xd61f0220U});
    const auto provider_code = words({0xd65f03c0U});
    REQUIRE(memory.map(caller_base, caller_code,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "caller.text", memory::GuestRegionKind::Text));
    REQUIRE(memory.map(provider_base, provider_code,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "provider.text", memory::GuestRegionKind::Text));
    std::array<std::byte, 8> slot_bytes{};
    for (std::size_t index = 0U; index < slot_bytes.size(); ++index)
        slot_bytes[index] = static_cast<std::byte>((provider_base >> (index * 8U)) & 0xffU);
    REQUIRE(memory.map(slot, slot_bytes,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "caller.got", memory::GuestRegionKind::Data));

    auto caller_map = make_map(memory, "caller", caller_base, caller_code.size());
    auto provider_map = make_map(memory, "provider", provider_base, provider_code.size());
    const auto process_map = analysis::ProcessFunctionMap::build(
        {std::move(caller_map), std::move(provider_map)});
    REQUIRE(process_map);
    const auto selected = execution::select_entry(
        process_map.value().maps().front().identity(), execution::EntrySelectionKind::DynamicInit);
    REQUIRE(selected);
    execution::ExecutionSession session(
        memory, process_map.value(), std::vector<loader::UnresolvedRelocation>{});
    const auto run = session.run(selected.value());
    REQUIRE(run);
    if (run && run.value().stop_reason != execution::ExecutionStopReason::EntryReturned)
        UNSCOPED_INFO(run.value().diagnostic);
    REQUIRE(run.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(run.value().direct_calls == 1U);
    REQUIRE(run.value().function_transfers == 1U);
    REQUIRE(run.value().returns == 2U);
    REQUIRE(run.value().runtime.imports_encountered == 0U);
    REQUIRE(run.value().executed_function_modules.size() == 3U);
    REQUIRE(run.value().executed_function_modules[2] == "provider");
    REQUIRE(run.value().maximum_call_depth == 1U);
}

TEST_CASE("M13 process image executes a direct cross-module guest call")
{
    constexpr memory::GuestAddress main_base = 0x100000U;
    constexpr memory::GuestAddress provider_base = 0x200000U;
    const auto main_bytes = minimal_nso({0x94040000U, 0xd65f03c0U});
    const auto provider_bytes = minimal_nso({0xd65f03c0U});
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"main", main_bytes, main_base},
        analysis::ProcessModuleInput{"provider", provider_bytes, provider_base}};
    analysis::ProcessImageOptions image_options;
    image_options.primary_module = "main";
    image_options.provider_search_complete = true;
    image_options.module_options.seed_text_entry = false;
    auto image = analysis::load_process_image(inputs, image_options);
    if (!image) UNSCOPED_INFO(error_code_name(image.error().code) << ": " << image.error().message);
    REQUIRE(image);

    std::vector<analysis::FinalizedFunctionMap> maps;
    for (const auto& module : image.value().modules())
    {
        auto identity = module.identity;
        const auto entry = module.identity.guest_base + 8U;
        identity.entry_points.push_back(analysis::EntryPointEvidence{
            entry, analysis::EntryPointKind::DynamicInit, "synthetic DT_INIT",
            analysis::FunctionConfidence::High, false, "synthetic cross-module test entry"});
        auto seeds = module.seeds;
        seeds.push_back(analysis::FunctionSeed{
            entry, analysis::FunctionDiscoverySource::ModuleEntry,
            analysis::FunctionConfidence::Confirmed, std::nullopt, std::nullopt,
            "synthetic cross-module test entry"});
        const auto map = analysis::FunctionMapBuilder::build(
            analysis::ModuleAnalysisInput{std::move(identity), &image.value().memory(), std::move(seeds)});
        REQUIRE(map);
        maps.push_back(std::move(map).value());
    }
    const auto process_map = analysis::ProcessFunctionMap::build(std::move(maps));
    REQUIRE(process_map);
    const auto* main_map = [&]() -> const analysis::FinalizedFunctionMap* {
        for (const auto& map : process_map.value().maps())
        {
            if (map.identity().module == "main") return &map;
        }
        return nullptr;
    }();
    REQUIRE(main_map != nullptr);
    const auto selected = execution::select_entry(
        main_map->identity(), execution::EntrySelectionKind::DynamicInit);
    if (!selected) UNSCOPED_INFO(error_code_name(selected.error().code) << ": " << selected.error().message);
    REQUIRE(selected);

    execution::ExecutionSession session(image.value().memory(), process_map.value(), image.value());
    const auto run = session.run(selected.value());
    REQUIRE(run);
    if (run && run.value().stop_reason != execution::ExecutionStopReason::EntryReturned)
        UNSCOPED_INFO(run.value().diagnostic);
    REQUIRE(run.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(run.value().direct_calls == 1U);
    REQUIRE(run.value().function_transfers == 0U);
    REQUIRE(run.value().returns == 2U);
    REQUIRE(run.value().runtime.imports_encountered == 0U);
    REQUIRE(run.value().executed_function_modules.size() == 2U);
    REQUIRE(run.value().executed_function_modules[1] == "provider");
    REQUIRE(run.value().process);
    REQUIRE(run.value().process->modules.size() == 2U);
}

TEST_CASE("M13 process function map rejects duplicate guest ownership")
{
    memory::GuestMemory memory;
    const auto code = words({0xd65f03c0U});
    REQUIRE(memory.map(0x1000U, code,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "text", memory::GuestRegionKind::Text));
    auto first = make_map(memory, "a", 0x1000U, code.size());
    auto second = make_map(memory, "b", 0x1000U, code.size());
    const auto result = analysis::ProcessFunctionMap::build(
        {std::move(first), std::move(second)});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::FunctionBoundaryConflict);
}

} // namespace
