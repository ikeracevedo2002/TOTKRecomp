#include "switchrecomp/analysis/module_set.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/execution/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

void write_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
{
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
    write_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] std::vector<std::byte> dynamic_nso(bool defines_provider, std::uint8_t marker,
                                                 bool caller = false)
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
    file[symbol_file + 4U] = static_cast<std::byte>(0x12U);
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

[[nodiscard]] analysis::ModuleSetByteInput byte_input(
    std::string name, const std::vector<std::byte>& bytes)
{
    return analysis::ModuleSetByteInput{std::move(name), bytes, std::nullopt,
                                        "explicit_configuration"};
}

} // namespace

TEST_CASE("M14 executable module inventory is deterministic and keeps identity provenance")
{
    const auto main_bytes = minimal_nso(1U);
    const auto sdk_bytes = minimal_nso(2U);
    const std::array<analysis::ModuleSetByteInput, 2> reverse{
        byte_input("sdk", sdk_bytes), byte_input("main", main_bytes)};
    const auto first = analysis::validate_module_set(reverse);
    REQUIRE(first);
    const auto ordered = first.value().process_inputs();
    REQUIRE(first.value().modules[0].logical_name == "main");
    REQUIRE(first.value().modules[1].logical_name == "sdk");
    REQUIRE(first.value().modules[0].name_provenance == "explicit_configuration");
    REQUIRE(first.value().modules[0].sha256 != first.value().modules[1].sha256);
    REQUIRE(ordered.size() == 2U);

    const auto second = analysis::validate_module_set(reverse);
    REQUIRE(second);
    REQUIRE(first.value().modules[0].sha256 == second.value().modules[0].sha256);
    REQUIRE(first.value().modules[1].build_id == second.value().modules[1].build_id);
}

TEST_CASE("M14 module inventory rejects malformed, duplicate-name, and duplicate-binary inputs")
{
    const auto valid = minimal_nso(1U);
    const std::vector<std::byte> malformed_bytes{
        std::byte{'N'}, std::byte{'S'}, std::byte{'O'}, std::byte{'0'}};
    std::array<analysis::ModuleSetByteInput, 1> malformed{
        byte_input("main", malformed_bytes)};
    auto result = analysis::validate_module_set(malformed);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::ModuleParseFailed);

    const auto other = minimal_nso(2U);
    const std::array<analysis::ModuleSetByteInput, 2> duplicate_name{
        byte_input("main", valid), byte_input("main", other)};
    result = analysis::validate_module_set(duplicate_name);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::DuplicateModuleIdentity);

    const std::array<analysis::ModuleSetByteInput, 2> duplicate_binary{
        byte_input("main", valid), byte_input("sdk", valid)};
    result = analysis::validate_module_set(duplicate_binary);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::ModuleSetIdentityConflict);
}

TEST_CASE("M14 directory inventory ignores non-NSO files and is not a completeness attestation")
{
    const auto directory = std::filesystem::temp_directory_path() / "totkrecomp-m14-module-set";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    REQUIRE(std::filesystem::create_directories(directory, error));
    {
        std::ofstream main_file(directory / "main", std::ios::binary);
        const auto bytes = minimal_nso(3U);
        main_file.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        std::ofstream sdk_file(directory / "sdk.nso", std::ios::binary);
        const auto sdk = minimal_nso(4U);
        sdk_file.write(reinterpret_cast<const char*>(sdk.data()),
                       static_cast<std::streamsize>(sdk.size()));
        std::ofstream npdm(directory / "main.npdm", std::ios::binary);
        npdm << "not an NSO";
        std::ofstream random(directory / "random.txt", std::ios::binary);
        random << "not an NSO";
    }
    analysis::DirectoryInventoryOptions options;
    options.explicit_bases["sdk"] = 0x500000U;
    const auto inventory = analysis::scan_prepared_module_directory(directory, options);
    REQUIRE(inventory);
    REQUIRE(inventory.value().modules.size() == 2U);
    REQUIRE(inventory.value().modules[0].logical_name == "main");
    REQUIRE(inventory.value().modules[1].logical_name == "sdk");
    REQUIRE(inventory.value().modules[1].explicit_base ==
            std::optional<memory::GuestAddress>(0x500000U));
    REQUIRE(inventory.value().completeness == analysis::ModuleSetCompleteness::Incomplete);
    REQUIRE(inventory.value().completeness_basis == analysis::ModuleSetCompletenessBasis::DirectoryScanOnly);
    REQUIRE(inventory.value().ignored_entries.size() == 2U);
    REQUIRE(std::is_sorted(inventory.value().ignored_entries.begin(),
                           inventory.value().ignored_entries.end()));
    std::filesystem::remove_all(directory, error);
}

