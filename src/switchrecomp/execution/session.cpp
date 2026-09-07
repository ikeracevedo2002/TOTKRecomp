#include "switchrecomp/execution/session.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <utility>

namespace switchrecomp::execution
{

namespace
{

using GuestAddress = memory::GuestAddress;
using json = nlohmann::json;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] json range_json(const analysis::GuestAddressRange& range)
{
    return json{{"base", hex_address(range.base)}, {"size", range.size}};
}

[[nodiscard]] json cpu_json(const runtime::CpuState& cpu)
{
    json registers = json::array();
    for (const auto value : cpu.x)
    {
        registers.push_back(hex_address(value));
    }
    return json{{"pc", hex_address(cpu.pc)}, {"sp", hex_address(cpu.sp)},
                {"x30", hex_address(cpu.x[30U])}, {"x", std::move(registers)}};
}

[[nodiscard]] bool is_memory_error(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::OutOfBounds:
    case ErrorCode::UnmappedMemory:
    case ErrorCode::PermissionDenied:
    case ErrorCode::InstructionFetchFailed:
    case ErrorCode::NonExecutableAddress:
    case ErrorCode::InvalidGuestAddress:
    case ErrorCode::MisalignedAtomicAccess:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] Result<GuestAddress> align_up(GuestAddress value, GuestAddress alignment)
{
    const auto mask = alignment - 1U;
    const auto rounded = checked_add_u64(value, mask);
    if (!rounded)
    {
        return Result<GuestAddress>::failure(rounded.error());
    }
    return Result<GuestAddress>::success(rounded.value() & ~mask);
}

} // namespace

const char* entry_selection_kind_name(EntrySelectionKind kind) noexcept
{
    switch (kind)
    {
    case EntrySelectionKind::DynamicInit: return "dynamic_init";
    case EntrySelectionKind::DynamicFini: return "dynamic_fini";
    case EntrySelectionKind::TextStartCandidate: return "text_start_candidate";
    case EntrySelectionKind::AnalystAddress: return "analyst_address";
    case EntrySelectionKind::VerifiedProcessEntry: return "verified_process_entry";
    }
    return "unknown";
}

Result<EntrySelection> select_entry(const analysis::ModuleIdentity& identity,
                                    EntrySelectionKind kind,
                                    std::optional<GuestAddress> analyst_address)
{
    if (kind == EntrySelectionKind::AnalystAddress)
    {
        if (!analyst_address)
        {
            return Result<EntrySelection>::failure(make_error(
                ErrorCode::InvalidArgument,
                "analyst address entry selection requires --entry-address"));
        }
        return Result<EntrySelection>::success(EntrySelection{
            kind, analyst_address.value(), "analyst-supplied guest address",
            analysis::FunctionConfidence::Manual, false});
    }

    const auto wanted = [&]() -> std::optional<analysis::EntryPointKind> {
        switch (kind)
        {
        case EntrySelectionKind::DynamicInit: return analysis::EntryPointKind::DynamicInit;
        case EntrySelectionKind::DynamicFini: return analysis::EntryPointKind::DynamicFini;
        case EntrySelectionKind::TextStartCandidate:
            return analysis::EntryPointKind::TextStartCandidate;
        case EntrySelectionKind::VerifiedProcessEntry:
            return analysis::EntryPointKind::VerifiedProcessEntry;
        case EntrySelectionKind::AnalystAddress: return std::nullopt;
        }
        return std::nullopt;
    }();

    if (!wanted)
    {
        return Result<EntrySelection>::failure(make_error(
            ErrorCode::InvalidArgument, "entry selection kind is not supported"));
    }

    for (const auto& evidence : identity.entry_points)
    {
        if (evidence.kind == wanted.value() &&
            (kind != EntrySelectionKind::VerifiedProcessEntry || evidence.verified_runtime_entry))
        {
            return Result<EntrySelection>::success(EntrySelection{
                kind, evidence.address, evidence.provenance, evidence.confidence,
                evidence.verified_runtime_entry});
        }
    }

    if (kind == EntrySelectionKind::VerifiedProcessEntry)
    {
        return Result<EntrySelection>::failure(make_error(
            ErrorCode::InvalidArgument,
            "no verified process entry exists for this single-module prepared input"));
    }
    return Result<EntrySelection>::failure(make_error(
        ErrorCode::InvalidArgument,
        std::string("requested entry kind has no metadata candidate: ") +
            entry_selection_kind_name(kind)));
}

