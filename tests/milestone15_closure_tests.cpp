#include "switchrecomp/analysis/module_set.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/execution/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void write_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
{
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
    write_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] std::vector<std::byte> minimal_nso(std::uint8_t marker)
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
    write_u32(file, 0x18U, 0x20U);
    write_u32(file, 0x20U, rodata_file);
    write_u32(file, 0x24U, 0x100U);
    write_u32(file, 0x28U, 0x10U);
    write_u32(file, 0x30U, data_file);
    write_u32(file, 0x34U, 0x200U);
    write_u32(file, 0x38U, 0x10U);
    file[0x40U] = static_cast<std::byte>(marker);
    write_u32(file, 0x60U, 0x20U);
    write_u32(file, 0x64U, 0x10U);
    write_u32(file, 0x68U, 0x10U);
    write_u32(file, text_file + 8U, 0xd65f03c0U);
    return file;
}

[[nodiscard]] std::vector<std::byte> dynamic_nso(bool defines_provider, std::uint8_t marker,
                                                 bool caller = false, bool weak_provider = false)
{
    constexpr std::size_t text_file = 0x100U;
    constexpr std::size_t rodata_file = 0x200U;
    constexpr std::size_t data_file = 0x500U;
    constexpr std::size_t data_offset = 0x8000U;
    constexpr std::size_t string_offset = 0x1a0U;
    constexpr std::size_t hash_offset = 0x1c0U;
    constexpr std::size_t symbol_offset = 0x200U;
    constexpr std::size_t relocation_offset = 0x260U;
    const std::string symbol_name = "__nnmusl_init_dso";
    std::vector<std::byte> file(0x600U, std::byte{0});
    file[0] = std::byte{'N'};
    file[1] = std::byte{'S'};
    file[2] = std::byte{'O'};
    file[3] = std::byte{'0'};
    write_u32(file, 0x10U, text_file);
    write_u32(file, 0x18U, 0x100U);
    write_u32(file, 0x20U, rodata_file);
    write_u32(file, 0x24U, 0x100U);
    write_u32(file, 0x28U, 0x300U);
    write_u32(file, 0x30U, data_file);
    write_u32(file, 0x34U, static_cast<std::uint32_t>(data_offset));
    write_u32(file, 0x38U, 0x100U);
    file[0x40U] = static_cast<std::byte>(marker);
    write_u32(file, text_file + 4U, 0x20U);
    write_u32(file, text_file + 0x20U, format::mod0_magic);
    write_u32(file, text_file + 0x24U, 0xe0U);
    write_u32(file, text_file + 0x28U, 0x7fe0U);
    write_u32(file, text_file + 0x2cU, 0x7fe0U);
    write_u32(file, text_file + 0x30U, 0x7fe0U);
    write_u32(file, text_file + 0x34U, 0x7fe0U);
    write_u32(file, text_file + 0x38U, 0x7fe0U);
    write_u32(file, text_file + 0x40U, 0xd65f03c0U);
    if (caller)
    {
        write_u32(file, text_file + 0x40U, 0x94000008U); // BL .plt
        write_u32(file, text_file + 0x44U, 0xd65f03c0U); // RET
        write_u32(file, text_file + 0x60U, 0xd2900410U); // MOVZ X16, #0x8020
        write_u32(file, text_file + 0x64U, 0xf9400211U); // LDR X17, [X16]
        write_u32(file, text_file + 0x68U, 0xd61f0220U); // BR X17
    }

    const auto dynamic_file = rodata_file;
    const auto dynamic_entry = [&](std::size_t index, std::uint64_t tag, std::uint64_t value) {
        write_u64(file, dynamic_file + index * 16U, tag);
        write_u64(file, dynamic_file + index * 16U + 8U, value);
    };
    dynamic_entry(0U, static_cast<std::uint64_t>(format::DynamicTag::DT_HASH), hash_offset);
    dynamic_entry(1U, static_cast<std::uint64_t>(format::DynamicTag::DT_STRTAB), string_offset);
    dynamic_entry(2U, static_cast<std::uint64_t>(format::DynamicTag::DT_STRSZ), symbol_name.size() + 2U);
    dynamic_entry(3U, static_cast<std::uint64_t>(format::DynamicTag::DT_SYMTAB), symbol_offset);
    dynamic_entry(4U, static_cast<std::uint64_t>(format::DynamicTag::DT_SYMENT), 24U);
    dynamic_entry(5U, static_cast<std::uint64_t>(format::DynamicTag::DT_JMPREL), relocation_offset);
    dynamic_entry(6U, static_cast<std::uint64_t>(format::DynamicTag::DT_PLTRELSZ), 24U);
    dynamic_entry(7U, static_cast<std::uint64_t>(format::DynamicTag::DT_PLTREL),
                  static_cast<std::uint64_t>(format::DynamicTag::DT_RELA));
    if (caller)
        dynamic_entry(8U, static_cast<std::uint64_t>(format::DynamicTag::DT_INIT), 0x40U);
    else
        dynamic_entry(8U, static_cast<std::uint64_t>(format::DynamicTag::DT_NULL), 0U);
    if (caller) dynamic_entry(9U, static_cast<std::uint64_t>(format::DynamicTag::DT_NULL), 0U);

    const auto string_file = rodata_file + (string_offset - 0x100U);
    std::copy(symbol_name.begin(), symbol_name.end(),
              reinterpret_cast<char*>(file.data() + string_file + 1U));
    const auto hash_file = rodata_file + (hash_offset - 0x100U);
    write_u32(file, hash_file, 1U);
    write_u32(file, hash_file + 4U, 2U);
    write_u32(file, hash_file + 8U, 1U);
    write_u32(file, hash_file + 12U, 0U);
    write_u32(file, hash_file + 16U, 1U);
    const auto symbol_file = rodata_file + (symbol_offset - 0x100U) + 24U;
    write_u32(file, symbol_file, 1U);
    file[symbol_file + 4U] = static_cast<std::byte>(weak_provider ? 0x22U : 0x12U);
    write_u32(file, symbol_file + 6U, defines_provider ? 1U : 0U);
    write_u64(file, symbol_file + 8U, defines_provider ? 0x40U : 0U);
    write_u64(file, symbol_file + 16U, defines_provider ? 4U : 0U);
    const auto relocation_file = rodata_file + (relocation_offset - 0x100U);
    write_u64(file, relocation_file, data_offset + 0x20U);
    write_u64(file, relocation_file + 8U, (std::uint64_t{1U} << 32U) | 1026U);
    write_u64(file, relocation_file + 16U, 0U);
    write_u32(file, 0x60U, 0x100U);
    write_u32(file, 0x64U, 0x300U);
    write_u32(file, 0x68U, 0x100U);
    write_u32(file, 0x88U, 0U);
    write_u32(file, 0x8cU, 0U);
    write_u32(file, 0x90U, static_cast<std::uint32_t>(string_offset));
    write_u32(file, 0x94U, static_cast<std::uint32_t>(symbol_name.size() + 2U));
    write_u32(file, 0x98U, static_cast<std::uint32_t>(symbol_offset));
    write_u32(file, 0x9cU, 48U);
    return file;
}