TEST_CASE("M14 explicit inventory requires expected identities for manifest verification")
{
    const auto main_bytes = minimal_nso(5U);
    const auto sdk_bytes = minimal_nso(6U);
    const std::array<analysis::ModuleSetByteInput, 2> inputs{
        byte_input("main", main_bytes), byte_input("sdk", sdk_bytes)};
    analysis::ModuleSetIngestionOptions options;
    options.expected_logical_names = {"main", "sdk"};
    const auto observed = analysis::validate_module_set(inputs, options);
    REQUIRE(observed);
    options.completeness = analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
    options.completeness_basis = analysis::ModuleSetCompletenessBasis::LocalManifestMatch;
    for (const auto& module : observed.value().modules)
        options.expected_modules.push_back(analysis::ModuleSetIngestionOptions::ExpectedModule{
            module.logical_name, module.sha256, module.build_id, module.input_size});
    const auto verified = analysis::validate_module_set(inputs, options);
    REQUIRE(verified);
    REQUIRE(verified.value().coherence == analysis::ModuleSetCoherence::Unverified);

    options.expected_logical_names = {"main", "missing"};
    const auto conflicting = analysis::validate_module_set(inputs, options);
    REQUIRE_FALSE(conflicting);
    REQUIRE(conflicting.error().code == ErrorCode::ModuleManifestMismatch);
}

TEST_CASE("M17 identity preflight reports the exact deterministic mismatch category")
{
    const auto bytes = minimal_nso(12U);
    const std::array<analysis::ModuleSetByteInput, 1> inputs{byte_input("main", bytes)};
    analysis::ModuleSetIngestionOptions options;
    options.expected_modules.push_back(analysis::ModuleSetIngestionOptions::ExpectedModule{
        "main", "0000000000000000000000000000000000000000000000000000000000000000",
        "expected-build-id", bytes.size()});
    const auto result = analysis::validate_module_set(inputs, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::ModuleManifestMismatch);
    REQUIRE(result.error().message.find("logical_module=main") != std::string::npos);
    REQUIRE(result.error().message.find("category=build_id_mismatch") != std::string::npos);
    REQUIRE(result.error().message.find("expected=expected-build-id") != std::string::npos);
    REQUIRE(result.error().message.find("actual=") != std::string::npos);
}

TEST_CASE("M14 completeness keeps legacy incomplete and declared-complete provider outcomes distinct")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x2000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "provider.text", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable symbols;
    symbols.symbols.push_back(format::DynamicSymbol{4U, 0U, "missing", format::SymbolBinding::Global,
                                                    format::SymbolType::Function,
                                                    format::SymbolVisibility::Default, 1U, 0x10U, 4U});
    const std::array<analysis::ProcessSymbolSource, 1> sources{
        analysis::ProcessSymbolSource{"sdk", 0x2000U, &symbols}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    REQUIRE(namespace_result.value().lookup(
                 "__nnmusl_init_dso", analysis::ModuleSetCompleteness::Incomplete,
                 analysis::ModuleSetCompletenessBasis::LegacyConfigFalse).status ==
             analysis::ProviderResolutionStatus::ProviderSearchIncomplete);
    const auto complete = namespace_result.value().lookup(
        "__nnmusl_init_dso", analysis::ModuleSetCompleteness::DeclaredComplete,
        analysis::ModuleSetCompletenessBasis::ExplicitInventory);
    REQUIRE(complete.status == analysis::ProviderResolutionStatus::NotFoundInSuppliedModules);
    REQUIRE(complete.candidates.empty());
}

