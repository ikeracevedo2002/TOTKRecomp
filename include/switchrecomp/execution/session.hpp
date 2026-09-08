#pragma once

#include "switchrecomp/analysis/whole_module.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/common/result.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/runtime/execution.hpp"
#include "switchrecomp/runtime/imports.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace switchrecomp::execution
{

enum class EntrySelectionKind
{
    DynamicInit,
    DynamicFini,
    TextStartCandidate,
    AnalystAddress,
    VerifiedProcessEntry,
};

[[nodiscard]] const char* entry_selection_kind_name(EntrySelectionKind kind) noexcept;

struct EntrySelection
{
    EntrySelectionKind kind = EntrySelectionKind::DynamicInit;
    memory::GuestAddress address = 0U;
    std::string source;
    analysis::FunctionConfidence confidence = analysis::FunctionConfidence::Low;
    bool verified_process_entry = false;
};

[[nodiscard]] Result<EntrySelection> select_entry(
    const analysis::ModuleIdentity& identity, EntrySelectionKind kind,
    std::optional<memory::GuestAddress> analyst_address = std::nullopt);

enum class ExecutionStopReason
{
    EntryReturned,
    UnresolvedImport,
    UnknownGuestFunction,
    InvalidIndirectTarget,
    UnresolvedIndirectControlFlow,
    UnsupportedInstruction,
    UnsupportedSemantic,
    FunctionOwnershipConflict,
    MemoryFault,
    RuntimeServiceBoundary,
    RuntimeImportUnimplemented,
    RuntimeImportAbiViolation,
    RuntimeImportMemoryFault,
    RuntimeImportInvariantViolation,
    GuestTrap,
    ReturnTargetMismatch,
    CallDepthExceeded,
    FunctionTransitionLimitExceeded,
    IrOperationLimitExceeded,
    EventLimitExceeded,
    GuestBlockLimitExceeded,
    InvalidCrossModuleTarget,
    IndirectTargetRefinementBudgetExceeded,
};

[[nodiscard]] const char* execution_stop_reason_name(ExecutionStopReason reason) noexcept;

enum class ExecutionEligibility
{
    Runnable,
    RunnableToBoundary,
    OwnershipConflict,
    UnsupportedInstruction,
    InvalidCFG,
    IrVerificationFailure,
    UnknownFunction,
    OtherFailure,
};

[[nodiscard]] const char* execution_eligibility_name(ExecutionEligibility eligibility) noexcept;

enum class ExecutionEventKind
{
    SessionStart,
    EntrySelected,
    StackMapped,
    FunctionEnter,
    DirectCall,
    IndirectCall,
    FunctionTransfer,
    Return,
    FunctionResume,
    ImportBoundary,
    RuntimeImportResolved,
    RuntimeImportEnter,
    RuntimeImportArgumentSummary,
    RuntimeStateRegistration,
    RuntimeImportReturn,
    RuntimeImportBoundary,
    IndirectBoundary,
    MemoryFault,
    UnsupportedBoundary,
    SessionStop,
};

[[nodiscard]] const char* execution_event_kind_name(ExecutionEventKind kind) noexcept;

struct ExecutionEvent
{
    std::size_t sequence = 0U;
    ExecutionEventKind kind = ExecutionEventKind::SessionStart;
    memory::GuestAddress function_entry = 0U;
    memory::GuestAddress guest_pc = 0U;
    memory::GuestAddress target = 0U;
    bool has_target = false;
    std::size_t call_depth = 0U;
    runtime::ExecutionBoundaryKind boundary = runtime::ExecutionBoundaryKind::None;
    std::string code;
    std::string import_symbol;
    std::string function_module;
    std::string target_module;
};

struct ExecutionBudgets
{
    std::size_t max_ir_operations = 100'000U;
    std::size_t max_function_transitions = 1'000U;
    std::size_t max_call_depth = 128U;
    std::size_t max_events = 4'096U;
    std::size_t max_guest_blocks = 1'000'000U;
};

struct ExecutionSessionOptions
{
    ExecutionBudgets budgets;
    memory::GuestSize synthetic_stack_size = memory::GuestSize{1U} * 1024U * 1024U;
    memory::GuestSize stack_guard_gap = 0x10000U;
};

struct ExecutionLoadSummary
{
    std::size_t relocations_parsed = 0U;
    std::size_t relocations_applied = 0U;
    std::size_t unresolved_relocations = 0U;
    std::size_t guest_bindings_attempted = 0U;
    std::size_t guest_bindings_resolved = 0U;
    std::size_t guest_bindings_ambiguous = 0U;
    std::size_t guest_bindings_unresolved = 0U;
    std::size_t cross_module_relocations = 0U;
};

struct ImportBoundary
{
    memory::GuestAddress relocation_target = 0U;
    std::size_t relocation_index = 0U;
    format::Relocation relocation{};
    format::ImportSymbol symbol{};
    std::string consumer_module;
};

class ImportBoundaryIndex
{
  public:
    explicit ImportBoundaryIndex(const std::vector<loader::UnresolvedRelocation>& relocations);
    explicit ImportBoundaryIndex(const analysis::ProcessImage& process_image);

    [[nodiscard]] const ImportBoundary* find(memory::GuestAddress relocation_target) const noexcept;

  private:
    std::map<memory::GuestAddress, ImportBoundary> entries_;
};

struct CallStackFrame
{
    memory::GuestAddress function_entry = 0U;
    memory::GuestAddress call_site_pc = 0U;
    memory::GuestAddress expected_return_pc = 0U;
    std::size_t call_depth = 0U;
};

struct RuntimeAbiArgumentObservation
{
    std::size_t index = 0U;
    std::string location;
    bool readable = false;
    std::uint64_t value = 0U;
    std::string diagnostic;
    std::string mapping;
    std::string permissions;
    std::string region_kind;
    bool aligned_8 = false;
    bool aligned_16 = false;
    std::vector<std::string> dynamic_tags;
    std::vector<std::string> dynamic_symbols;
    std::vector<std::string> relocation_ranges;
    std::string range_validity;
};

struct RuntimeImportObservation
{
    runtime::RuntimeImportDescriptor descriptor;
    ImportBoundary provenance;
    runtime::ExternalInvocationKind invocation = runtime::ExternalInvocationKind::Call;
    memory::GuestAddress source_guest_pc = 0U;
    runtime::GuestAbiSnapshot abi;
    std::vector<RuntimeAbiArgumentObservation> arguments;
    std::string abi_validation;
    std::string abi_diagnostic;
    std::string outcome;
    std::string outcome_diagnostic;
    std::string trampoline_classification;
    std::vector<std::string> trampoline_evidence;
    std::optional<memory::GuestAddress> dynamic_pltgot;
    std::optional<std::uint64_t> dynamic_pltgot_slot_delta;
    std::string guest_provider_resolution;
    bool runtime_fallback_eligible = false;
};

struct RuntimeExecutionSummary
{
    std::size_t imports_encountered = 0U;
    std::size_t imports_resolved = 0U;
    std::size_t imports_handled = 0U;
    std::size_t imports_unimplemented = 0U;
    std::size_t imports_abi_violations = 0U;
    std::size_t imports_memory_faults = 0U;
    std::size_t imports_invariant_violations = 0U;
    std::size_t import_returns = 0U;
    std::size_t dso_modules_registered = 0U;
    std::vector<RuntimeImportObservation> imports;
};

struct ExecutedGuestInstruction
{
    memory::GuestAddress guest_pc = 0U;
    std::string module;
    std::string mnemonic;
    std::string instruction_id;
    bool executed = false;
    std::optional<memory::GuestAddress> next_guest_pc;
    std::string destination_register;
    std::vector<std::string> source_registers;
    struct RegisterValue
    {
        std::string name;
        std::uint64_t value = 0U;
    };
    std::vector<RegisterValue> pre_registers;
    std::vector<RegisterValue> post_registers;
    std::optional<std::uint64_t> expected_high64;
    std::optional<std::uint64_t> actual_high64;
    std::optional<bool> result_matches;
    std::size_t call_depth = 0U;
};

struct ExecutionSessionResult
{
    static constexpr std::uint32_t schema_version = 6U;

    analysis::ModuleIdentity identity;
    EntrySelection entry;
    ExecutionSessionOptions options;
    ExecutionLoadSummary relocations;
    std::optional<format::ModuleMetadata> module_metadata;
    std::size_t analyzed_functions = 0U;
    std::size_t conflicting_functions = 0U;
    std::size_t precise_conflicts = 0U;
    memory::GuestSize precise_owned_bytes = 0U;
    memory::GuestAddress stack_base = 0U;
    memory::GuestAddress stack_end = 0U;
    memory::GuestAddress initial_sp = 0U;
    memory::GuestAddress synthetic_lr_sentinel = 0U;
    runtime::CpuState final_cpu{};
    ExecutionStopReason stop_reason = ExecutionStopReason::UnsupportedSemantic;
    memory::GuestAddress stop_pc = 0U;
    memory::GuestAddress current_function = 0U;
    std::optional<memory::GuestAddress> source_pc;
    std::optional<memory::GuestAddress> diagnostic_pc;
    std::optional<memory::GuestAddress> target;
    std::string target_register;
    std::string target_provenance;
    std::optional<std::uint32_t> diagnostic_opcode;
    std::string diagnostic_instruction_id;
    std::string diagnostic_instruction;
    std::optional<ImportBoundary> import_boundary;
    std::string diagnostic;
    std::string current_function_module;
    std::string stop_module;
    std::optional<analysis::ProcessImageSummary> process;
    bool provider_guest_code_entered = false;
    std::vector<memory::GuestAddress> executed_functions;
    std::vector<std::string> executed_function_modules;
    std::size_t direct_calls = 0U;
    std::size_t indirect_calls = 0U;
    std::size_t function_transfers = 0U;
    std::size_t returns = 0U;
    std::size_t ir_operations = 0U;
    std::size_t guest_blocks = 0U;
    std::size_t guest_instruction_count = 0U;
    std::size_t instructions_after_former_blocker = 0U;
    std::size_t maximum_call_depth = 0U;
    std::vector<CallStackFrame> call_stack;
    std::vector<memory::GuestAddress> observation_targets;
    std::vector<memory::GuestAddress> instruction_observation_targets;
    std::vector<ExecutedGuestInstruction> executed_guest_instructions;
    std::vector<ExecutedGuestInstruction> instruction_evidence;
    std::vector<analysis::IndirectTargetAssessment> indirect_target_discovery;
    std::vector<ExecutionEvent> events;
    RuntimeExecutionSummary runtime;
};

class ExecutionSession
{
  public:
    ExecutionSession(memory::GuestMemory& memory, const analysis::FinalizedFunctionMap& function_map,
                      const std::vector<loader::UnresolvedRelocation>& unresolved_relocations,
                      ExecutionSessionOptions options = {},
                      ExecutionLoadSummary load_summary = {},
                      runtime::RuntimeImportRegistry* runtime_imports = nullptr,
                      const format::ModuleMetadata* module_metadata = nullptr,
                      const format::DynamicSymbolTable* symbols = nullptr);

    ExecutionSession(memory::GuestMemory& memory, const analysis::ProcessFunctionMap& function_map,
                      const analysis::ProcessImage& process_image,
                      ExecutionSessionOptions options = {},
                      ExecutionLoadSummary load_summary = {},
                      runtime::RuntimeImportRegistry* runtime_imports = nullptr);

    ExecutionSession(memory::GuestMemory& memory, const analysis::ProcessFunctionMap& function_map,
                      const std::vector<loader::UnresolvedRelocation>& unresolved_relocations,
                      ExecutionSessionOptions options = {},
                      ExecutionLoadSummary load_summary = {},
                      runtime::RuntimeImportRegistry* runtime_imports = nullptr);

    [[nodiscard]] Result<ExecutionSessionResult> run(const EntrySelection& entry);

  private:
    struct LiftCacheEntry
    {
        std::optional<ir::Function> function;
        std::optional<Error> error;
    };

    struct SessionFrame
    {
        memory::GuestAddress function_entry = 0U;
        memory::GuestAddress call_site_pc = 0U;
        memory::GuestAddress expected_return_pc = 0U;
        std::size_t call_depth = 0U;
        interpreter::InterpreterFrame interpreter;
    };

    [[nodiscard]] Result<void> map_stack(ExecutionSessionResult& result);
    [[nodiscard]] Result<const ir::Function*> lift_for_execution(
        memory::GuestAddress entry, ExecutionSessionResult& result);
    [[nodiscard]] Result<void> enter_function(memory::GuestAddress entry,
                                              ExecutionSessionResult& result);
    [[nodiscard]] Result<void> dispatch_call(const runtime::ExecutionResult& boundary,
                                             ExecutionSessionResult& result);
    [[nodiscard]] Result<void> dispatch_transfer(const runtime::ExecutionResult& boundary,
                                                 ExecutionSessionResult& result);
    [[nodiscard]] Result<void> dispatch_runtime_import(
        const runtime::ExecutionResult& boundary, const ImportBoundary& import,
        runtime::ExternalInvocationKind invocation, ExecutionSessionResult& result);
    [[nodiscard]] Result<void> stop(ExecutionSessionResult& result, ExecutionStopReason reason,
                                    std::string diagnostic, std::optional<memory::GuestAddress> target = std::nullopt,
                                    const runtime::ExecutionResult* boundary = nullptr);
    [[nodiscard]] bool record_event(ExecutionSessionResult& result, ExecutionEvent event);
    [[nodiscard]] ExecutionStopReason classify_error(const Error& error) const noexcept;
    [[nodiscard]] Result<void> classify_target(const runtime::ExecutionResult& boundary,
                                               ExecutionSessionResult& result, bool call);
    [[nodiscard]] const analysis::FunctionRecord* function_record(
        memory::GuestAddress entry) const noexcept;
    [[nodiscard]] const analysis::FinalizedFunctionMap* function_map_for(
        memory::GuestAddress entry) const noexcept;
    [[nodiscard]] std::string module_name_for(memory::GuestAddress entry) const;
    void prepare_observation_targets();
    [[nodiscard]] std::optional<ExecutedGuestInstruction> describe_observed_instruction(
        const runtime::ObservedInstructionExecution& observation, std::size_t call_depth) const;

    memory::GuestMemory* memory_ = nullptr;
    const analysis::FinalizedFunctionMap* function_map_ = nullptr;
    const analysis::ProcessFunctionMap* process_function_map_ = nullptr;
    const analysis::ProcessImage* process_image_ = nullptr;
    ImportBoundaryIndex imports_;
    runtime::RuntimeImportRegistry empty_runtime_imports_;
    runtime::RuntimeImportRegistry* runtime_imports_ = nullptr;
    const format::ModuleMetadata* module_metadata_ = nullptr;
    const format::DynamicSymbolTable* symbols_ = nullptr;
    ExecutionSessionOptions options_;
    ExecutionLoadSummary load_summary_;
    std::map<memory::GuestAddress, LiftCacheEntry> lift_cache_;
    std::vector<memory::GuestAddress> observation_targets_;
    std::vector<memory::GuestAddress> instruction_observation_targets_;
    std::vector<SessionFrame> suspended_frames_;
    SessionFrame current_;
    runtime::CpuState cpu_{};
    runtime::RuntimeContext runtime_{};
    runtime::RuntimeState runtime_state_{};
    bool running_ = false;
};

[[nodiscard]] std::string render_execution_report_json(const ExecutionSessionResult& result);

} // namespace switchrecomp::execution
