#pragma once

#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/common/sha256.hpp"
#include "switchrecomp/format/dynamic_symbols.hpp"
#include "switchrecomp/format/elf_rela.hpp"
#include "switchrecomp/format/module_metadata.hpp"
#include "switchrecomp/format/nso.hpp"
#include "switchrecomp/loader/relocation_processor.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace switchrecomp::analysis
{

struct PreparedModuleOptions
{
    std::string module_name = "main";
    memory::GuestAddress module_base = 0U;
    bool seed_text_entry = true;
    bool apply_relocations = true;
    std::vector<FunctionSeed> seeds;
    format::NsoMaterializationLimits materialization_limits;
    memory::GuestMemoryLimits memory_limits;
    format::ModuleMetadataParseOptions metadata_options;
    loader::RelocationProcessorOptions relocation_options;
};

struct LoadedModule
{
    ModuleIdentity identity;
    format::NsoImage image;
    memory::GuestMemory memory;
    format::ModuleMetadata metadata;
    std::optional<format::DynamicSymbolTable> symbols;
    std::vector<format::Relocation> relocations;
    std::size_t applied_relocations = 0U;
    std::vector<loader::UnresolvedRelocation> unresolved_relocations;
    std::vector<format::ImportSymbol> unresolved_imports;
    std::vector<FunctionSeed> seeds;
};

[[nodiscard]] Result<LoadedModule> load_prepared_nso(
    std::span<const std::byte> file_bytes, const PreparedModuleOptions& options = {});

enum class TranslationMode : std::uint8_t
{
    Strict,
    Diagnostic,
};

[[nodiscard]] std::string_view translation_mode_name(TranslationMode mode) noexcept;

struct TranslationOptions
{
    TranslationMode mode = TranslationMode::Strict;
    bool lower_llvm = false;
    bool include_ir = false;
    FunctionMapOptions function_map;
    lifter::LiftOptions lift;
};

struct FunctionTranslationResult
{
    std::string module;
    std::string function;
    memory::GuestAddress guest_entry = 0U;
    TranslationStatus status = TranslationStatus::Discovered;
    std::optional<ir::Function> ir;
    std::string ir_text;
    std::string llvm_ir;
    std::vector<UnsupportedRecord> unsupported;
    std::vector<FunctionDiagnostic> diagnostics;
};

struct CoverageFamily
{
    std::string family;
    std::size_t decoded = 0U;
    std::size_t supported = 0U;
    std::size_t unsupported = 0U;
};

struct UnsupportedSummary
{
    std::string instruction;
    std::string semantic_family;
    std::size_t count = 0U;
    std::size_t functions = 0U;
    std::vector<memory::GuestAddress> example_pcs;
};

struct ModuleCoverage
{
    AnalysisBudgets budgets;
    std::size_t executable_bytes = 0U;
    std::size_t decoded_instructions = 0U;
    std::size_t supported_instructions = 0U;
    std::size_t unsupported_instructions = 0U;
    std::size_t decode_failures = 0U;

    std::size_t function_seeds = 0U;
    std::size_t functions_discovered = 0U;
    std::size_t functions_analyzed = 0U;
    std::size_t functions_fully_lifted = 0U;
    std::size_t functions_verified = 0U;
    std::size_t functions_translated = 0U;
    std::size_t functions_unsupported = 0U;
    std::size_t functions_failed = 0U;
    std::size_t conflicting_functions = 0U;

    memory::GuestSize bytes_analyzed = 0U;
    memory::GuestSize ownership_bytes = 0U;
    memory::GuestSize envelope_span_bytes = 0U;
    std::size_t basic_blocks = 0U;
    std::size_t cfg_edges = 0U;
    std::size_t function_transfers = 0U;
    std::size_t direct_calls = 0U;
    std::size_t indirect_calls = 0U;
    std::size_t resolved_indirect_calls = 0U;
    std::size_t unresolved_indirect_calls = 0U;
    std::size_t ir_verification_failures = 0U;
    std::size_t runtime_import_boundaries = 0U;
    std::size_t unresolved_imports = 0U;
    std::size_t unresolved_relocation_bindings = 0U;
    std::size_t unresolved_plt_relocations = 0U;
    std::size_t unresolved_non_plt_relocations = 0U;
    std::size_t lifted_instructions = 0U;
    bool analysis_budget_exhausted = false;

    std::vector<CoverageFamily> families;
    std::vector<UnsupportedSummary> top_unsupported;
};

struct WholeModuleTranslationResult
{
    ModuleIdentity identity;
    TranslationMode mode = TranslationMode::Strict;
    FinalizedFunctionMap function_map;
    ModuleCoverage coverage;
    std::vector<FunctionTranslationResult> functions;
    std::vector<format::DynamicSymbol> symbols;
    std::vector<format::ImportSymbol> unresolved_imports;
    std::vector<format::Relocation> relocations;
    std::size_t applied_relocations = 0U;
    std::vector<loader::UnresolvedRelocation> unresolved_relocations;
    bool strict_success = false;

    [[nodiscard]] bool succeeded() const noexcept { return strict_success; }
};

[[nodiscard]] Result<WholeModuleTranslationResult> translate_module(
    const ModuleAnalysisInput& input, const TranslationOptions& options = {});

[[nodiscard]] Result<WholeModuleTranslationResult> translate_module(
    const LoadedModule& module, const TranslationOptions& options = {});

[[nodiscard]] std::string render_function_map_json(const FinalizedFunctionMap& map);
[[nodiscard]] std::string render_translation_report_json(
    const WholeModuleTranslationResult& result);
[[nodiscard]] std::string render_translation_report(
    const WholeModuleTranslationResult& result);

} // namespace switchrecomp::analysis
