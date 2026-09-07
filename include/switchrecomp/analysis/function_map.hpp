#pragma once

#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace switchrecomp::analysis
{

enum class FunctionDiscoverySource : std::uint8_t
{
    ModuleEntry,
    DynamicSymbol,
    Export,
    DirectCall,
    RelocationReference,
    AnalystSeed,
    ManualOverride,
    JumpTable,
    Heuristic,
};

enum class FunctionConfidence : std::uint8_t
{
    Confirmed,
    High,
    Medium,
    Low,
    Manual,
    Conflict,
};

enum class TranslationStatus : std::uint8_t
{
    Discovered,
    Analyzed,
    Lifted,
    Verified,
    Lowerable,
    Translated,
    Unsupported,
    Excluded,
    Failed,
    Conflict,
};

enum class FailureCategory : std::uint8_t
{
    DecodeFailure,
    InvalidInstruction,
    InvalidCFG,
    UnsupportedInstruction,
    UnsupportedSemantic,
    UnresolvedIndirectFlow,
    FunctionBoundaryConflict,
    InvalidGuestAddress,
    AnalysisBudgetExceeded,
    IRVerificationFailure,
    MissingImportBinding,
    RuntimeBoundary,
    CodegenFailure,
};

[[nodiscard]] std::string_view function_discovery_source_name(
    FunctionDiscoverySource source) noexcept;
[[nodiscard]] std::string_view function_confidence_name(FunctionConfidence confidence) noexcept;
[[nodiscard]] std::string_view translation_status_name(TranslationStatus status) noexcept;
[[nodiscard]] std::string_view failure_category_name(FailureCategory category) noexcept;

struct ModuleIdentity
{
    std::string module;
    std::string build_id;
    std::string input_sha256;
    memory::GuestAddress guest_base = 0U;
    std::vector<GuestAddressRange> executable_ranges;
    std::string translator_version;
    std::uint32_t metadata_schema_version = 1U;
    std::string llvm_version;
    std::vector<std::string> feature_flags;
};

struct FunctionSeed
{
    memory::GuestAddress entry = 0U;
    FunctionDiscoverySource source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    std::optional<memory::GuestAddress> canonical_entry;
    std::optional<std::string> name;
    std::string note;
};

struct DiscoveryEvidence
{
    FunctionDiscoverySource source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    memory::GuestAddress entry = 0U;
    std::string note;
};

struct FunctionDiagnostic
{
    FailureCategory category = FailureCategory::InvalidCFG;
    ErrorCode code = ErrorCode::InvalidControlFlow;
    memory::GuestAddress pc = 0U;
    std::optional<std::uint32_t> opcode;
    std::string message;
};

struct UnsupportedRecord
{
    std::string module;
    std::string function;
    memory::GuestAddress pc = 0U;
    std::uint32_t opcode = 0U;
    std::string instruction;
    std::string semantic_family;
    std::string reason;
    std::string required_action;
};

struct FunctionBoundaryConflict
{
    std::string module;
    memory::GuestAddress first_function = 0U;
    memory::GuestAddress second_function = 0U;
    GuestAddressRange first_range;
    GuestAddressRange second_range;
    FunctionDiscoverySource first_source = FunctionDiscoverySource::Heuristic;
    FunctionDiscoverySource second_source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence first_confidence = FunctionConfidence::Low;
    FunctionConfidence second_confidence = FunctionConfidence::Low;
    std::string resolution;
};

struct FunctionRecord
{
    std::string module;
    std::string synthetic_id;
    memory::GuestAddress canonical_entry = 0U;
    std::vector<memory::GuestAddress> entries;
    memory::GuestAddress range_begin = 0U;
    memory::GuestAddress range_end = 0U;
    FunctionDiscoverySource primary_source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    std::optional<std::string> name;
    std::vector<DiscoveryEvidence> evidence;
    std::optional<ControlFlowGraph> cfg;
    std::vector<memory::GuestAddress> direct_calls;
    std::vector<CallSite> indirect_calls;
    std::vector<UnresolvedControlFlow> unresolved_control_flow;
    TranslationStatus translation_status = TranslationStatus::Discovered;
    std::vector<UnsupportedRecord> unsupported;
    std::vector<FunctionDiagnostic> diagnostics;
};

struct AnalysisBudgets
{
    std::size_t max_functions = 1'000'000U;
    std::size_t max_blocks = 16'384U;
    std::size_t max_instructions = 4'000'000U;
    std::size_t max_edges = 8'000'000U;
    std::size_t max_seeds = 2'000'000U;
    memory::GuestSize max_bytes_analyzed = memory::GuestSize{64U} * 1024U * 1024U;
};

struct ModuleAnalysisInput
{
    ModuleIdentity identity;
    const memory::GuestMemory* memory = nullptr;
    std::vector<FunctionSeed> seeds;
};

struct FunctionMapOptions
{
    AnalysisBudgets budgets;
    AnalysisOptions cfg;
    bool continue_after_function_failure = true;
};

class FinalizedFunctionMap
{
  public:
    FinalizedFunctionMap() = default;

    [[nodiscard]] const ModuleIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] const std::vector<FunctionRecord>& functions() const noexcept
    {
        return functions_;
    }
    [[nodiscard]] const std::vector<FunctionBoundaryConflict>& conflicts() const noexcept
    {
        return conflicts_;
    }
    [[nodiscard]] const FunctionRecord* find(memory::GuestAddress entry) const noexcept;
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

  private:
    friend class FunctionMapBuilder;
    friend Result<void> validate_finalized_function_map(const FinalizedFunctionMap& map);
    ModuleIdentity identity_;
    std::vector<FunctionRecord> functions_;
    std::vector<FunctionBoundaryConflict> conflicts_;
    bool frozen_ = false;
};

class FunctionMapBuilder
{
  public:
    [[nodiscard]] static Result<FinalizedFunctionMap> build(
        const ModuleAnalysisInput& input, const FunctionMapOptions& options = {});
};

[[nodiscard]] Result<void> validate_finalized_function_map(const FinalizedFunctionMap& map);

} // namespace switchrecomp::analysis