TEST_CASE("M14 audited provider occurrences explain non-executable and undefined symbols")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x3000U, 0x40U, memory::GuestMemoryPermissions::Read,
                       "provider.rodata", memory::GuestRegionKind::Rodata));
    format::DynamicSymbolTable symbols;
    symbols.symbols.push_back(format::DynamicSymbol{5U, 0U, "__nnmusl_init_dso",
                                                    format::SymbolBinding::Global,
                                                    format::SymbolType::Function,
                                                    format::SymbolVisibility::Default, 1U, 0x10U, 4U});
    symbols.symbols.push_back(format::DynamicSymbol{6U, 0U, "__nnmusl_init_dso",
                                                    format::SymbolBinding::Global,
                                                    format::SymbolType::Function,
                                                    format::SymbolVisibility::Default, 0U, 0U, 0U});
    const std::array<analysis::ProcessSymbolSource, 1> sources{
        analysis::ProcessSymbolSource{"sdk", 0x3000U, &symbols}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    const auto lookup = namespace_result.value().lookup(
        "__nnmusl_init_dso", analysis::ModuleSetCompleteness::DeclaredComplete,
        analysis::ModuleSetCompletenessBasis::ExplicitInventory);
    REQUIRE(lookup.candidates.empty());
    REQUIRE(lookup.status == analysis::ProviderResolutionStatus::ProviderIneligible);
    REQUIRE(lookup.occurrences.size() == 2U);
    REQUIRE(lookup.occurrences[0].eligibility == analysis::ProviderEligibility::NonExecutable);
    REQUIRE(lookup.occurrences[1].eligibility == analysis::ProviderEligibility::Undefined);
}

