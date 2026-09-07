#pragma once

#include "switchrecomp/analysis/whole_module.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace switchrecomp::analysis
{

enum class ModuleSetCompleteness : std::uint8_t
{
    Incomplete,
    DeclaredComplete,
    ManifestVerifiedComplete,
};

[[nodiscard]] std::string_view module_set_completeness_name(
    ModuleSetCompleteness completeness) noexcept;

enum class ModuleSetCompletenessBasis : std::uint8_t
{
    LegacyConfigFalse,
    ExplicitLocalAssertion,
    ExplicitInventory,
    LocalManifestMatch,
    TargetManifestMatch,
    DirectoryScanOnly,
    Unknown,
};

[[nodiscard]] std::string_view module_set_completeness_basis_name(
    ModuleSetCompletenessBasis basis) noexcept;

enum class ModuleSetCoherence : std::uint8_t
{
    Verified,
    PartiallyVerified,
    Unverified,
    Conflicting,
};

[[nodiscard]] std::string_view module_set_coherence_name(ModuleSetCoherence coherence) noexcept;

enum class ProviderEligibility : std::uint8_t
{
    Eligible,
    NoName,
    Undefined,
    LocalBinding,
    IneligibleBinding,
    HiddenVisibility,
    InvalidAddress,
    OutsideProcessMemory,
    NonExecutable,
    UnsupportedType,
};

[[nodiscard]] std::string_view provider_eligibility_name(
    ProviderEligibility eligibility) noexcept;

enum class ProviderResolutionStatus : std::uint8_t
{
    ResolvedGuestModule,
    AmbiguousGuestProvider,
    NotFoundInSuppliedModules,
    ProviderSearchIncomplete,
    ProviderIneligible,
    InvalidProviderDefinition,
};

[[nodiscard]] std::string_view provider_resolution_status_name(
    ProviderResolutionStatus status) noexcept;

struct ProcessModuleInput
{
    std::string logical_name;
    std::span<const std::byte> file_bytes;
    std::optional<memory::GuestAddress> explicit_base;
    std::string name_provenance = "explicit_configuration";
};

struct ProcessImageOptions
{
    std::string primary_module = "main";
    memory::GuestAddress automatic_base = 0x7200000000ULL;
    memory::GuestAddress module_alignment = 0x1000U;
    memory::GuestAddress module_gap = 0x10000U;
    std::optional<memory::GuestAddress> reserved_stack_base;
    memory::GuestSize reserved_stack_size = 0U;
    bool provider_search_complete = false;
    ModuleSetCompleteness module_set_completeness = ModuleSetCompleteness::Incomplete;
    ModuleSetCompletenessBasis module_set_completeness_basis =
        ModuleSetCompletenessBasis::LegacyConfigFalse;
    ModuleSetCoherence module_set_coherence = ModuleSetCoherence::Unverified;
    std::string module_set_coherence_basis = "unknown";
    std::string module_set_source = "explicit";
    std::vector<std::string> ignored_module_entries;
    // Inventory tools can deliberately stop after parsing/analysis metadata.
    // Such a ProcessImage is useful for evidence, but is not an executable
    // relocation-applied process state.
    bool plan_relocations = true;
    bool apply_relocations = true;
    PreparedModuleOptions module_options;
};

struct ProcessModule
{
    ModuleIdentity identity;
    std::string name_provenance = "explicit_configuration";
    std::uint64_t input_size = 0U;
    ModuleBaseProvenance base_provenance = ModuleBaseProvenance::ExplicitAnalysisBase;
    format::NsoImage image;
    format::ModuleMetadata metadata;
    std::optional<format::DynamicSymbolTable> symbols;
    std::vector<format::Relocation> relocations;
    std::size_t applied_relocations = 0U;
    std::vector<loader::UnresolvedRelocation> unresolved_relocations;
    std::vector<format::ImportSymbol> unresolved_imports;
    std::vector<FunctionSeed> seeds;
    std::vector<memory::GuestMemoryRegionInfo> mappings;
};

struct ProviderCandidate
{
    std::string module;
    std::uint32_t symbol_index = 0U;
    std::string symbol;
    format::SymbolBinding binding = format::SymbolBinding::Unknown;
    format::SymbolType type = format::SymbolType::Unknown;
    format::SymbolVisibility visibility = format::SymbolVisibility::Unknown;
    std::uint16_t section_index = 0U;
    std::uint64_t value = 0U;
    memory::GuestAddress address = 0U;
    bool executable = false;
};

struct ProviderOccurrence
{
    std::string module;
    std::uint32_t symbol_index = 0U;
    std::string symbol;
    bool defined = false;
    format::SymbolBinding binding = format::SymbolBinding::Unknown;
    format::SymbolType type = format::SymbolType::Unknown;
    format::SymbolVisibility visibility = format::SymbolVisibility::Unknown;
    std::uint16_t section_index = 0U;
    std::uint64_t value = 0U;
    std::optional<memory::GuestAddress> address;
    bool executable = false;
    bool eligible = false;
    ProviderEligibility eligibility = ProviderEligibility::UnsupportedType;
};

struct ProviderLookup
{
    ProviderResolutionStatus status = ProviderResolutionStatus::NotFoundInSuppliedModules;
    std::vector<ProviderCandidate> candidates;
    std::vector<ProviderOccurrence> occurrences;
    std::optional<std::size_t> selected_candidate;
    ModuleSetCompleteness completeness = ModuleSetCompleteness::Incomplete;
    ModuleSetCompletenessBasis completeness_basis = ModuleSetCompletenessBasis::LegacyConfigFalse;
};

struct ProcessSymbolSource
{
    std::string module;
    memory::GuestAddress base = 0U;
    const format::DynamicSymbolTable* symbols = nullptr;
};

class ProcessSymbolNamespace
{
  public:
    [[nodiscard]] static Result<ProcessSymbolNamespace> build(
        const memory::GuestMemory& memory, std::span<const ProcessSymbolSource> sources);
    // M14 audited construction retains non-provider occurrences and their
    // exclusion reasons. The legacy build() remains strict for M13 callers.
    [[nodiscard]] static Result<ProcessSymbolNamespace> build_audited(
        const memory::GuestMemory& memory, std::span<const ProcessSymbolSource> sources);
    [[nodiscard]] ProviderLookup lookup(std::string_view name,
                                        bool search_complete) const;
    [[nodiscard]] ProviderLookup lookup(std::string_view name,
                                        ModuleSetCompleteness completeness,
                                        ModuleSetCompletenessBasis basis) const;
    [[nodiscard]] const std::vector<ProviderCandidate>& candidates() const noexcept
    {
        return candidates_;
    }

  private:
    [[nodiscard]] static Result<ProcessSymbolNamespace> build_impl(
        const memory::GuestMemory& memory, std::span<const ProcessSymbolSource> sources,
        bool strict_invalid_providers);
    std::vector<ProviderCandidate> candidates_;
    std::vector<ProviderOccurrence> occurrences_;
};

struct ProcessBinding
{
    std::string consumer_module;
    std::size_t consumer_symbol_index = 0U;
    std::string symbol;
    std::size_t relocation_index = 0U;
    format::Relocation relocation{};
    ProviderLookup provider;
    std::optional<std::string> provider_module;
    std::optional<std::uint32_t> provider_symbol_index;
    std::optional<memory::GuestAddress> provider_address;
    std::string resolution_basis;
    std::string confidence;
    bool applied = false;
};

struct ProcessModuleSummary
{
    struct Segment
    {
        std::string kind;
        std::uint32_t file_offset = 0U;
        std::uint32_t memory_offset = 0U;
        std::uint32_t memory_size = 0U;
        std::uint32_t stored_size = 0U;
        bool compressed = false;
        bool hash_required = false;
    };

    struct Mapping
    {
        memory::GuestAddress base = 0U;
        memory::GuestSize size = 0U;
        memory::GuestMemoryPermissions permissions = memory::GuestMemoryPermissions::None;
        memory::GuestRegionKind kind = memory::GuestRegionKind::Other;
    };

    std::string logical_name;
    std::string name_provenance;
    std::uint64_t input_size = 0U;
    std::string sha256;
    std::string build_id;
    std::uint32_t nso_version = 0U;
    std::uint32_t nso_flags = 0U;
    memory::GuestAddress base = 0U;
    ModuleBaseProvenance base_provenance = ModuleBaseProvenance::ExplicitAnalysisBase;
    bool runtime_base_verified = false;
    std::vector<Segment> segments;
    std::uint64_t bss_size = 0U;
    std::vector<GuestAddressRange> mapped_ranges;
    std::vector<Mapping> mappings;
    std::size_t dynamic_symbol_count = 0U;
    std::size_t defined_symbol_count = 0U;
    std::size_t undefined_symbol_count = 0U;
    std::size_t relocation_count = 0U;
    std::size_t applied_relocations = 0U;
    std::size_t unresolved_relocations = 0U;
    bool mod0_available = false;
    bool dynamic_available = false;
    std::string mod0_status;
    std::string dynamic_status;
    bool provider_index_eligible = false;
    std::vector<std::string> dynamic_tags;
};

struct ProcessImageSummary
{
    std::string primary_module;
    std::string layout_mode = "deterministic_analysis_layout";
    bool provider_search_complete = false;
    ModuleSetCompleteness completeness = ModuleSetCompleteness::Incomplete;
    ModuleSetCompletenessBasis completeness_basis = ModuleSetCompletenessBasis::LegacyConfigFalse;
    ModuleSetCoherence coherence = ModuleSetCoherence::Unverified;
    std::string coherence_basis = "unknown";
    std::string source = "explicit";
    std::size_t module_count = 0U;
    std::size_t executable_module_count = 0U;
    bool relocations_planned = true;
    bool transactional_relocation_success = true;
    std::vector<std::string> ignored_module_entries;
    std::vector<ProcessModuleSummary> modules;
    std::vector<ProcessBinding> bindings;
    std::optional<ProviderLookup> focus_provider;
};

class ProcessImage
{
  public:
    ProcessImage() = default;

    [[nodiscard]] const memory::GuestMemory& memory() const noexcept { return memory_; }
    [[nodiscard]] memory::GuestMemory& memory() noexcept { return memory_; }
    [[nodiscard]] const std::vector<ProcessModule>& modules() const noexcept { return modules_; }
    [[nodiscard]] const ProcessModule* module(std::string_view name) const noexcept;
    [[nodiscard]] const ProcessModule* module_for_address(
        memory::GuestAddress address, memory::GuestSize size = 1U) const noexcept;
    [[nodiscard]] const std::vector<ProcessBinding>& bindings() const noexcept
    {
        return bindings_;
    }
    [[nodiscard]] ProviderLookup lookup_provider(std::string_view name) const;
    [[nodiscard]] std::vector<loader::UnresolvedRelocation> unresolved_relocations() const;
    [[nodiscard]] ProcessImageSummary summary() const;
    [[nodiscard]] bool executable_state_valid() const noexcept
    {
        return executable_state_valid_;
    }

  private:
    friend Result<ProcessImage> load_process_image(
        std::span<const ProcessModuleInput> inputs, const ProcessImageOptions& options);

    memory::GuestMemory memory_;
    std::string primary_module_;
    bool provider_search_complete_ = false;
    ModuleSetCompleteness completeness_ = ModuleSetCompleteness::Incomplete;
    ModuleSetCompletenessBasis completeness_basis_ = ModuleSetCompletenessBasis::LegacyConfigFalse;
    ModuleSetCoherence coherence_ = ModuleSetCoherence::Unverified;
    std::string coherence_basis_ = "unknown";
    std::string source_ = "explicit";
    bool relocations_planned_ = true;
    bool executable_state_valid_ = true;
    std::vector<std::string> ignored_module_entries_;
    ProcessSymbolNamespace symbol_namespace_;
    std::vector<ProcessModule> modules_;
    std::vector<ProcessBinding> bindings_;
};

[[nodiscard]] Result<ProcessImage> load_process_image(
    std::span<const ProcessModuleInput> inputs, const ProcessImageOptions& options = {});

class ProcessFunctionMap
{
  public:
    ProcessFunctionMap() = default;

    [[nodiscard]] static Result<ProcessFunctionMap> build(
        std::vector<FinalizedFunctionMap> maps);
    [[nodiscard]] const FunctionRecord* find(memory::GuestAddress entry) const noexcept;
    [[nodiscard]] const FinalizedFunctionMap* map_for(
        memory::GuestAddress entry) const noexcept;
    [[nodiscard]] const std::vector<FinalizedFunctionMap>& maps() const noexcept { return maps_; }

  private:
    std::vector<FinalizedFunctionMap> maps_;
    std::map<memory::GuestAddress, std::size_t> entries_;
};

} // namespace switchrecomp::analysis