const char* execution_stop_reason_name(ExecutionStopReason reason) noexcept
{
    switch (reason)
    {
    case ExecutionStopReason::EntryReturned: return "entry_returned";
    case ExecutionStopReason::UnresolvedImport: return "unresolved_import";
    case ExecutionStopReason::UnknownGuestFunction: return "unknown_guest_function";
    case ExecutionStopReason::InvalidIndirectTarget: return "invalid_indirect_target";
    case ExecutionStopReason::UnresolvedIndirectControlFlow:
        return "unresolved_indirect_control_flow";
    case ExecutionStopReason::UnsupportedInstruction: return "unsupported_instruction";
    case ExecutionStopReason::UnsupportedSemantic: return "unsupported_semantic";
    case ExecutionStopReason::FunctionOwnershipConflict: return "function_ownership_conflict";
    case ExecutionStopReason::MemoryFault: return "memory_fault";
    case ExecutionStopReason::RuntimeServiceBoundary: return "runtime_service_boundary";
    case ExecutionStopReason::GuestTrap: return "guest_trap";
    case ExecutionStopReason::ReturnTargetMismatch: return "return_target_mismatch";
    case ExecutionStopReason::CallDepthExceeded: return "call_depth_exceeded";
    case ExecutionStopReason::FunctionTransitionLimitExceeded:
        return "function_transition_limit_exceeded";
    case ExecutionStopReason::IrOperationLimitExceeded: return "ir_operation_limit_exceeded";
    case ExecutionStopReason::EventLimitExceeded: return "event_limit_exceeded";
    case ExecutionStopReason::GuestBlockLimitExceeded: return "guest_block_limit_exceeded";
    }
    return "unknown";
}

const char* execution_eligibility_name(ExecutionEligibility eligibility) noexcept
{
    switch (eligibility)
    {
    case ExecutionEligibility::Runnable: return "runnable";
    case ExecutionEligibility::RunnableToBoundary: return "runnable_to_boundary";
    case ExecutionEligibility::OwnershipConflict: return "ownership_conflict";
    case ExecutionEligibility::UnsupportedInstruction: return "unsupported_instruction";
    case ExecutionEligibility::InvalidCFG: return "invalid_cfg";
    case ExecutionEligibility::IrVerificationFailure: return "ir_verification_failure";
    case ExecutionEligibility::UnknownFunction: return "unknown_function";
    case ExecutionEligibility::OtherFailure: return "other_failure";
    }
    return "unknown";
}

const char* execution_event_kind_name(ExecutionEventKind kind) noexcept
{
    switch (kind)
    {
    case ExecutionEventKind::SessionStart: return "session_start";
    case ExecutionEventKind::EntrySelected: return "entry_selected";
    case ExecutionEventKind::StackMapped: return "stack_mapped";
    case ExecutionEventKind::FunctionEnter: return "function_enter";
    case ExecutionEventKind::DirectCall: return "direct_call";
    case ExecutionEventKind::IndirectCall: return "indirect_call";
    case ExecutionEventKind::FunctionTransfer: return "function_transfer";
    case ExecutionEventKind::Return: return "return";
    case ExecutionEventKind::FunctionResume: return "function_resume";
    case ExecutionEventKind::ImportBoundary: return "import_boundary";
    case ExecutionEventKind::IndirectBoundary: return "indirect_boundary";
    case ExecutionEventKind::MemoryFault: return "memory_fault";
    case ExecutionEventKind::UnsupportedBoundary: return "unsupported_boundary";
    case ExecutionEventKind::SessionStop: return "session_stop";
    }
    return "unknown";
}

ImportBoundaryIndex::ImportBoundaryIndex(
    const std::vector<loader::UnresolvedRelocation>& relocations)
{
    std::vector<const loader::UnresolvedRelocation*> ordered;
    ordered.reserve(relocations.size());
    for (const auto& relocation : relocations)
    {
        ordered.push_back(&relocation);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->relocation.target_address != right->relocation.target_address)
        {
            return left->relocation.target_address < right->relocation.target_address;
        }
        return left->relocation_index < right->relocation_index;
    });
    for (const auto* unresolved : ordered)
    {
        entries_.emplace(unresolved->relocation.target_address,
                         ImportBoundary{unresolved->relocation.target_address,
                                         unresolved->relocation_index,
                                         unresolved->relocation,
                                         unresolved->symbol});
    }
}

const ImportBoundary* ImportBoundaryIndex::find(GuestAddress relocation_target) const noexcept
{
    const auto found = entries_.find(relocation_target);
    return found == entries_.end() ? nullptr : &found->second;
}

ExecutionSession::ExecutionSession(
    memory::GuestMemory& memory, const analysis::FinalizedFunctionMap& function_map,
    const std::vector<loader::UnresolvedRelocation>& unresolved_relocations,
    ExecutionSessionOptions options, ExecutionLoadSummary load_summary)
    : memory_(&memory), function_map_(&function_map), imports_(unresolved_relocations),
      options_(std::move(options)), load_summary_(load_summary)
{
}

