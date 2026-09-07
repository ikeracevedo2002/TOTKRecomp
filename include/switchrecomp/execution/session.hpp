#pragma once

#include "switchrecomp/analysis/whole_module.hpp"
#include "switchrecomp/common/result.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/runtime/execution.hpp"

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
    GuestTrap,
    ReturnTargetMismatch,
    CallDepthExceeded,
    FunctionTransitionLimitExceeded,
    IrOperationLimitExceeded,
    EventLimitExceeded,
    GuestBlockLimitExceeded,
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
};

struct ImportBoundary
{
    memory::GuestAddress relocation_target = 0U;
    std::size_t relocation_index = 0U;
    format::Relocation relocation{};
    format::ImportSymbol symbol{};
};

class ImportBoundaryIndex
{
  public:
    explicit ImportBoundaryIndex(const std::vector<loader::UnresolvedRelocation>& relocations);

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

struct ExecutionSessionResult
{
    static constexpr std::uint32_t schema_version = 1U;

    analysis::ModuleIdentity identity;
    EntrySelection entry;
    ExecutionSessionOptions options;
    ExecutionLoadSummary relocations;
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
    std::optional<memory::GuestAddress> target;
    std::string target_register;
    std::string target_provenance;
    std::optional<ImportBoundary> import_boundary;
    std::string diagnostic;
    std::vector<memory::GuestAddress> executed_functions;
    std::size_t direct_calls = 0U;
    std::size_t indirect_calls = 0U;
    std::size_t function_transfers = 0U;
    std::size_t returns = 0U;
    std::size_t ir_operations = 0U;
    std::size_t guest_blocks = 0U;
    std::size_t maximum_call_depth = 0U;
    std::vector<CallStackFrame> call_stack;
    std::vector<ExecutionEvent> events;
};

class ExecutionSession
{
  public:
    ExecutionSession(memory::GuestMemory& memory, const analysis::FinalizedFunctionMap& function_map,
                      const std::vector<loader::UnresolvedRelocation>& unresolved_relocations,
                      ExecutionSessionOptions options = {},
                      ExecutionLoadSummary load_summary = {});

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
    [[nodiscard]] Result<void> stop(ExecutionSessionResult& result, ExecutionStopReason reason,
                                    std::string diagnostic, std::optional<memory::GuestAddress> target = std::nullopt,
                                    const runtime::ExecutionResult* boundary = nullptr);
    [[nodiscard]] bool record_event(ExecutionSessionResult& result, ExecutionEvent event);
    [[nodiscard]] ExecutionStopReason classify_error(const Error& error) const noexcept;
    [[nodiscard]] Result<void> classify_target(const runtime::ExecutionResult& boundary,
                                               ExecutionSessionResult& result, bool call);
    [[nodiscard]] const analysis::FunctionRecord* function_record(
        memory::GuestAddress entry) const noexcept;

    memory::GuestMemory* memory_ = nullptr;
    const analysis::FinalizedFunctionMap* function_map_ = nullptr;
    ImportBoundaryIndex imports_;
    ExecutionSessionOptions options_;
    ExecutionLoadSummary load_summary_;
    std::map<memory::GuestAddress, LiftCacheEntry> lift_cache_;
    std::vector<SessionFrame> suspended_frames_;
    SessionFrame current_;
    runtime::CpuState cpu_{};
    runtime::RuntimeContext runtime_{};
    bool running_ = false;
};

[[nodiscard]] std::string render_execution_report_json(const ExecutionSessionResult& result);

} // namespace switchrecomp::execution