[[nodiscard]] analysis::ModuleSetByteInput input(std::string name,
                                                 const std::vector<std::byte>& bytes)
{
    return analysis::ModuleSetByteInput{std::move(name), bytes, std::nullopt,
                                        "synthetic_m15_fixture"};
}

TEST_CASE("M15 load-order evidence is deterministic and separate from lookup precedence")
{
    const auto rtld = minimal_nso(1U);
    const auto main = minimal_nso(2U);
    const auto subsdk2 = minimal_nso(3U);
    const auto subsdk10 = minimal_nso(4U);
    const auto sdk = minimal_nso(5U);
    const std::array<analysis::ModuleSetByteInput, 5> inputs{
        input("sdk", sdk), input("subsdk10", subsdk10), input("main", main),
        input("rtld", rtld), input("subsdk2", subsdk2)};
    const auto inventory = analysis::validate_module_set(inputs);
    if (!inventory)
        UNSCOPED_INFO(error_code_name(inventory.error().code) << ": " << inventory.error().message);
    REQUIRE(inventory);
    REQUIRE(inventory.value().module_load_order ==
            std::vector<std::string>{"rtld", "main", "subsdk2", "subsdk10", "sdk"});
    REQUIRE(inventory.value().module_load_order_basis == "public_exefs_load_order");

    const auto unknown = minimal_nso(6U);
    const std::array<analysis::ModuleSetByteInput, 1> unknown_input{input("plugin", unknown)};
    const auto unknown_inventory = analysis::validate_module_set(unknown_input);
    REQUIRE(unknown_inventory);
    REQUIRE(unknown_inventory.value().module_load_order_basis ==
            "public_exefs_load_order_with_unclassified_modules");
}