const analysis::FunctionRecord* ExecutionSession::function_record(GuestAddress entry) const noexcept
{
    return function_map_ == nullptr ? nullptr : function_map_->find(entry);
}

ExecutionStopReason ExecutionSession::classify_error(const Error& error) const noexcept
{
    if (is_memory_error(error.code)) return ExecutionStopReason::MemoryFault;
    switch (error.code)
    {
    case ErrorCode::ExecutionTrap: return ExecutionStopReason::GuestTrap;
    case ErrorCode::ExecutionLimitExceeded: return ExecutionStopReason::IrOperationLimitExceeded;
    case ErrorCode::FunctionBoundaryConflict:
        return ExecutionStopReason::FunctionOwnershipConflict;
    case ErrorCode::UnknownGuestFunction: return ExecutionStopReason::UnknownGuestFunction;
    case ErrorCode::UnresolvedIndirectFlow:
        return ExecutionStopReason::UnresolvedIndirectControlFlow;
    case ErrorCode::RuntimeBoundary: return ExecutionStopReason::RuntimeServiceBoundary;
    case ErrorCode::UnsupportedInstruction:
    case ErrorCode::UnsupportedOperandForm:
    case ErrorCode::Unsupported: return ExecutionStopReason::UnsupportedInstruction;
    default: return ExecutionStopReason::UnsupportedSemantic;
    }
}

bool ExecutionSession::record_event(ExecutionSessionResult& result, ExecutionEvent event)
{
    if (result.events.size() >= options_.budgets.max_events)
    {
        result.stop_reason = ExecutionStopReason::EventLimitExceeded;
        result.diagnostic = "execution event limit exhausted";
        running_ = false;
        return false;
    }
    event.sequence = result.events.size();
    result.events.push_back(std::move(event));
    return true;
}

Result<void> ExecutionSession::map_stack(ExecutionSessionResult& result)
{
    if (memory_ == nullptr || options_.synthetic_stack_size == 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "controlled execution requires a non-empty synthetic stack"));
    }
    GuestAddress highest_end = 0U;
    for (const auto& region : memory_->regions())
    {
        const auto end = checked_add_u64(region.base, region.size);
        if (!end)
        {
            return Result<void>::failure(make_error(
                ErrorCode::ArithmeticOverflow, "mapped guest region end overflows"));
        }
        highest_end = std::max(highest_end, end.value());
    }
    const auto gap_end = checked_add_u64(highest_end, options_.stack_guard_gap);
    if (!gap_end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "synthetic stack guard gap overflows"));
    }
    constexpr GuestAddress page_size = 0x1000U;
    const auto base = align_up(gap_end.value(), page_size);
    if (!base)
    {
        return Result<void>::failure(base.error());
    }
    const auto size = align_up(options_.synthetic_stack_size, page_size);
    if (!size)
    {
        return Result<void>::failure(size.error());
    }
    const auto end = checked_add_u64(base.value(), size.value());
    if (!end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "synthetic stack end overflows"));
    }
    const auto mapped = memory_->map(base.value(), size.value(),
                                     memory::GuestMemoryPermissions::Read |
                                         memory::GuestMemoryPermissions::Write,
                                     "synthetic.controlled.stack", memory::GuestRegionKind::Other);
    if (!mapped)
    {
        return mapped;
    }
    result.stack_base = base.value();
    result.stack_end = end.value();
    result.initial_sp = result.stack_end & ~GuestAddress{0xfU};
    if ((result.initial_sp & 0xfU) != 0U || result.initial_sp <= result.stack_base)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress, "synthetic stack cannot provide an aligned initial SP"));
    }
    if (!record_event(result, ExecutionEvent{0U, ExecutionEventKind::StackMapped, 0U, 0U,
                                               result.stack_base, true, 0U,
                                               runtime::ExecutionBoundaryKind::None, {}}))
    {
        return Result<void>::success();
    }
    return Result<void>::success();
}

