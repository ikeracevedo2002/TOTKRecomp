#pragma once

#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace switchrecomp::analysis
{

struct ModuleSetByteInput
{
    std::string logical_name;
    std::span<const std::byte> file_bytes;
    std::optional<memory::GuestAddress> explicit_base;
    std::string name_provenance = "explicit_configuration";
};

struct ModuleSetFileInput
{
    std::string logical_name;
    std::filesystem::path path;
    std::optional<memory::GuestAddress> explicit_base;
    std::string name_provenance = "explicit_configuration";
};

struct ModuleInventoryRange
{
    std::uint32_t memory_offset = 0U;
    std::uint32_t memory_size = 0U;
};

struct ModuleInventoryEntry
{
    std::string logical_name;
    std::string name_provenance;
    std::string sha256;
    std::string build_id;
    std::uint64_t input_size = 0U;
    std::uint32_t nso_flags = 0U;
    std::string materialization_status;
    ModuleInventoryRange text;
    ModuleInventoryRange rodata;
    ModuleInventoryRange data;
    ModuleInventoryRange bss;
    std::optional<memory::GuestAddress> explicit_base;
    std::optional<memory::GuestAddress> selected_analysis_base;
    std::string base_provenance;
    bool runtime_base_verified = false;
    std::string mod0_status = "not_parsed";
    std::string dynamic_status = "not_parsed";
    std::size_t dynamic_symbol_count = 0U;
    std::size_t defined_symbol_count = 0U;
    std::size_t undefined_symbol_count = 0U;
    std::size_t relocation_count = 0U;
    bool executable_mapping = true;
    bool module_load_valid = true;
    bool provider_index_eligible = false;
    std::vector<GuestAddressRange> executable_guest_mappings;
};

struct ModuleSetIngestionOptions
{
    struct ExpectedModule
    {
        std::string logical_name;
        std::string sha256;
        std::string build_id;
        std::optional<std::uint64_t> input_size;
    };

    ModuleSetCompleteness completeness = ModuleSetCompleteness::Incomplete;
    ModuleSetCompletenessBasis completeness_basis = ModuleSetCompletenessBasis::LegacyConfigFalse;
    ModuleSetCoherence coherence = ModuleSetCoherence::Unverified;
    std::string coherence_basis = "unknown";
    std::string source = "explicit";
    std::vector<std::string> expected_logical_names;
    std::vector<ExpectedModule> expected_modules;
    std::size_t max_modules = 128U;
    std::size_t max_file_size = std::size_t{512U} * 1024U * 1024U;
};

struct DirectoryInventoryOptions : ModuleSetIngestionOptions
{
    std::size_t max_directory_entries = 4096U;
    std::map<std::string, memory::GuestAddress> explicit_bases;
};

class ModuleSetInventory
{
  public:
    ModuleSetCompleteness completeness = ModuleSetCompleteness::Incomplete;
    ModuleSetCompletenessBasis completeness_basis = ModuleSetCompletenessBasis::LegacyConfigFalse;
    ModuleSetCoherence coherence = ModuleSetCoherence::Unverified;
    std::string coherence_basis = "unknown";
    std::string source = "explicit";
    // This is evidence about physical ExeFS loading order only. It is not
    // symbol lookup precedence and is never used to break provider ties.
    std::vector<std::string> module_load_order;
    std::string module_load_order_basis = "not_established";
    std::vector<ModuleInventoryEntry> modules;
    // These are logical directory entry names and stable ignore reasons, never
    // host paths. They are retained so an inventory explains what was searched.
    std::vector<std::string> ignored_entries;

    [[nodiscard]] std::vector<ProcessModuleInput> process_inputs() const;
    void enrich_from_process_summary(const ProcessImageSummary& summary);
    [[nodiscard]] bool empty() const noexcept { return owned_modules_.empty(); }

  private:
    struct OwnedModule
    {
        std::string logical_name;
        std::string name_provenance;
        std::vector<std::byte> bytes;
        std::optional<memory::GuestAddress> explicit_base;
    };

    friend Result<ModuleSetInventory> validate_module_set(
        std::span<const ModuleSetByteInput> inputs, const ModuleSetIngestionOptions& options);
    friend Result<ModuleSetInventory> validate_impl(
        std::span<const ModuleSetByteInput> inputs, const ModuleSetIngestionOptions& options);
    friend Result<ModuleSetInventory> scan_prepared_module_directory(
        const std::filesystem::path& directory, const DirectoryInventoryOptions& options);

    std::vector<OwnedModule> owned_modules_;
};

[[nodiscard]] Result<ModuleSetInventory> validate_module_set(
    std::span<const ModuleSetByteInput> inputs,
    const ModuleSetIngestionOptions& options = {});

[[nodiscard]] Result<ModuleSetInventory> ingest_module_files(
    std::span<const ModuleSetFileInput> inputs,
    const ModuleSetIngestionOptions& options = {});

[[nodiscard]] Result<ModuleSetInventory> scan_prepared_module_directory(
    const std::filesystem::path& directory,
    const DirectoryInventoryOptions& options = {});

} // namespace switchrecomp::analysis