TEST_CASE("M14 provider selection is process-wide and evidence-based for weak and strong symbols")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "main.text", memory::GuestRegionKind::Text));
    REQUIRE(memory.map(0x2000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "sdk.text", memory::GuestRegionKind::Text));
    REQUIRE(memory.map(0x3000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "subsdk.text", memory::GuestRegionKind::Text));

    format::DynamicSymbolTable sdk;
    sdk.symbols = {
        format::DynamicSymbol{1U, 0U, "only_weak", format::SymbolBinding::Weak,
                              format::SymbolType::Function, format::SymbolVisibility::Default,
                              1U, 0x10U, 4U},
        format::DynamicSymbol{2U, 0U, "strong_plus_weak", format::SymbolBinding::Weak,
                              format::SymbolType::Function, format::SymbolVisibility::Default,
                              1U, 0x14U, 4U},
        format::DynamicSymbol{3U, 0U, "ambiguous", format::SymbolBinding::Global,
                              format::SymbolType::Function, format::SymbolVisibility::Default,
                              1U, 0x18U, 4U},
        format::DynamicSymbol{4U, 0U, "local_fn", format::SymbolBinding::Local,
                              format::SymbolType::Function, format::SymbolVisibility::Default,
                              1U, 0x1cU, 4U},
    };
    format::DynamicSymbolTable subsdk;
    subsdk.symbols = {
        format::DynamicSymbol{5U, 0U, "strong_plus_weak", format::SymbolBinding::Global,
                              format::SymbolType::Function, format::SymbolVisibility::Default,
                              1U, 0x10U, 4U},
        format::DynamicSymbol{6U, 0U, "ambiguous", format::SymbolBinding::Global,
                              format::SymbolType::Function, format::SymbolVisibility::Default,
                              1U, 0x14U, 4U},
        format::DynamicSymbol{7U, 0U, "third_module_fn", format::SymbolBinding::Global,
                              format::SymbolType::Function, format::SymbolVisibility::Default,
                              1U, 0x18U, 4U},
    };
    const std::array<analysis::ProcessSymbolSource, 2> sources{
        analysis::ProcessSymbolSource{"sdk", 0x2000U, &sdk},
        analysis::ProcessSymbolSource{"subsdk", 0x3000U, &subsdk}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    constexpr auto completeness = analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
    const auto basis = analysis::ModuleSetCompletenessBasis::TargetManifestMatch;

    const auto weak = namespace_result.value().lookup("only_weak", completeness, basis);
    REQUIRE(weak.status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(weak.candidates.size() == 1U);
    REQUIRE(weak.candidates.front().binding == format::SymbolBinding::Weak);

    const auto strong = namespace_result.value().lookup(
        "strong_plus_weak", completeness, basis);
    REQUIRE(strong.status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(strong.candidates.size() == 2U);
    REQUIRE(strong.selected_candidate);
    REQUIRE(strong.candidates[strong.selected_candidate.value()].binding ==
            format::SymbolBinding::Global);

    REQUIRE(namespace_result.value().lookup("ambiguous", completeness, basis).status ==
            analysis::ProviderResolutionStatus::AmbiguousGuestProvider);
    REQUIRE(namespace_result.value().lookup("local_fn", completeness, basis).status ==
            analysis::ProviderResolutionStatus::NotFoundInSuppliedModules);
    const auto third = namespace_result.value().lookup(
        "third_module_fn", completeness, basis);
    REQUIRE(third.status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(third.candidates.front().module == "subsdk");
}

TEST_CASE("M14 process summary carries completeness, coherence, and ignored-entry provenance")
{
    const auto main_bytes = minimal_nso(7U);
    const std::array<analysis::ProcessModuleInput, 1> inputs{
        analysis::ProcessModuleInput{"main", main_bytes, 0x100000U, "explicit_configuration"}};
    analysis::ProcessImageOptions options;
    options.module_set_completeness = analysis::ModuleSetCompleteness::DeclaredComplete;
    options.module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitInventory;
    options.module_set_coherence = analysis::ModuleSetCoherence::PartiallyVerified;
    options.module_set_coherence_basis = "synthetic_manifest";
    options.ignored_module_entries = {"random.txt:not_nso"};
    const auto image = analysis::load_process_image(inputs, options);
    REQUIRE(image);
    const auto summary = image.value().summary();
    REQUIRE(summary.provider_search_complete);
    REQUIRE(summary.completeness == analysis::ModuleSetCompleteness::DeclaredComplete);
    REQUIRE(summary.completeness_basis == analysis::ModuleSetCompletenessBasis::ExplicitInventory);
    REQUIRE(summary.coherence == analysis::ModuleSetCoherence::PartiallyVerified);
    REQUIRE(summary.ignored_module_entries == std::vector<std::string>{"random.txt:not_nso"});
    REQUIRE(summary.modules.size() == 1U);
    REQUIRE(summary.modules.front().input_size == main_bytes.size());
    REQUIRE(summary.modules.front().name_provenance == "explicit_configuration");
}

TEST_CASE("M14 cross-module JUMP_SLOT uses the provider module guest base transactionally")
{
    const auto consumer_bytes = dynamic_nso(false, 8U);
    const auto provider_bytes = dynamic_nso(true, 9U);
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"main", consumer_bytes, 0x100000U},
        analysis::ProcessModuleInput{"sdk", provider_bytes, 0x200000U}};
    analysis::ProcessImageOptions options;
    options.primary_module = "main";
    options.provider_search_complete = true;
    options.module_options.seed_text_entry = false;
    const auto process = analysis::load_process_image(inputs, options);
    if (!process) UNSCOPED_INFO(error_code_name(process.error().code) << ": " << process.error().message);
    REQUIRE(process);
    REQUIRE(process.value().bindings().size() == 1U);
    const auto& binding = process.value().bindings().front();
    REQUIRE(binding.symbol == "__nnmusl_init_dso");
    REQUIRE(binding.provider.status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(binding.provider_module == std::optional<std::string>("sdk"));
    REQUIRE(binding.provider_address == std::optional<memory::GuestAddress>(0x200040U));
    REQUIRE(binding.applied);
    REQUIRE(process.value().modules()[0].applied_relocations == 1U);
    std::array<std::byte, 8> slot{};
    REQUIRE(process.value().memory().read(0x108020U, slot));
    std::uint64_t slot_value = 0U;
    for (std::size_t index = 0U; index < slot.size(); ++index)
        slot_value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(slot[index])) << (index * 8U);
    REQUIRE(slot_value == 0x200040U);
    REQUIRE(process.value().summary().focus_provider);
    REQUIRE(process.value().summary().focus_provider->completeness ==
            analysis::ModuleSetCompleteness::DeclaredComplete);
}

TEST_CASE("M14 a resolved guest provider wins over a same-name runtime handler")
{
    const auto consumer_bytes = dynamic_nso(false, 10U, true);
    const auto provider_bytes = dynamic_nso(true, 11U);
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"main", consumer_bytes, 0x0U},
        analysis::ProcessModuleInput{"sdk", provider_bytes, 0x100000U}};
    analysis::ProcessImageOptions image_options;
    image_options.primary_module = "main";
    image_options.provider_search_complete = true;
    image_options.module_options.seed_text_entry = false;
    bool runtime_handler_called = false;
    const auto run_once = [&]() -> Result<execution::ExecutionSessionResult> {
        auto process = analysis::load_process_image(inputs, image_options);
        if (!process)
            return Result<execution::ExecutionSessionResult>::failure(process.error());

        std::vector<analysis::FinalizedFunctionMap> maps;
        for (const auto& module : process.value().modules())
        {
            const auto map = analysis::FunctionMapBuilder::build(
                analysis::ModuleAnalysisInput{module.identity, &process.value().memory(), module.seeds});
            if (!map)
                return Result<execution::ExecutionSessionResult>::failure(map.error());
            maps.push_back(std::move(map).value());
        }
        const auto process_map = analysis::ProcessFunctionMap::build(std::move(maps));
        if (!process_map)
            return Result<execution::ExecutionSessionResult>::failure(process_map.error());
        const analysis::FinalizedFunctionMap* main_map = nullptr;
        for (const auto& map : process_map.value().maps())
            if (map.identity().module == "main") main_map = &map;
        if (main_map == nullptr)
            return Result<execution::ExecutionSessionResult>::failure(
                make_error(ErrorCode::InvalidArgument, "synthetic process has no main function map"));
        const auto entry = execution::select_entry(
            main_map->identity(), execution::EntrySelectionKind::DynamicInit);
        if (!entry)
            return Result<execution::ExecutionSessionResult>::failure(entry.error());

        runtime::RuntimeImportRegistry registry;
        runtime::RuntimeImportDescriptor descriptor;
        descriptor.symbol_name = "__nnmusl_init_dso";
        descriptor.subsystem = runtime::RuntimeSubsystem::DynamicLoader;
        descriptor.support = runtime::RuntimeSupportStatus::Implemented;
        descriptor.signature.argument_count = 0U;
        descriptor.evidence.confidence = "synthetic";
        const auto registered = registry.register_import(
            descriptor, [&runtime_handler_called](runtime::RuntimeImportContext&) {
                runtime_handler_called = true;
                return Result<runtime::RuntimeImportOutcome>::success(
                    runtime::RuntimeImportOutcome::handled());
            });
        if (!registered)
            return Result<execution::ExecutionSessionResult>::failure(registered.error());
        execution::ExecutionSession session(process.value().memory(), process_map.value(), process.value(),
                                             {}, {}, &registry);
        return session.run(entry.value());
    };

    const auto run = run_once();
    if (!run) UNSCOPED_INFO(error_code_name(run.error().code) << ": " << run.error().message);
    REQUIRE(run);
    if (run && run.value().stop_reason != execution::ExecutionStopReason::EntryReturned)
        UNSCOPED_INFO(run.value().diagnostic);
    REQUIRE(run.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(run.value().provider_guest_code_entered);
    REQUIRE(run.value().runtime.imports_encountered == 0U);
    REQUIRE_FALSE(runtime_handler_called);
    REQUIRE(run.value().function_transfers == 1U);
    REQUIRE(run.value().maximum_call_depth == 1U);
    REQUIRE(std::find(run.value().executed_function_modules.begin(),
                      run.value().executed_function_modules.end(), "sdk") !=
            run.value().executed_function_modules.end());
    const auto first_report = execution::render_execution_report_json(run.value());
    REQUIRE(first_report.find("/Users/") == std::string::npos);
    REQUIRE(first_report.find("__nnmusl_init_dso") != std::string::npos);
    const auto repeated = run_once();
    REQUIRE(repeated);
    REQUIRE(first_report == execution::render_execution_report_json(repeated.value()));
}