Result<const ir::Function*> ExecutionSession::lift_for_execution(
    GuestAddress entry, ExecutionSessionResult& result)
{
    const auto found = lift_cache_.find(entry);
    if (found != lift_cache_.end())
    {
        if (found->second.function) return Result<const ir::Function*>::success(&found->second.function.value());
        return Result<const ir::Function*>::failure(found->second.error.value());
    }

    const auto* record = function_record(entry);
    if (record == nullptr)
    {
        const auto error = make_error(ErrorCode::UnknownGuestFunction,
                                      "guest address is not an exact finalized function entry");
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, error});
        return Result<const ir::Function*>::failure(error);
    }
    if (record->translation_status == analysis::TranslationStatus::Conflict)
    {
        std::ostringstream message;
        message << "function ownership conflict for " << hex_address(entry);
        for (const auto& conflict : function_map_->conflicts())
        {
            if (conflict.first_function == entry || conflict.second_function == entry)
            {
                message << "; conflicts with "
                        << hex_address(conflict.first_function == entry
                                           ? conflict.second_function
                                           : conflict.first_function);
                for (const auto& range : conflict.overlap_ranges)
                {
                    message << " overlap " << hex_address(range.base) << "+" << range.size;
                }
            }
        }
        const auto error = make_error(ErrorCode::FunctionBoundaryConflict, message.str());
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, error});
        return Result<const ir::Function*>::failure(error);
    }
    if (!record->cfg)
    {
        const auto error = make_error(
            ErrorCode::UnsupportedInstruction,
            "finalized function has no executable CFG: " + hex_address(entry));
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, error});
        return Result<const ir::Function*>::failure(error);
    }
    const auto lifted = lifter::lift_function(record->cfg.value());
    if (!lifted)
    {
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, lifted.error()});
        return Result<const ir::Function*>::failure(lifted.error());
    }
    auto inserted = lift_cache_.emplace(entry, LiftCacheEntry{lifted.value(), std::nullopt});
    (void)result;
    return Result<const ir::Function*>::success(&inserted.first->second.function.value());
}

Result<void> ExecutionSession::enter_function(GuestAddress entry,
                                               ExecutionSessionResult& result)
{
    if (result.executed_functions.size() >= options_.budgets.max_function_transitions)
    {
        return stop(result, ExecutionStopReason::FunctionTransitionLimitExceeded,
                    "function transition limit exhausted");
    }
    const auto eligible = lift_for_execution(entry, result);
    if (!eligible)
    {
        return stop(result, classify_error(eligible.error()), eligible.error().message, entry);
    }
    current_.function_entry = entry;
    current_.interpreter = interpreter::InterpreterFrame{};
    result.executed_functions.push_back(entry);
    result.maximum_call_depth = std::max(result.maximum_call_depth, current_.call_depth);
    return record_event(result, ExecutionEvent{0U, ExecutionEventKind::FunctionEnter, entry, 0U, 0U,
                                                false, current_.call_depth,
                                                runtime::ExecutionBoundaryKind::None, {}})
               ? Result<void>::success()
               : Result<void>::success();
}

Result<void> ExecutionSession::stop(ExecutionSessionResult& result, ExecutionStopReason reason,
                                    std::string diagnostic,
                                    std::optional<GuestAddress> target,
                                    const runtime::ExecutionResult* boundary)
{
    result.stop_reason = reason;
    result.diagnostic = std::move(diagnostic);
    result.target = target;
    result.target_register = boundary == nullptr ? "" : boundary->boundary.target_register;
    result.target_provenance = boundary == nullptr ? "" : boundary->boundary.target_provenance;
    result.final_cpu = cpu_;
    result.current_function = current_.function_entry;
    result.stop_pc = boundary == nullptr ? cpu_.pc : boundary->boundary.source_guest_pc;
    result.call_stack.clear();
    for (const auto& frame : suspended_frames_)
    {
        result.call_stack.push_back(CallStackFrame{frame.function_entry, frame.call_site_pc,
                                                   frame.expected_return_pc, frame.call_depth});
    }
    result.call_stack.push_back(CallStackFrame{current_.function_entry, current_.call_site_pc,
                                               current_.expected_return_pc, current_.call_depth});

    ExecutionEventKind event_kind = ExecutionEventKind::UnsupportedBoundary;
    if (reason == ExecutionStopReason::UnresolvedImport) event_kind = ExecutionEventKind::ImportBoundary;
    else if (reason == ExecutionStopReason::UnresolvedIndirectControlFlow ||
             reason == ExecutionStopReason::UnknownGuestFunction ||
             reason == ExecutionStopReason::InvalidIndirectTarget)
        event_kind = ExecutionEventKind::IndirectBoundary;
    else if (reason == ExecutionStopReason::MemoryFault) event_kind = ExecutionEventKind::MemoryFault;
    if (!record_event(result, ExecutionEvent{0U, event_kind, current_.function_entry, result.stop_pc,
                                               target.value_or(0U), target.has_value(),
                                               current_.call_depth,
                                               boundary == nullptr
                                                   ? runtime::ExecutionBoundaryKind::None
                                                   : boundary->boundary.kind,
                                               execution_stop_reason_name(reason)}))
    {
        return Result<void>::success();
    }
    (void)record_event(result, ExecutionEvent{0U, ExecutionEventKind::SessionStop,
                                               current_.function_entry, result.stop_pc,
                                               target.value_or(0U), target.has_value(),
                                               current_.call_depth,
                                               boundary == nullptr
                                                   ? runtime::ExecutionBoundaryKind::None
                                                   : boundary->boundary.kind,
                                               execution_stop_reason_name(reason)});
    running_ = false;
    return Result<void>::success();
}

