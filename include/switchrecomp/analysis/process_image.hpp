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

enum class ProviderResolutionStatus : std::uint8_t
{
    ResolvedGuestModule,
    AmbiguousGuestProvider,
    NotFoundInSuppliedModules,
    ProviderSearchIncomplete,
    InvalidProviderDefinition,
};

[[nodiscard]] std::string_view provider_resolution_status_name(
    ProviderResolutionStatus status) noexcept;

struct ProcessModuleInput
{
    std::string logical_name;
    std::span<const std::byte> file_bytes;
    std::optional<memory::GuestAddress> explicit_base;
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
    bool apply_relocations = true;
    PreparedModuleOptions module_options;
};

struct ProcessModule
{
    ModuleIdentity identity;
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

struct ProviderLookup
{
    ProviderResolutionStatus status = ProviderResolutionStatus::NotFoundInSuppliedModules;
    std::vector<ProviderCandidate> candidates;
    std::optional<std::size_t> selected_candidate;
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
    [[nodiscard]] ProviderLookup lookup(std::string_view name,
                                        bool search_complete) const;
    [[nodiscard]] const std::vector<ProviderCandidate>& candidates() const noexcept
    {
        return candidates_;
    }

  private:
    std::vector<ProviderCandidate> candidates_;
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
    std::vector<std::string> dynamic_tags;
};

struct ProcessImageSummary
{
    std::string primary_module;
    std::string layout_mode = "deterministic_analysis_layout";
    bool provider_search_complete = false;
    std::vector<ProcessModuleSummary> modules;
    std::vector<ProcessBinding> bindings;
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
    [[nodiscard]] std::vector<loader::UnresolvedRelocation> unresolved_relocations() const;
    [[nodiscard]] ProcessImageSummary summary() const;

  private:
    friend Result<ProcessImage> load_process_image(
        std::span<const ProcessModuleInput> inputs, const ProcessImageOptions& options);

    memory::GuestMemory memory_;
    std::string primary_module_;
    bool provider_search_complete_ = false;
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