TEST_CASE("M15 incomplete provider search never selects an eligible candidate")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x2000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "sdk.text", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable symbols;
    symbols.symbols.push_back(format::DynamicSymbol{
        7U, 0U, "__nnmusl_init_dso", format::SymbolBinding::Global,
        format::SymbolType::Function, format::SymbolVisibility::Default, 1U, 0x10U, 4U});
    const std::array<analysis::ProcessSymbolSource, 1> sources{
        analysis::ProcessSymbolSource{"sdk", 0x2000U, &symbols}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    const auto incomplete = namespace_result.value().lookup(
        "__nnmusl_init_dso", analysis::ModuleSetCompleteness::Incomplete,
        analysis::ModuleSetCompletenessBasis::DirectoryScanOnly);
    REQUIRE(incomplete.candidates.size() == 1U);
    REQUIRE_FALSE(incomplete.selected_candidate);
    REQUIRE(incomplete.status == analysis::ProviderResolutionStatus::ProviderSearchIncomplete);
    const auto complete = namespace_result.value().lookup(
        "__nnmusl_init_dso", analysis::ModuleSetCompleteness::DeclaredComplete,
        analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion);
    REQUIRE(complete.status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(complete.selected_candidate);
}

TEST_CASE("M15 complete provider closure preserves candidates, exclusions, and ambiguity")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "main.text", memory::GuestRegionKind::Text));
    REQUIRE(memory.map(0x2000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "sdk.text", memory::GuestRegionKind::Text));
    REQUIRE(memory.map(0x3000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "subsdk.text", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable main_symbols;
    main_symbols.symbols.push_back(format::DynamicSymbol{
        1U, 0U, "__nnmusl_init_dso", format::SymbolBinding::Global,
        format::SymbolType::Function, format::SymbolVisibility::Default, 0U, 0U, 0U});
    format::DynamicSymbolTable sdk_symbols;
    sdk_symbols.symbols.push_back(format::DynamicSymbol{
        2U, 0U, "__nnmusl_init_dso", format::SymbolBinding::Global,
        format::SymbolType::Function, format::SymbolVisibility::Default, 1U, 0x10U, 4U});
    sdk_symbols.symbols.push_back(format::DynamicSymbol{
        3U, 0U, "weak_only", format::SymbolBinding::Weak,
        format::SymbolType::Function, format::SymbolVisibility::Default, 1U, 0x14U, 4U});
    format::DynamicSymbolTable subsdk_symbols;
    subsdk_symbols.symbols.push_back(format::DynamicSymbol{
        4U, 0U, "__nnmusl_init_dso", format::SymbolBinding::Global,
        format::SymbolType::Function, format::SymbolVisibility::Default, 1U, 0x10U, 4U});
    subsdk_symbols.symbols.push_back(format::DynamicSymbol{
        5U, 0U, "hidden_fn", format::SymbolBinding::Global,
        format::SymbolType::Function, format::SymbolVisibility::Hidden, 1U, 0x18U, 4U});
    subsdk_symbols.symbols.push_back(format::DynamicSymbol{
        6U, 0U, "local_fn", format::SymbolBinding::Local,
        format::SymbolType::Function, format::SymbolVisibility::Default, 1U, 0x1cU, 4U});
    const std::array<analysis::ProcessSymbolSource, 3> sources{
        analysis::ProcessSymbolSource{"main", 0x1000U, &main_symbols},
        analysis::ProcessSymbolSource{"sdk", 0x2000U, &sdk_symbols},
        analysis::ProcessSymbolSource{"subsdk0", 0x3000U, &subsdk_symbols}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    constexpr auto complete = analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
    const auto provider = namespace_result.value().lookup("__nnmusl_init_dso", complete,
                                                          analysis::ModuleSetCompletenessBasis::TargetManifestMatch);
    REQUIRE(provider.status == analysis::ProviderResolutionStatus::AmbiguousGuestProvider);
    REQUIRE(provider.candidates.size() == 2U);
    REQUIRE(provider.selected_candidate == std::nullopt);
    REQUIRE(provider.occurrences.size() == 3U);
    REQUIRE(namespace_result.value().lookup("weak_only", complete,
                                            analysis::ModuleSetCompletenessBasis::TargetManifestMatch)
                .status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(namespace_result.value().lookup("hidden_fn", complete,
                                            analysis::ModuleSetCompletenessBasis::TargetManifestMatch)
                .status == analysis::ProviderResolutionStatus::NotFoundInSuppliedModules);
}

TEST_CASE("M15 JUMP_SLOT evidence records provider-base arithmetic and readback")
{
    const auto consumer_bytes = dynamic_nso(false, 1U);
    const auto provider_bytes = dynamic_nso(true, 2U);
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"main", consumer_bytes, 0x100000U},
        analysis::ProcessModuleInput{"sdk", provider_bytes, 0x200000U}};
    analysis::ProcessImageOptions options;
    options.primary_module = "main";
    options.module_set_completeness = analysis::ModuleSetCompleteness::DeclaredComplete;
    options.module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion;
    options.module_options.seed_text_entry = false;
    const auto process = analysis::load_process_image(inputs, options);
    REQUIRE(process);
    REQUIRE(process.value().bindings().size() == 1U);
    const auto& binding = process.value().bindings().front();
    REQUIRE(binding.provider_module == std::optional<std::string>("sdk"));
    REQUIRE(binding.provider_base == std::optional<memory::GuestAddress>(0x200000U));
    REQUIRE(binding.provider_symbol_value == std::optional<std::uint64_t>(0x40U));
    REQUIRE(binding.provider_address == std::optional<memory::GuestAddress>(0x200040U));
    REQUIRE(binding.resolved_value == std::optional<std::uint64_t>(0x200040U));
    REQUIRE(binding.slot_value_verified);
    REQUIRE(binding.provider_address !=
            std::optional<memory::GuestAddress>(0x100000U + 0x40U));
}

TEST_CASE("M15 provider arithmetic and relocation commit remain typed and transactional")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x20U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "relocations", memory::GuestRegionKind::Data));
    loader::RelocationPlan plan;
    plan.relocation_count = 2U;
    plan.applied.push_back(loader::AppliedRelocation{
        0U, format::Relocation{0U, 0x1000U, 257U, format::AArch64RelocationType::Abs64, 0U, 0},
        0x1122334455667788ULL, 8U});
    plan.applied.push_back(loader::AppliedRelocation{
        1U, format::Relocation{0U, 0x2000U, 257U, format::AArch64RelocationType::Abs64, 0U, 0},
        0xaabbccddeeff0011ULL, 8U});
    const auto applied = loader::apply_relocation_plan(memory, plan);
    REQUIRE_FALSE(applied);
    std::array<std::byte, 8> unchanged{};
    REQUIRE(memory.read(0x1000U, unchanged));
    REQUIRE(std::all_of(unchanged.begin(), unchanged.end(), [](const auto byte) {
        return byte == std::byte{0};
    }));

    REQUIRE(memory.map(std::numeric_limits<memory::GuestAddress>::max() - 0x20U, 0x20U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "overflow-provider", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable symbols;
    symbols.symbols.push_back(format::DynamicSymbol{
        9U, 0U, "__nnmusl_init_dso", format::SymbolBinding::Global,
        format::SymbolType::Function, format::SymbolVisibility::Default, 1U, 0x40U, 4U});
    const std::array<analysis::ProcessSymbolSource, 1> sources{
        analysis::ProcessSymbolSource{"sdk", std::numeric_limits<memory::GuestAddress>::max() - 0x20U,
                                      &symbols}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    const auto lookup = namespace_result.value().lookup(
        "__nnmusl_init_dso", analysis::ModuleSetCompleteness::DeclaredComplete,
        analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion);
    REQUIRE(lookup.status == analysis::ProviderResolutionStatus::ProviderIneligible);
    REQUIRE(lookup.occurrences.front().eligibility == analysis::ProviderEligibility::InvalidAddress);
}

struct RunCase
{
    execution::ExecutionStopReason stop_reason = execution::ExecutionStopReason::UnresolvedImport;
    bool handler_called = false;
    std::string provider_resolution;
    bool fallback_eligible = false;
};

[[nodiscard]] RunCase run_runtime_gate(analysis::ModuleSetCompleteness completeness,
                                       bool add_provider, bool weak_provider = false)
{
    const auto consumer_bytes = dynamic_nso(false, 3U, true);
    const auto provider_bytes = dynamic_nso(true, 4U, false, weak_provider);
    const auto second_provider_bytes = dynamic_nso(true, 5U);
    std::vector<std::vector<std::byte>> storage;
    storage.push_back(consumer_bytes);
    if (add_provider) storage.push_back(provider_bytes);
    if (add_provider && completeness == analysis::ModuleSetCompleteness::ManifestVerifiedComplete)
        storage.push_back(second_provider_bytes);
    std::vector<analysis::ProcessModuleInput> inputs;
    inputs.push_back(analysis::ProcessModuleInput{"main", storage[0], 0x0U});
    if (add_provider)
    {
        inputs.push_back(analysis::ProcessModuleInput{"sdk", storage[1], 0x100000U});
        if (completeness == analysis::ModuleSetCompleteness::ManifestVerifiedComplete)
        {
            inputs.push_back(analysis::ProcessModuleInput{"subsdk0", storage[2], 0x200000U});
        }
    }
    analysis::ProcessImageOptions options;
    options.primary_module = "main";
    options.module_set_completeness = completeness;
    options.module_set_completeness_basis = completeness == analysis::ModuleSetCompleteness::Incomplete
                                                ? analysis::ModuleSetCompletenessBasis::DirectoryScanOnly
                                                : analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion;
    options.module_options.seed_text_entry = false;
    auto process = analysis::load_process_image(inputs, options);
    REQUIRE(process);

    std::vector<analysis::FinalizedFunctionMap> maps;
    for (const auto& module : process.value().modules())
    {
        const auto map = analysis::FunctionMapBuilder::build(
            analysis::ModuleAnalysisInput{module.identity, &process.value().memory(), module.seeds});
        REQUIRE(map);
        maps.push_back(std::move(map).value());
    }
    const auto process_map = analysis::ProcessFunctionMap::build(std::move(maps));
    REQUIRE(process_map);
    const analysis::FinalizedFunctionMap* main_map = nullptr;
    for (const auto& map : process_map.value().maps())
        if (map.identity().module == "main") main_map = &map;
    REQUIRE(main_map != nullptr);
    const auto entry = execution::select_entry(main_map->identity(),
                                               execution::EntrySelectionKind::DynamicInit);
    REQUIRE(entry);

    runtime::RuntimeImportRegistry registry;
    runtime::RuntimeImportDescriptor descriptor;
    descriptor.symbol_name = "__nnmusl_init_dso";
    descriptor.support = runtime::RuntimeSupportStatus::Implemented;
    descriptor.subsystem = runtime::RuntimeSubsystem::DynamicLoader;
    descriptor.signature.argument_count = 0U;
    descriptor.evidence.confidence = "synthetic_m15";
    RunCase result;
    const auto registered = registry.register_import(
        descriptor, [&](runtime::RuntimeImportContext&) {
            result.handler_called = true;
            return Result<runtime::RuntimeImportOutcome>::success(
                runtime::RuntimeImportOutcome::handled());
        });
    REQUIRE(registered);
    execution::ExecutionSession session(process.value().memory(), process_map.value(), process.value(),
                                        {}, {}, &registry);
    const auto run = session.run(entry.value());
    REQUIRE(run);
    result.stop_reason = run.value().stop_reason;
    if (run.value().runtime.imports.empty())
    {
        result.provider_resolution = analysis::provider_resolution_status_name(
            process.value().lookup_provider("__nnmusl_init_dso").status);
    }
    else
    {
        result.provider_resolution = run.value().runtime.imports.front().guest_provider_resolution;
        result.fallback_eligible = run.value().runtime.imports.front().runtime_fallback_eligible;
    }
    return result;
}

TEST_CASE("M15 runtime registry cannot mask incomplete or ambiguous guest resolution")
{
    const auto incomplete = run_runtime_gate(analysis::ModuleSetCompleteness::Incomplete, false);
    REQUIRE(incomplete.stop_reason == execution::ExecutionStopReason::UnresolvedImport);
    REQUIRE_FALSE(incomplete.handler_called);
    REQUIRE(incomplete.provider_resolution == "provider_search_incomplete");
    REQUIRE_FALSE(incomplete.fallback_eligible);

    const auto ambiguous = run_runtime_gate(
        analysis::ModuleSetCompleteness::ManifestVerifiedComplete, true);
    REQUIRE(ambiguous.stop_reason == execution::ExecutionStopReason::UnresolvedImport);
    REQUIRE_FALSE(ambiguous.handler_called);
    REQUIRE(ambiguous.provider_resolution == "ambiguous_guest_provider");
    REQUIRE_FALSE(ambiguous.fallback_eligible);
}

TEST_CASE("M15 runtime registry is eligible only after complete no-provider search")
{
    const auto complete_no_provider = run_runtime_gate(
        analysis::ModuleSetCompleteness::ManifestVerifiedComplete, false);
    REQUIRE(complete_no_provider.stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(complete_no_provider.handler_called);
    REQUIRE(complete_no_provider.provider_resolution == "provider_not_found_complete");
    REQUIRE(complete_no_provider.fallback_eligible);

    const auto weak_provider = run_runtime_gate(
        analysis::ModuleSetCompleteness::ManifestVerifiedComplete, true, true);
    REQUIRE(weak_provider.stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE_FALSE(weak_provider.handler_called);
    REQUIRE(weak_provider.provider_resolution == "resolved_guest_module");
    REQUIRE_FALSE(weak_provider.fallback_eligible);
}

} // namespace