Result<void> ExecutionSession::classify_target(const runtime::ExecutionResult& boundary,
                                               ExecutionSessionResult& result, bool call)
{
    const auto& payload = boundary.boundary;
    const auto target = payload.target_guest_address;
    if (payload.has_provenance_address)
    {
        if (const auto* import = imports_.find(payload.provenance_address))
        {
            result.import_boundary = *import;
            return stop(result, ExecutionStopReason::UnresolvedImport,
                        "indirect target originates at unresolved relocation " +
                            hex_address(payload.provenance_address), target, &boundary);
        }
    }
    if ((target & 0x3U) != 0U || target == 0U)
    {
        return stop(result, ExecutionStopReason::InvalidIndirectTarget,
                    "indirect target is zero or not AArch64 aligned", target, &boundary);
    }
    const auto* record = function_record(target);
    if (record != nullptr && record->translation_status == analysis::TranslationStatus::Conflict)
    {
        return stop(result, ExecutionStopReason::FunctionOwnershipConflict,
                    "target belongs to a precise function ownership conflict", target, &boundary);
    }
    const auto executable = memory_->is_executable(target, 4U);
    if (!executable || !executable.value())
    {
        return stop(result, ExecutionStopReason::InvalidIndirectTarget,
                    "indirect target is not mapped executable guest code", target, &boundary);
    }
    if (record == nullptr)
    {
        return stop(result, ExecutionStopReason::UnknownGuestFunction,
                    "aligned executable target is not an exact trusted function entry", target, &boundary);
    }
    const auto eligible = lift_for_execution(target, result);
    if (!eligible)
    {
        return stop(result, classify_error(eligible.error()), eligible.error().message, target, &boundary);
    }
    return call ? dispatch_call(boundary, result) : dispatch_transfer(boundary, result);
}

Result<void> ExecutionSession::dispatch_call(const runtime::ExecutionResult& boundary,
                                              ExecutionSessionResult& result)
{
    if (boundary.boundary.continuation_block == ir::invalid_block ||
        boundary.boundary.continuation_guest_pc == 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidControlFlow, "guest call boundary has no explicit continuation"));
    }
    if (suspended_frames_.size() + 1U > options_.budgets.max_call_depth)
    {
        return stop(result, ExecutionStopReason::CallDepthExceeded,
                    "guest call depth limit exhausted", boundary.boundary.target_guest_address,
                    &boundary);
    }
    const auto event_kind = boundary.boundary.kind == runtime::ExecutionBoundaryKind::IndirectCall
                                ? ExecutionEventKind::IndirectCall
                                : ExecutionEventKind::DirectCall;
    if (!record_event(result, ExecutionEvent{0U, event_kind, current_.function_entry,
                                               boundary.boundary.source_guest_pc,
                                               boundary.boundary.target_guest_address, true,
                                               current_.call_depth, boundary.boundary.kind, {}}))
    {
        return Result<void>::success();
    }
    current_.interpreter.current_block = boundary.boundary.continuation_block;
    suspended_frames_.push_back(std::move(current_));
    current_ = SessionFrame{};
    current_.call_site_pc = boundary.boundary.source_guest_pc;
    current_.expected_return_pc = boundary.boundary.continuation_guest_pc;
    current_.call_depth = suspended_frames_.size();
    current_.function_entry = boundary.boundary.target_guest_address;
    return enter_function(current_.function_entry, result);
}

Result<void> ExecutionSession::dispatch_transfer(const runtime::ExecutionResult& boundary,
                                                 ExecutionSessionResult& result)
{
    if (!record_event(result, ExecutionEvent{0U, ExecutionEventKind::FunctionTransfer,
                                               current_.function_entry,
                                               boundary.boundary.source_guest_pc,
                                               boundary.boundary.target_guest_address, true,
                                               current_.call_depth, boundary.boundary.kind, {}}))
    {
        return Result<void>::success();
    }
    if (result.executed_functions.size() >= options_.budgets.max_function_transitions)
    {
        return stop(result, ExecutionStopReason::FunctionTransitionLimitExceeded,
                    "function transition limit exhausted", boundary.boundary.target_guest_address,
                    &boundary);
    }
    current_.function_entry = boundary.boundary.target_guest_address;
    current_.interpreter = interpreter::InterpreterFrame{};
    ++result.function_transfers;
    result.executed_functions.push_back(current_.function_entry);
    result.maximum_call_depth = std::max(result.maximum_call_depth, current_.call_depth);
    return record_event(result, ExecutionEvent{0U, ExecutionEventKind::FunctionEnter,
                                                current_.function_entry, 0U, 0U, false,
                                                current_.call_depth,
                                                runtime::ExecutionBoundaryKind::None, {}})
               ? Result<void>::success()
               : Result<void>::success();
}

Result<ExecutionSessionResult> ExecutionSession::run(const EntrySelection& entry)
{
    if (running_)
    {
        return Result<ExecutionSessionResult>::failure(
            make_error(ErrorCode::InvalidArgument, "execution session is already running"));
    }
    if (memory_ == nullptr || function_map_ == nullptr || options_.budgets.max_ir_operations == 0U ||
        options_.budgets.max_function_transitions == 0U || options_.budgets.max_call_depth == 0U ||
        options_.budgets.max_events == 0U || options_.budgets.max_guest_blocks == 0U)
    {
        return Result<ExecutionSessionResult>::failure(
            make_error(ErrorCode::InvalidArgument, "execution budgets and session inputs must be non-zero"));
    }
    if (entry.address == 0U || (entry.address & 0x3U) != 0U)
    {
        return Result<ExecutionSessionResult>::failure(
            make_error(ErrorCode::InvalidGuestAddress, "selected execution entry is invalid"));
    }

    running_ = true;
    suspended_frames_.clear();
    current_ = SessionFrame{};
    lift_cache_.clear();
    ExecutionSessionResult result;
    result.identity = function_map_->identity();
    result.entry = entry;
    result.options = options_;
    result.relocations = load_summary_;
    result.analyzed_functions = function_map_->functions().size();
    result.precise_conflicts = function_map_->conflicts().size();
    std::set<GuestAddress> conflicting_functions;
    for (const auto& conflict : function_map_->conflicts())
    {
        conflicting_functions.insert(conflict.first_function);
        conflicting_functions.insert(conflict.second_function);
    }
    result.conflicting_functions = conflicting_functions.size();
    for (const auto& function : function_map_->functions())
    {
        const auto owned = analysis::precise_owned_byte_count(function.owned_code_ranges);
        if (!owned || owned.value() > std::numeric_limits<memory::GuestSize>::max() -
                              result.precise_owned_bytes)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(make_error(
                ErrorCode::AnalysisBudgetExceeded, "precise ownership byte count overflows"));
        }
        result.precise_owned_bytes += owned.value();
    }
    if (!record_event(result, ExecutionEvent{0U, ExecutionEventKind::SessionStart, 0U, 0U, 0U,
                                               false, 0U,
                                               runtime::ExecutionBoundaryKind::None, {}}) ||
        !record_event(result, ExecutionEvent{0U, ExecutionEventKind::EntrySelected, 0U,
                                               entry.address, entry.address, true, 0U,
                                               runtime::ExecutionBoundaryKind::None,
                                               entry_selection_kind_name(entry.kind)}))
    {
        result.final_cpu = cpu_;
        return Result<ExecutionSessionResult>::success(std::move(result));
    }
    const auto stack = map_stack(result);
    if (!stack)
    {
        running_ = false;
        return Result<ExecutionSessionResult>::failure(stack.error());
    }
    if (!running_)
    {
        result.final_cpu = cpu_;
        return Result<ExecutionSessionResult>::success(std::move(result));
    }
    const auto sentinel = checked_add_u64(result.stack_end, 0x1000U);
    if (!sentinel)
    {
        running_ = false;
        return Result<ExecutionSessionResult>::failure(sentinel.error());
    }
    result.synthetic_lr_sentinel = sentinel.value();
    cpu_ = runtime::CpuState{};
    cpu_.sp = result.initial_sp;
    cpu_.pc = entry.address;
    cpu_.x[30U] = result.synthetic_lr_sentinel;
    runtime_ = runtime::RuntimeContext{memory_};
    runtime_.cpu = &cpu_;
    current_.function_entry = entry.address;
    current_.expected_return_pc = result.synthetic_lr_sentinel;
    const auto entered = enter_function(entry.address, result);
    if (!entered)
    {
        running_ = false;
        return Result<ExecutionSessionResult>::failure(entered.error());
    }
    while (running_)
    {
        const auto function_result = lift_for_execution(current_.function_entry, result);
        if (!function_result)
        {
            (void)stop(result, classify_error(function_result.error()), function_result.error().message,
                       current_.function_entry);
            break;
        }
        const auto* function = function_result.value();
        if (result.ir_operations >= options_.budgets.max_ir_operations)
        {
            (void)stop(result, ExecutionStopReason::IrOperationLimitExceeded,
                       "session IR operation limit exhausted");
            break;
        }
        runtime::ExecutionOptions interpreter_options;
        interpreter_options.max_ir_operations =
            options_.budgets.max_ir_operations - result.ir_operations;
        const auto step = interpreter::execute_until_boundary(
            *function, cpu_, runtime_, current_.interpreter, interpreter_options);
        if (!step)
        {
            (void)stop(result, classify_error(step.error()), step.error().message);
            break;
        }
        result.ir_operations += step.value().executed_operations;
        result.guest_blocks += step.value().executed_blocks;
        if (result.guest_blocks > options_.budgets.max_guest_blocks)
        {
            (void)stop(result, ExecutionStopReason::GuestBlockLimitExceeded,
                       "guest block execution limit exhausted");
            break;
        }
        const auto& boundary = step.value().boundary;
        if (step.value().status == runtime::ExecutionStatus::LimitExceeded)
        {
            (void)stop(result, ExecutionStopReason::IrOperationLimitExceeded,
                       "session IR operation limit exhausted", std::nullopt, &step.value());
            break;
        }
        switch (boundary.kind)
        {
        case runtime::ExecutionBoundaryKind::Return:
            ++result.returns;
            if (!record_event(result, ExecutionEvent{0U, ExecutionEventKind::Return,
                                                       current_.function_entry,
                                                       boundary.source_guest_pc,
                                                       boundary.target_guest_address,
                                                       boundary.target_known, current_.call_depth,
                                                       boundary.kind, {}}))
                break;
            if (suspended_frames_.empty())
            {
                (void)stop(result, ExecutionStopReason::EntryReturned,
                           "controlled entry component returned", std::nullopt, &step.value());
                break;
            }
            if (boundary.target_guest_address != current_.expected_return_pc)
            {
                (void)stop(result, ExecutionStopReason::ReturnTargetMismatch,
                           "guest return target does not match the caller continuation",
                           boundary.target_guest_address, &step.value());
                break;
            }
            current_ = std::move(suspended_frames_.back());
            suspended_frames_.pop_back();
            cpu_.pc = current_.expected_return_pc;
            if (!record_event(result, ExecutionEvent{0U, ExecutionEventKind::FunctionResume,
                                                       current_.function_entry,
                                                       current_.expected_return_pc, 0U, false,
                                                       current_.call_depth,
                                                       runtime::ExecutionBoundaryKind::Return, {}}))
                break;
            break;
        case runtime::ExecutionBoundaryKind::DirectCall:
        case runtime::ExecutionBoundaryKind::IndirectCall:
        {
            if (boundary.kind == runtime::ExecutionBoundaryKind::DirectCall) ++result.direct_calls;
            else ++result.indirect_calls;
            const auto dispatched = classify_target(step.value(), result, true);
            if (!dispatched)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(dispatched.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::FunctionTransfer:
        case runtime::ExecutionBoundaryKind::IndirectBranch:
        {
            const auto dispatched = classify_target(step.value(), result, false);
            if (!dispatched)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(dispatched.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::Trap:
            (void)stop(result, ExecutionStopReason::GuestTrap,
                       boundary.target_provenance, std::nullopt, &step.value());
            break;
        case runtime::ExecutionBoundaryKind::BudgetExhaustion:
            (void)stop(result, ExecutionStopReason::IrOperationLimitExceeded,
                       "session IR operation limit exhausted", std::nullopt, &step.value());
            break;
        case runtime::ExecutionBoundaryKind::None:
            (void)stop(result, ExecutionStopReason::UnsupportedSemantic,
                       "interpreter returned without a semantic boundary", std::nullopt,
                       &step.value());
            break;
        }
    }
    result.final_cpu = cpu_;
    if (running_)
    {
        running_ = false;
    }
    return Result<ExecutionSessionResult>::success(std::move(result));
}

std::string render_execution_report_json(const ExecutionSessionResult& result)
{
    json executable_ranges = json::array();
    for (const auto& range : result.identity.executable_ranges)
    {
        executable_ranges.push_back(range_json(range));
    }
    json events = json::array();
    for (const auto& event : result.events)
    {
        json item{{"sequence", event.sequence}, {"kind", execution_event_kind_name(event.kind)},
                  {"function_entry", hex_address(event.function_entry)},
                  {"guest_pc", hex_address(event.guest_pc)}, {"call_depth", event.call_depth},
                  {"boundary", runtime::execution_boundary_kind_name(event.boundary)},
                  {"code", event.code}};
        if (event.has_target) item["target"] = hex_address(event.target);
        events.push_back(std::move(item));
    }
    json stack = json::array();
    for (const auto& frame : result.call_stack)
    {
        stack.push_back(json{{"function_entry", hex_address(frame.function_entry)},
                             {"call_site_pc", hex_address(frame.call_site_pc)},
                             {"expected_return_pc", hex_address(frame.expected_return_pc)},
                             {"call_depth", frame.call_depth}});
    }
    json executed = json::array();
    for (const auto entry : result.executed_functions) executed.push_back(hex_address(entry));
    json value{{"schema_version", ExecutionSessionResult::schema_version},
                {"module", json{{"logical_name", result.identity.module},
                                  {"sha256", result.identity.input_sha256},
                                  {"build_id", result.identity.build_id},
                                  {"analysis_base", hex_address(result.identity.guest_base)},
                                  {"guest_base_verified", result.identity.guest_base_verified},
                                  {"executable_ranges", std::move(executable_ranges)}}},
                {"relocations", json{{"parsed", result.relocations.relocations_parsed},
                                      {"applied", result.relocations.relocations_applied},
                                      {"unresolved", result.relocations.unresolved_relocations}}},
                {"analysis", json{{"functions", result.analyzed_functions},
                                   {"conflicting_functions", result.conflicting_functions},
                                   {"conflict_records", result.precise_conflicts},
                                   {"precise_conflicts", result.precise_conflicts},
                                   {"precise_owned_bytes", result.precise_owned_bytes}}},
                {"entry", json{{"kind", entry_selection_kind_name(result.entry.kind)},
                                {"address", hex_address(result.entry.address)},
                                {"source", result.entry.source},
                                {"confidence", analysis::function_confidence_name(result.entry.confidence)},
                                {"verified_process_entry", result.entry.verified_process_entry}}},
                {"launch_context", json{{"synthetic_stack", true},
                                         {"synthetic_stack_size", result.options.synthetic_stack_size},
                                         {"stack_base", hex_address(result.stack_base)},
                                         {"stack_end", hex_address(result.stack_end)},
                                         {"initial_sp", hex_address(result.initial_sp)},
                                         {"synthetic_lr_sentinel", hex_address(result.synthetic_lr_sentinel)},
                                         {"tls", "synthetic_zero"},
                                         {"state_description", "deterministic controlled-run state"}}},
                {"execution", json{{"stop_reason", execution_stop_reason_name(result.stop_reason)},
                                    {"stop_pc", hex_address(result.stop_pc)},
                                    {"current_function", hex_address(result.current_function)},
                                    {"target", result.target ? json(hex_address(result.target.value())) : json(nullptr)},
                                    {"target_register", result.target_register},
                                    {"target_provenance", result.target_provenance},
                                    {"executed_functions", std::move(executed)},
                                    {"direct_calls", result.direct_calls},
                                    {"indirect_calls", result.indirect_calls},
                                    {"function_transfers", result.function_transfers},
                                    {"returns", result.returns},
                                    {"ir_operations", result.ir_operations},
                                    {"guest_blocks", result.guest_blocks},
                                    {"maximum_call_depth", result.maximum_call_depth},
                                    {"diagnostic", result.diagnostic}}},
                {"budgets", json{{"max_ir_operations", result.options.budgets.max_ir_operations},
                                  {"max_function_transitions", result.options.budgets.max_function_transitions},
                                  {"max_call_depth", result.options.budgets.max_call_depth},
                                  {"max_events", result.options.budgets.max_events},
                                  {"max_guest_blocks", result.options.budgets.max_guest_blocks}}},
                {"call_stack_snapshot", std::move(stack)},
                {"registers", cpu_json(result.final_cpu)},
                {"events", std::move(events)}};
    if (result.import_boundary)
    {
        const auto& import = result.import_boundary.value();
        value["execution"]["import"] = json{{"relocation_target", hex_address(import.relocation_target)},
                                               {"relocation_index", import.relocation_index},
                                               {"symbol_index", import.symbol.symbol_index},
                                               {"symbol", import.symbol.name},
                                               {"binding", format::symbol_binding_name(import.symbol.binding)},
                                               {"symbol_type", format::symbol_type_name(import.symbol.type)},
                                               {"visibility", format::symbol_visibility_name(import.symbol.visibility)},
                                               {"relocation_type", import.relocation.raw_type},
                                               {"relocation_type_name", format::aarch64_relocation_type_name(import.relocation.type)},
                                               {"relocation_source", format::relocation_source_name(import.relocation.source)},
                                               {"addend", import.relocation.addend}};
    }
    return value.dump(2) + "\n";
}

} // namespace switchrecomp::execution
