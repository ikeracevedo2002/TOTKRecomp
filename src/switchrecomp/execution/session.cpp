#include "switchrecomp/execution/session.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
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

[[nodiscard]] json abi_snapshot_json(const runtime::GuestAbiSnapshot& snapshot)
{
    json arguments = json::array();
    for (const auto value : snapshot.x0_x7)
    {
        arguments.push_back(hex_address(value));
    }
    return json{{"x0_x7", std::move(arguments)}, {"sp", hex_address(snapshot.sp)},
                {"x29", hex_address(snapshot.x29)}, {"x30", hex_address(snapshot.x30)},
                {"pc", hex_address(snapshot.pc)}};
}

[[nodiscard]] json dynamic_pointer_json(const std::optional<format::DynamicPointer>& pointer)
{
    if (!pointer)
    {
        return nullptr;
    }
    return json{{"module_offset", hex_address(pointer->module_offset)},
                {"address", hex_address(pointer->address)}};
}

[[nodiscard]] json mod0_range_json(const std::optional<format::Mod0Range>& range)
{
    if (!range)
    {
        return nullptr;
    }
    return json{{"start_offset", range->start_offset}, {"end_offset", range->end_offset},
                {"start_address", hex_address(range->start_address)},
                {"end_address", hex_address(range->end_address)}};
}

[[nodiscard]] json module_metadata_json(
    const std::optional<format::ModuleMetadata>& metadata)
{
    if (!metadata)
    {
        return json{{"present", false}};
    }

    json value{{"present", true}, {"module_base", hex_address(metadata->module_base)}};
    if (metadata->mod0)
    {
        const auto& mod0 = metadata->mod0.value();
        value["mod0"] = json{
            {"address", hex_address(mod0.address)},
            {"dynamic_offset", mod0.dynamic_offset},
            {"bss_start_offset", mod0.bss_start_offset},
            {"bss_end_offset", mod0.bss_end_offset},
            {"exception_info_start_offset", mod0.exception_info_start_offset},
            {"exception_info_end_offset", mod0.exception_info_end_offset},
            {"module_object_offset", mod0.module_object_offset},
            {"dynamic_address", hex_address(mod0.dynamic_address)},
            {"bss_start_address", hex_address(mod0.bss_start_address)},
            {"bss_end_address", hex_address(mod0.bss_end_address)},
            {"exception_info_start_address", hex_address(mod0.exception_info_start_address)},
            {"exception_info_end_address", hex_address(mod0.exception_info_end_address)},
            {"module_object_address", hex_address(mod0.module_object_address)},
            {"relro", mod0_range_json(mod0.relro)},
            {"nx_debug_link", mod0_range_json(mod0.nx_debug_link)},
            {"gnu_build_id_note", mod0_range_json(mod0.gnu_build_id_note)}};
    }
    else
    {
        value["mod0"] = nullptr;
    }

    if (!metadata->dynamic)
    {
        value["dynamic"] = nullptr;
        return value;
    }

    const auto& dynamic = metadata->dynamic.value();
    json entries = json::array();
    for (const auto& entry : dynamic.entries)
    {
        entries.push_back(json{{"tag", entry.tag},
                               {"tag_name", format::dynamic_tag_name(
                                                   static_cast<format::DynamicTag>(entry.tag))},
                               {"value", hex_address(entry.value)}});
    }
    value["dynamic"] = json{
        {"module_base", hex_address(dynamic.module_base)},
        {"address", hex_address(dynamic.address)},
        {"entry_count", dynamic.entry_count},
        {"entries", std::move(entries)},
        {"pltgot", dynamic_pointer_json(dynamic.pltgot)},
        {"hash", dynamic_pointer_json(dynamic.hash)},
        {"gnu_hash", dynamic_pointer_json(dynamic.gnu_hash)},
        {"strtab", dynamic_pointer_json(dynamic.strtab)},
        {"symtab", dynamic_pointer_json(dynamic.symtab)},
        {"rela", dynamic_pointer_json(dynamic.rela)},
        {"init", dynamic_pointer_json(dynamic.init)},
        {"fini", dynamic_pointer_json(dynamic.fini)},
        {"rel", dynamic_pointer_json(dynamic.rel)},
        {"debug", dynamic_pointer_json(dynamic.debug)},
        {"jmprel", dynamic_pointer_json(dynamic.jmprel)},
        {"init_array", dynamic_pointer_json(dynamic.init_array)},
        {"fini_array", dynamic_pointer_json(dynamic.fini_array)},
        {"preinit_array", dynamic_pointer_json(dynamic.preinit_array)},
        {"pltrelsz", dynamic.pltrelsz ? json(dynamic.pltrelsz.value()) : json(nullptr)},
        {"strsz", dynamic.strsz ? json(dynamic.strsz.value()) : json(nullptr)},
        {"syment", dynamic.syment ? json(dynamic.syment.value()) : json(nullptr)},
        {"relasz", dynamic.relasz ? json(dynamic.relasz.value()) : json(nullptr)},
        {"relaent", dynamic.relaent ? json(dynamic.relaent.value()) : json(nullptr)},
        {"relsz", dynamic.relsz ? json(dynamic.relsz.value()) : json(nullptr)},
        {"relent", dynamic.relent ? json(dynamic.relent.value()) : json(nullptr)},
        {"plt_rel_type", dynamic.plt_rel_type ? json(dynamic.plt_rel_type.value()) : json(nullptr)},
        {"init_array_size", dynamic.init_array_size ? json(dynamic.init_array_size.value()) : json(nullptr)},
        {"fini_array_size", dynamic.fini_array_size ? json(dynamic.fini_array_size.value()) : json(nullptr)},
        {"preinit_array_size", dynamic.preinit_array_size ? json(dynamic.preinit_array_size.value()) : json(nullptr)},
        {"relacount", dynamic.relacount ? json(dynamic.relacount.value()) : json(nullptr)},
        {"relcount", dynamic.relcount ? json(dynamic.relcount.value()) : json(nullptr)},
        {"rela_count", dynamic.rela_count ? json(dynamic.rela_count.value()) : json(nullptr)},
        {"jmprel_count", dynamic.jmprel_count ? json(dynamic.jmprel_count.value()) : json(nullptr)},
        {"rel_count", dynamic.rel_count ? json(dynamic.rel_count.value()) : json(nullptr)}};
    return value;
}

[[nodiscard]] json runtime_import_json(const RuntimeImportObservation& observation)
{
    const auto& descriptor = observation.descriptor;
    json abi_arguments = json::array();
    for (const auto kind : descriptor.signature.arguments)
    {
        abi_arguments.push_back(runtime::abi_value_kind_name(kind));
    }
    if (descriptor.signature.arguments.empty() && descriptor.signature.argument_count)
    {
        abi_arguments = json::array();
    }

    json observed_arguments = json::array();
    for (const auto& argument : observation.arguments)
    {
        observed_arguments.push_back(json{
            {"index", argument.index},
            {"location", argument.location},
            {"readable", argument.readable},
            {"value", hex_address(argument.value)},
            {"diagnostic", argument.diagnostic},
            {"mapping", argument.mapping},
            {"permissions", argument.permissions},
            {"region_kind", argument.region_kind},
            {"aligned_8", argument.aligned_8},
            {"aligned_16", argument.aligned_16},
            {"dynamic_tags", argument.dynamic_tags},
            {"dynamic_symbols", argument.dynamic_symbols},
            {"relocation_ranges", argument.relocation_ranges},
            {"range_validity", argument.range_validity}});
    }

    json trampoline_evidence = observation.trampoline_evidence;
    const auto& import = observation.provenance;
    return json{
        {"symbol", descriptor.symbol_name},
        {"support", runtime::runtime_support_status_name(descriptor.support)},
        {"subsystem", runtime::runtime_subsystem_name(descriptor.subsystem)},
        {"signature", json{
                          {"argument_count", descriptor.signature.argument_count
                                                   ? json(descriptor.signature.argument_count.value())
                                                   : json(nullptr)},
                          {"observed_argument_count", descriptor.signature.observed_argument_count},
                          {"arguments", std::move(abi_arguments)},
                          {"return", runtime::abi_return_kind_name(
                                         descriptor.signature.return_kind)}}},
        {"evidence", json{{"sources", descriptor.evidence.sources},
                           {"confidence", descriptor.evidence.confidence},
                           {"rationale", descriptor.evidence.rationale}}},
        {"invocation", runtime::external_invocation_kind_name(observation.invocation)},
        {"source_guest_pc", hex_address(observation.source_guest_pc)},
        {"dynamic_pltgot", observation.dynamic_pltgot
                                ? json(hex_address(observation.dynamic_pltgot.value()))
                                : json(nullptr)},
        {"dynamic_pltgot_slot_delta", observation.dynamic_pltgot_slot_delta
                                          ? json(hex_address(observation.dynamic_pltgot_slot_delta.value()))
                                          : json(nullptr)},
        {"guest_provider_resolution", observation.guest_provider_resolution},
        {"runtime_fallback_eligible", observation.runtime_fallback_eligible},
        {"provenance", json{
                           {"consumer_module", import.consumer_module},
                           {"relocation_target", hex_address(import.relocation_target)},
                           {"relocation_index", import.relocation_index},
                           {"relocation_offset", hex_address(import.relocation.offset)},
                           {"relocation_type", import.relocation.raw_type},
                           {"relocation_type_name", format::aarch64_relocation_type_name(
                                                         import.relocation.type)},
                           {"relocation_source", format::relocation_source_name(
                                                       import.relocation.source)},
                           {"relocation_addend", import.relocation.addend},
                           {"symbol_index", import.symbol.symbol_index},
                           {"symbol", import.symbol.name},
                           {"binding", format::symbol_binding_name(import.symbol.binding)},
                           {"type", format::symbol_type_name(import.symbol.type)},
                           {"visibility", format::symbol_visibility_name(import.symbol.visibility)},
                           {"section_index", import.symbol.section_index}}},
        {"abi", json{{"snapshot", abi_snapshot_json(observation.abi)},
                      {"validation", observation.abi_validation},
                      {"diagnostic", observation.abi_diagnostic},
                      {"arguments", std::move(observed_arguments)}}},
        {"trampoline", json{{"classification", observation.trampoline_classification},
                             {"evidence", std::move(trampoline_evidence)}}},
        {"outcome", json{{"status", observation.outcome},
                          {"diagnostic", observation.outcome_diagnostic}}}};
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

[[nodiscard]] std::string permissions_text(memory::GuestMemoryPermissions permissions)
{
    std::string result;
    if (memory::has_permission(permissions, memory::GuestMemoryPermissions::Read)) result += 'r';
    else result += '-';
    if (memory::has_permission(permissions, memory::GuestMemoryPermissions::Write)) result += 'w';
    else result += '-';
    if (memory::has_permission(permissions, memory::GuestMemoryPermissions::Execute)) result += 'x';
    else result += '-';
    return result;
}

[[nodiscard]] bool range_contains(GuestAddress begin, GuestAddress end, GuestAddress value) noexcept
{
    return begin <= value && value < end;
}

void add_pointer_relation(const std::optional<format::DynamicPointer>& pointer,
                          std::string_view tag, GuestAddress value,
                          std::vector<std::string>& relations)
{
    if (pointer && pointer->address == value)
    {
        relations.emplace_back(tag);
    }
}

void add_range_relation(const std::optional<format::DynamicPointer>& pointer,
                        const std::optional<std::uint64_t>& size, std::string_view tag,
                        GuestAddress value, std::vector<std::string>& relations)
{
    if (!pointer || !size || size.value() == 0U)
    {
        return;
    }
    const auto end = checked_add_u64(pointer->address, size.value());
    if (end && range_contains(pointer->address, end.value(), value))
    {
        relations.emplace_back(tag);
    }
}

void add_symbol_relations(const format::DynamicSymbolTable* symbols, GuestAddress module_base,
                          GuestAddress value, std::vector<std::string>& relations)
{
    if (symbols == nullptr)
    {
        return;
    }
    constexpr std::uint16_t shn_abs = 0xfff1U;
    for (const auto& symbol : symbols->symbols)
    {
        if (!symbol.is_defined() || symbol.name.empty())
        {
            continue;
        }
        const auto address = symbol.section_index == shn_abs
                                 ? Result<GuestAddress>::success(symbol.value)
                                 : checked_add_u64(module_base, symbol.value);
        if (address && address.value() == value)
        {
            relations.push_back(std::to_string(symbol.index) + ":" + symbol.name);
        }
    }
}

[[nodiscard]] RuntimeAbiArgumentObservation observe_argument(
    const runtime::AArch64GuestCall& abi, std::size_t index, const memory::GuestMemory& memory,
    const format::DynamicInfo* dynamic, const format::DynamicSymbolTable* symbols)
{
    RuntimeAbiArgumentObservation observation;
    observation.index = index;
    if (index < 8U)
    {
        observation.location = "x" + std::to_string(index);
    }
    else
    {
        const auto offset = checked_mul_u64(static_cast<std::uint64_t>(index - 8U), 8U);
        observation.location = offset ? "[sp+0x" + [&]() {
            std::ostringstream text;
            text << std::hex << offset.value();
            return text.str();
        }() + "]" : "[sp+overflow]";
    }

    const auto value = abi.integer_argument(index);
    if (!value)
    {
        observation.diagnostic = std::string(error_code_name(value.error().code)) + ": " +
                                 value.error().message;
        observation.mapping = "invalid";
        observation.permissions = "unavailable";
        observation.region_kind = "unknown";
        observation.range_validity = "invalid_argument_slot";
        return observation;
    }
    observation.readable = true;
    observation.value = value.value();
    observation.aligned_8 = (value.value() & 0x7U) == 0U;
    observation.aligned_16 = (value.value() & 0xfU) == 0U;
    const auto region = memory.region_at(value.value());
    if (!region)
    {
        observation.mapping = value.value() == 0U ? "null" : "unmapped";
        observation.permissions = "unavailable";
        observation.region_kind = "unknown";
    }
    else
    {
        observation.mapping = region.value().name;
        observation.permissions = permissions_text(region.value().permissions);
        observation.region_kind = std::string(memory::guest_region_kind_name(region.value().kind));
    }
    if (dynamic != nullptr)
    {
        add_pointer_relation(dynamic->pltgot, "DT_PLTGOT", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->strtab, "DT_STRTAB", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->symtab, "DT_SYMTAB", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->rela, "DT_RELA", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->jmprel, "DT_JMPREL", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->init, "DT_INIT", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->fini, "DT_FINI", value.value(), observation.dynamic_tags);
        add_range_relation(dynamic->rela, dynamic->relasz, "DT_RELA", value.value(),
                           observation.relocation_ranges);
        add_range_relation(dynamic->jmprel, dynamic->pltrelsz, "DT_JMPREL", value.value(),
                           observation.relocation_ranges);
    }
    add_symbol_relations(symbols, dynamic == nullptr ? 0U : dynamic->module_base,
                         value.value(), observation.dynamic_symbols);
    observation.range_validity = "point_checked;extent_not_established";
    return observation;
}

void classify_import_trampoline(const analysis::FunctionRecord* record,
                                const runtime::ExecutionResult& boundary,
                                const ImportBoundary& import,
                                const format::DynamicInfo* dynamic,
                                RuntimeImportObservation& observation)
{
    observation.trampoline_classification = "not_classified";
    if (record == nullptr || !record->cfg)
    {
        observation.trampoline_evidence.push_back("no finalized CFG for current guest function");
        return;
    }
    const auto owned = analysis::function_owns_address(*record, boundary.boundary.source_guest_pc);
    const auto single_block = record->cfg->blocks.size() == 1U;
    bool final_br = false;
    bool source_matches = false;
    if (single_block)
    {
        const auto& block = record->cfg->blocks.begin()->second;
        if (!block.instructions.empty())
        {
            const auto& instruction = block.instructions.back();
            final_br = instruction.id == aarch64::InstructionId::Br;
            source_matches = instruction.address == boundary.boundary.source_guest_pc;
        }
    }
    const bool relocation_is_plt = import.relocation.source == format::RelocationSource::JmpRel;
    const bool relocation_is_jump_slot =
        import.relocation.type == format::AArch64RelocationType::JumpSlot;
    const bool provenance_matches = boundary.boundary.has_provenance_address &&
                                    boundary.boundary.provenance_address == import.relocation_target;
    observation.trampoline_evidence.push_back(
        owned ? "exact function ownership contains the indirect-branch PC"
              : "exact function ownership does not contain the indirect-branch PC");
    observation.trampoline_evidence.push_back(
        single_block ? "finalized CFG contains one basic block" : "finalized CFG is not one block");
    observation.trampoline_evidence.push_back(
        final_br ? "last decoded instruction is BR" : "last decoded instruction is not BR");
    observation.trampoline_evidence.push_back(
        source_matches ? "BR PC matches the execution boundary source PC"
                       : "BR PC does not match the execution boundary source PC");
    observation.trampoline_evidence.push_back(
        provenance_matches ? "BR target provenance is the unresolved relocation slot"
                            : "BR target provenance does not match the relocation slot");
    observation.trampoline_evidence.push_back(
        relocation_is_plt ? "relocation source is JMPREL" : "relocation source is not JMPREL");
    observation.trampoline_evidence.push_back(
        relocation_is_jump_slot ? "relocation type is R_AARCH64_JUMP_SLOT"
                                : "relocation type is not R_AARCH64_JUMP_SLOT");
    if (dynamic != nullptr && dynamic->jmprel && dynamic->jmprel_count)
    {
        observation.trampoline_evidence.push_back("DT_JMPREL metadata and entry count are parsed");
    }
    else
    {
        observation.trampoline_evidence.push_back("DT_JMPREL metadata is unavailable");
    }
    if (dynamic != nullptr && dynamic->pltgot)
    {
        observation.trampoline_evidence.push_back(
            "DT_PLTGOT metadata is parsed; relocation slot is retained for explicit GOT comparison");
    }

    if (owned && single_block && final_br && source_matches && provenance_matches &&
        relocation_is_plt && relocation_is_jump_slot)
    {
        observation.trampoline_classification = "import_trampoline";
    }
    else
    {
        observation.trampoline_classification = "indirect_import_candidate";
    }
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
    case ExecutionStopReason::RuntimeImportUnimplemented: return "runtime_import_unimplemented";
    case ExecutionStopReason::RuntimeImportAbiViolation: return "runtime_import_abi_violation";
    case ExecutionStopReason::RuntimeImportMemoryFault: return "runtime_import_memory_fault";
    case ExecutionStopReason::RuntimeImportInvariantViolation:
        return "runtime_import_invariant_violation";
    case ExecutionStopReason::GuestTrap: return "guest_trap";
    case ExecutionStopReason::ReturnTargetMismatch: return "return_target_mismatch";
    case ExecutionStopReason::CallDepthExceeded: return "call_depth_exceeded";
    case ExecutionStopReason::FunctionTransitionLimitExceeded:
        return "function_transition_limit_exceeded";
    case ExecutionStopReason::IrOperationLimitExceeded: return "ir_operation_limit_exceeded";
    case ExecutionStopReason::EventLimitExceeded: return "event_limit_exceeded";
    case ExecutionStopReason::GuestBlockLimitExceeded: return "guest_block_limit_exceeded";
    case ExecutionStopReason::InvalidCrossModuleTarget: return "invalid_cross_module_target";
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
    case ExecutionEventKind::RuntimeImportResolved: return "runtime_import_resolved";
    case ExecutionEventKind::RuntimeImportEnter: return "runtime_import_enter";
    case ExecutionEventKind::RuntimeImportArgumentSummary: return "runtime_import_argument_summary";
    case ExecutionEventKind::RuntimeStateRegistration: return "runtime_state_registration";
    case ExecutionEventKind::RuntimeImportReturn: return "runtime_import_return";
    case ExecutionEventKind::RuntimeImportBoundary: return "runtime_import_boundary";
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
                                         unresolved->symbol,
                                         {}});
    }
}

ImportBoundaryIndex::ImportBoundaryIndex(const analysis::ProcessImage& process_image)
{
    struct Item
    {
        const loader::UnresolvedRelocation* relocation = nullptr;
        std::string module;
    };
    std::vector<Item> ordered;
    for (const auto& module : process_image.modules())
    {
        for (const auto& relocation : module.unresolved_relocations)
        {
            ordered.push_back(Item{&relocation, module.identity.module});
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.relocation->relocation.target_address !=
            right.relocation->relocation.target_address)
        {
            return left.relocation->relocation.target_address <
                   right.relocation->relocation.target_address;
        }
        if (left.module != right.module) return left.module < right.module;
        return left.relocation->relocation_index < right.relocation->relocation_index;
    });
    for (const auto& item : ordered)
    {
        const auto& unresolved = *item.relocation;
        entries_.emplace(unresolved.relocation.target_address,
                         ImportBoundary{unresolved.relocation.target_address,
                                         unresolved.relocation_index,
                                         unresolved.relocation,
                                         unresolved.symbol,
                                         item.module});
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
    ExecutionSessionOptions options, ExecutionLoadSummary load_summary,
    runtime::RuntimeImportRegistry* runtime_imports,
    const format::ModuleMetadata* module_metadata,
    const format::DynamicSymbolTable* symbols)
    : memory_(&memory), function_map_(&function_map), imports_(unresolved_relocations),
      runtime_imports_(runtime_imports == nullptr ? &empty_runtime_imports_ : runtime_imports),
      module_metadata_(module_metadata), symbols_(symbols), options_(std::move(options)),
      load_summary_(load_summary)
{
}

ExecutionSession::ExecutionSession(
    memory::GuestMemory& memory, const analysis::ProcessFunctionMap& function_map,
    const analysis::ProcessImage& process_image, ExecutionSessionOptions options,
    ExecutionLoadSummary load_summary, runtime::RuntimeImportRegistry* runtime_imports)
    : memory_(&memory), process_function_map_(&function_map), process_image_(&process_image),
      imports_(process_image),
      runtime_imports_(runtime_imports == nullptr ? &empty_runtime_imports_ : runtime_imports),
      options_(std::move(options)), load_summary_(load_summary)
{
    for (const auto& map : function_map.maps())
    {
        if (map.identity().module == process_image.summary().primary_module)
        {
            function_map_ = &map;
            break;
        }
    }
}

ExecutionSession::ExecutionSession(
    memory::GuestMemory& memory, const analysis::ProcessFunctionMap& function_map,
    const std::vector<loader::UnresolvedRelocation>& unresolved_relocations,
    ExecutionSessionOptions options, ExecutionLoadSummary load_summary,
    runtime::RuntimeImportRegistry* runtime_imports)
    : memory_(&memory), process_function_map_(&function_map), imports_(unresolved_relocations),
      runtime_imports_(runtime_imports == nullptr ? &empty_runtime_imports_ : runtime_imports),
      options_(std::move(options)), load_summary_(load_summary)
{
    if (!function_map.maps().empty()) function_map_ = &function_map.maps().front();
}

const analysis::FunctionRecord* ExecutionSession::function_record(GuestAddress entry) const noexcept
{
    if (process_function_map_ != nullptr) return process_function_map_->find(entry);
    return function_map_ == nullptr ? nullptr : function_map_->find(entry);
}

const analysis::FinalizedFunctionMap* ExecutionSession::function_map_for(
    GuestAddress entry) const noexcept
{
    if (process_function_map_ != nullptr) return process_function_map_->map_for(entry);
    return function_map_;
}

std::string ExecutionSession::module_name_for(GuestAddress entry) const
{
    const auto* map = function_map_for(entry);
    if (map != nullptr) return map->identity().module;
    if (process_image_ != nullptr)
    {
        if (const auto* module = process_image_->module_for_address(entry, 4U))
            return module->identity.module;
    }
    return {};
}

void ExecutionSession::prepare_observation_targets()
{
    observation_targets_.clear();
    if (process_image_ == nullptr) return;

    // M17 observes only the selected guest provider's scalar UMULH sites. The
    // set is derived from parsed CFGs, never from a hard-coded executable
    // address, and is capped before entering the interpreter.
    for (const auto& binding : process_image_->bindings())
    {
        if (binding.symbol != "__nnmusl_init_dso" || !binding.provider_address || !binding.applied)
            continue;
        const auto* record = function_record(binding.provider_address.value());
        if (record == nullptr || !record->cfg) continue;
        for (const auto& [unused, block] : record->cfg->blocks)
        {
            (void)unused;
            for (const auto& instruction : block.instructions)
            {
                if (instruction.id == aarch64::InstructionId::Umulh)
                    observation_targets_.push_back(instruction.address);
                if (observation_targets_.size() >= 32U) return;
            }
        }
    }
    std::sort(observation_targets_.begin(), observation_targets_.end());
    observation_targets_.erase(
        std::unique(observation_targets_.begin(), observation_targets_.end()),
        observation_targets_.end());
}

std::optional<ExecutedGuestInstruction> ExecutionSession::describe_observed_instruction(
    GuestAddress guest_pc, std::size_t call_depth) const
{
    const analysis::FunctionRecord* owner = nullptr;
    const analysis::BasicBlock* owner_block = nullptr;
    const auto inspect_map = [&](const analysis::FinalizedFunctionMap& map) {
        for (const auto& candidate : map.functions())
        {
            if (!candidate.cfg) continue;
            for (const auto& [unused, block] : candidate.cfg->blocks)
            {
                (void)unused;
                const auto found = std::find_if(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const auto& instruction) { return instruction.address == guest_pc; });
                if (found != block.instructions.end())
                {
                    owner = &candidate;
                    owner_block = &block;
                    return;
                }
            }
            if (owner != nullptr) return;
        }
    };
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
        {
            inspect_map(map);
            if (owner != nullptr) break;
        }
    }
    else if (function_map_ != nullptr)
    {
        inspect_map(*function_map_);
    }
    if (owner == nullptr || owner_block == nullptr || !owner->cfg) return std::nullopt;
    for (std::size_t index = 0U; index < owner_block->instructions.size(); ++index)
    {
        const auto& instruction = owner_block->instructions[index];
        if (instruction.address != guest_pc || instruction.id != aarch64::InstructionId::Umulh)
            continue;
        ExecutedGuestInstruction result;
        result.guest_pc = guest_pc;
        result.module = owner->module;
        result.mnemonic = instruction.disassembly.empty()
                              ? std::string(aarch64::instruction_id_name(instruction.id))
                              : instruction.disassembly;
        result.executed = true;
        result.call_depth = call_depth;
        if (!instruction.operands.empty() &&
            instruction.operands.front().kind == aarch64::OperandKind::Register)
        {
            result.destination_register = aarch64::register_name(instruction.operands.front().reg);
        }
        for (std::size_t operand_index = 1U; operand_index < instruction.operands.size();
             ++operand_index)
        {
            const auto& operand = instruction.operands[operand_index];
            if (operand.kind == aarch64::OperandKind::Register)
                result.source_registers.push_back(aarch64::register_name(operand.reg));
        }
        if (index + 1U < owner_block->instructions.size())
        {
            result.next_guest_pc = owner_block->instructions[index + 1U].address;
        }
        else if (instruction.control_flow.kind == aarch64::ControlFlowKind::Fallthrough)
        {
            const auto next = checked_add_u64(guest_pc, 4U);
            if (next) result.next_guest_pc = next.value();
        }
        return result;
    }
    return std::nullopt;
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
    case ErrorCode::RuntimeImportUnimplemented:
        return ExecutionStopReason::RuntimeImportUnimplemented;
    case ErrorCode::RuntimeImportAbiViolation:
        return ExecutionStopReason::RuntimeImportAbiViolation;
    case ErrorCode::RuntimeImportMemoryFault:
        return ExecutionStopReason::RuntimeImportMemoryFault;
    case ErrorCode::RuntimeImportInvariantViolation:
        return ExecutionStopReason::RuntimeImportInvariantViolation;
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
                                               runtime::ExecutionBoundaryKind::None, {}, {}, {}, {}}))
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
        const auto* owning_map = function_map_for(entry);
        for (const auto& conflict : owning_map == nullptr
                                         ? std::vector<analysis::FunctionBoundaryConflict>{}
                                         : owning_map->conflicts())
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
    lifter::LiftOptions lift_options;
    lift_options.stop_at_unsupported_instruction = true;
    const auto lifted = lifter::lift_function(record->cfg.value(), lift_options);
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
    result.executed_function_modules.push_back(module_name_for(entry));
    result.maximum_call_depth = std::max(result.maximum_call_depth, current_.call_depth);
    ExecutionEvent event{0U, ExecutionEventKind::FunctionEnter, entry, entry, 0U, false,
                         current_.call_depth, runtime::ExecutionBoundaryKind::None,
                         {}, {}, {}, {}};
    event.function_module = module_name_for(entry);
    return record_event(result, std::move(event))
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
    result.current_function_module = module_name_for(current_.function_entry);
    result.stop_module = module_name_for(target.value_or(current_.function_entry));
    if (result.stop_module.empty() && result.import_boundary)
    {
        result.stop_module = result.import_boundary->consumer_module;
    }
    result.stop_pc = boundary == nullptr ? cpu_.pc : boundary->boundary.source_guest_pc;
    if (boundary != nullptr)
    {
        if (boundary->boundary.kind == runtime::ExecutionBoundaryKind::UnsupportedInstruction)
            result.diagnostic_pc = boundary->boundary.source_guest_pc;
        else
            result.source_pc = boundary->boundary.source_guest_pc;
    }
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
    else if (reason == ExecutionStopReason::RuntimeImportUnimplemented ||
             reason == ExecutionStopReason::RuntimeImportAbiViolation ||
             reason == ExecutionStopReason::RuntimeImportMemoryFault ||
             reason == ExecutionStopReason::RuntimeImportInvariantViolation)
        event_kind = ExecutionEventKind::RuntimeImportBoundary;
    const auto import_symbol = result.import_boundary
                                   ? result.import_boundary->symbol.name
                                   : std::string{};
    ExecutionEvent stop_event{0U, event_kind, current_.function_entry, result.stop_pc,
                               target.value_or(0U), target.has_value(), current_.call_depth,
                               boundary == nullptr ? runtime::ExecutionBoundaryKind::None
                                                   : boundary->boundary.kind,
                               execution_stop_reason_name(reason), import_symbol, {}, {}};
    stop_event.function_module = result.current_function_module;
    stop_event.target_module = module_name_for(target.value_or(current_.function_entry));
    if (!record_event(result, std::move(stop_event)))
    {
        return Result<void>::success();
    }
    ExecutionEvent session_stop{0U, ExecutionEventKind::SessionStop, current_.function_entry,
                                result.stop_pc, target.value_or(0U), target.has_value(),
                                current_.call_depth,
                                boundary == nullptr ? runtime::ExecutionBoundaryKind::None
                                                    : boundary->boundary.kind,
                                execution_stop_reason_name(reason), import_symbol, {}, {}};
    session_stop.function_module = result.current_function_module;
    session_stop.target_module = module_name_for(target.value_or(current_.function_entry));
    (void)record_event(result, std::move(session_stop));
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
            const auto invocation = call ? runtime::ExternalInvocationKind::Call
                                         : runtime::ExternalInvocationKind::TailTransfer;
            return dispatch_runtime_import(boundary, *import, invocation, result);
        }
    }
    if ((target & 0x3U) != 0U || target == 0U)
    {
        return stop(result, ExecutionStopReason::InvalidIndirectTarget,
                    "indirect target is zero or not AArch64 aligned", target, &boundary);
    }
    const auto* record = function_record(target);
    if (process_image_ != nullptr && process_image_->module_for_address(target, 4U) == nullptr)
    {
        return stop(result, ExecutionStopReason::InvalidCrossModuleTarget,
                    "target is not owned by exactly one loaded process module", target, &boundary);
    }
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
    ExecutionEvent call_event{0U, event_kind, current_.function_entry,
                              boundary.boundary.source_guest_pc,
                              boundary.boundary.target_guest_address, true,
                              current_.call_depth, boundary.boundary.kind, {}, {}, {}, {}};
    call_event.function_module = module_name_for(current_.function_entry);
    call_event.target_module = module_name_for(boundary.boundary.target_guest_address);
    if (!record_event(result, std::move(call_event)))
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
    ExecutionEvent transfer_event{0U, ExecutionEventKind::FunctionTransfer,
                                  current_.function_entry,
                                  boundary.boundary.source_guest_pc,
                                  boundary.boundary.target_guest_address, true,
                                  current_.call_depth, boundary.boundary.kind, {}, {}, {}, {}};
    transfer_event.function_module = module_name_for(current_.function_entry);
    transfer_event.target_module = module_name_for(boundary.boundary.target_guest_address);
    if (!record_event(result, std::move(transfer_event)))
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
    result.executed_function_modules.push_back(module_name_for(current_.function_entry));
    result.maximum_call_depth = std::max(result.maximum_call_depth, current_.call_depth);
    ExecutionEvent event{0U, ExecutionEventKind::FunctionEnter, current_.function_entry,
                         current_.function_entry, 0U, false, current_.call_depth,
                         runtime::ExecutionBoundaryKind::None, {}, {}, {}, {}};
    event.function_module = module_name_for(current_.function_entry);
    return record_event(result, std::move(event))
               ? Result<void>::success()
               : Result<void>::success();
}

Result<void> ExecutionSession::dispatch_runtime_import(
    const runtime::ExecutionResult& boundary, const ImportBoundary& import,
    runtime::ExternalInvocationKind invocation, ExecutionSessionResult& result)
{
    ++result.runtime.imports_encountered;
    const analysis::ProcessBinding* process_binding = nullptr;
    bool runtime_fallback_eligible = true;
    std::string guest_provider_resolution = "not_available_without_process_image";
    if (process_image_ != nullptr)
    {
        const auto binding_it = std::find_if(
            process_image_->bindings().begin(), process_image_->bindings().end(),
            [&](const auto& binding) {
                return binding.consumer_module == import.consumer_module &&
                       binding.relocation_index == import.relocation_index &&
                       binding.relocation.target_address == import.relocation_target;
            });
        if (binding_it != process_image_->bindings().end()) process_binding = &*binding_it;
        if (process_binding == nullptr)
        {
            runtime_fallback_eligible = false;
            guest_provider_resolution = "binding_missing";
        }
        else
        {
            guest_provider_resolution = std::string(
                analysis::provider_resolution_status_name(process_binding->provider.status));
            runtime_fallback_eligible = process_binding->provider.status ==
                                        analysis::ProviderResolutionStatus::NotFoundInSuppliedModules;
        }
    }
    const auto* registered_descriptor = runtime_imports_ == nullptr
                                            ? nullptr
                                            : runtime_imports_->find(import.symbol.name);
    const auto* descriptor = runtime_fallback_eligible ? registered_descriptor : nullptr;

    RuntimeImportObservation observation;
    observation.provenance = import;
    observation.invocation = invocation;
    observation.source_guest_pc = boundary.boundary.source_guest_pc;
    observation.guest_provider_resolution = std::move(guest_provider_resolution);
    observation.runtime_fallback_eligible = runtime_fallback_eligible;
    if (registered_descriptor != nullptr)
    {
        observation.descriptor = *registered_descriptor;
        if (descriptor != nullptr) ++result.runtime.imports_resolved;
    }
    else
    {
        observation.descriptor.symbol_name = import.symbol.name;
        observation.descriptor.support = runtime::RuntimeSupportStatus::Unknown;
        observation.descriptor.subsystem = runtime::RuntimeSubsystem::Unknown;
        observation.descriptor.evidence.confidence = "not_registered";
    }

    const format::ModuleMetadata* import_metadata = module_metadata_;
    const format::DynamicSymbolTable* import_symbols = symbols_;
    if (process_image_ != nullptr && !import.consumer_module.empty())
    {
        if (const auto* module = process_image_->module(import.consumer_module))
        {
            import_metadata = &module->metadata;
            import_symbols = module->symbols ? &module->symbols.value() : nullptr;
        }
    }
    const auto* dynamic = import_metadata != nullptr && import_metadata->dynamic
                              ? &import_metadata->dynamic.value()
                              : nullptr;
    classify_import_trampoline(function_record(current_.function_entry), boundary, import, dynamic,
                               observation);
    if (dynamic != nullptr && dynamic->pltgot)
    {
        observation.dynamic_pltgot = dynamic->pltgot->address;
        if (import.relocation_target >= dynamic->pltgot->address)
        {
            observation.dynamic_pltgot_slot_delta =
                import.relocation_target - dynamic->pltgot->address;
        }
    }

    const runtime::AbiSignature empty_signature;
    const auto& signature = descriptor == nullptr ? empty_signature : descriptor->signature;
    runtime::AArch64GuestCall abi(cpu_, *memory_, signature);
    observation.abi = abi.snapshot();
    const auto observed_count = descriptor == nullptr ? 0U : signature.observed_argument_count;
    observation.arguments.reserve(observed_count);
    for (std::size_t index = 0U; index < observed_count; ++index)
    {
        observation.arguments.push_back(
            observe_argument(abi, index, *memory_, dynamic, import_symbols));
    }

    const auto runtime_event = [&](ExecutionEventKind kind, std::string code) {
        return record_event(result, ExecutionEvent{0U, kind, current_.function_entry,
                                                     boundary.boundary.source_guest_pc,
                                                     boundary.boundary.target_guest_address, true,
                                                     current_.call_depth, boundary.boundary.kind,
                                                     std::move(code), import.symbol.name, {}, {}});
    };

    if (!runtime_fallback_eligible)
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "guest_provider_resolution_blocked";
        observation.outcome_diagnostic =
            "runtime fallback is forbidden until complete guest-provider resolution reports "
            "provider_not_found_complete (resolution=" + observation.guest_provider_resolution + ")";
        result.runtime.imports.push_back(std::move(observation));
        return stop(result, ExecutionStopReason::UnresolvedImport,
                    "guest provider resolution did not authorize runtime fallback for '" +
                        import.symbol.name + "'",
                    boundary.boundary.target_guest_address, &boundary);
    }

    if (descriptor == nullptr)
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "unresolved";
        observation.outcome_diagnostic =
            "no runtime descriptor is registered for the provenance-backed import";
        result.runtime.imports.push_back(std::move(observation));
        return stop(result, ExecutionStopReason::UnresolvedImport,
                    "indirect target originates at unresolved relocation " +
                        hex_address(import.relocation_target),
                    boundary.boundary.target_guest_address, &boundary);
    }

    if (!runtime_event(ExecutionEventKind::RuntimeImportResolved,
                       std::string(runtime::runtime_support_status_name(descriptor->support))))
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "event_limit";
        result.runtime.imports.push_back(std::move(observation));
        return Result<void>::success();
    }
    if (!runtime_event(ExecutionEventKind::RuntimeImportEnter,
                       std::string(runtime::external_invocation_kind_name(invocation))))
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "event_limit";
        result.runtime.imports.push_back(std::move(observation));
        return Result<void>::success();
    }
    if (!runtime_event(ExecutionEventKind::RuntimeImportArgumentSummary,
                       "observed_argument_slots=" + std::to_string(observed_count)))
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "event_limit";
        result.runtime.imports.push_back(std::move(observation));
        return Result<void>::success();
    }

    runtime::RuntimeImportContext context{abi, *memory_, runtime_state_, *descriptor,
                                          runtime::ImportProvenance{
                                              import.relocation_target, import.relocation_index,
                                              import.relocation, import.symbol},
                                          invocation, import_metadata};
    const auto invoked = runtime_imports_->invoke(context);
    runtime::RuntimeImportOutcome outcome = invoked
                                                ? std::move(invoked).value()
                                                : runtime::RuntimeImportOutcome::invariant_violation(
                                                      invoked.error());
    if (outcome.abi_validated)
    {
        observation.abi_validation = outcome.abi_signature_known
                                          ? "valid"
                                          : "observed_slots_valid_signature_unknown";
    }
    else
    {
        observation.abi_validation = "invalid";
    }
    observation.abi_diagnostic = outcome.error ?
                                     std::string(error_code_name(outcome.error->code)) + ": " +
                                         outcome.error->message
                                                     : std::string{};
    observation.outcome = std::string(runtime::runtime_import_outcome_name(outcome.kind));
    observation.outcome_diagnostic = outcome.diagnostic;
    result.runtime.dso_modules_registered = runtime_state_.dso_modules_registered();
    result.runtime.imports.push_back(std::move(observation));

    if (outcome.kind != runtime::RuntimeImportOutcomeKind::Handled)
    {
        switch (outcome.kind)
        {
        case runtime::RuntimeImportOutcomeKind::Unimplemented:
            ++result.runtime.imports_unimplemented;
            return stop(result, ExecutionStopReason::RuntimeImportUnimplemented,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::AbiViolation:
            ++result.runtime.imports_abi_violations;
            return stop(result, ExecutionStopReason::RuntimeImportAbiViolation,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::MemoryFault:
            ++result.runtime.imports_memory_faults;
            return stop(result, ExecutionStopReason::RuntimeImportMemoryFault,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::InvariantViolation:
            ++result.runtime.imports_invariant_violations;
            return stop(result, ExecutionStopReason::RuntimeImportInvariantViolation,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::Handled:
            break;
        }
    }

    if (!runtime_event(ExecutionEventKind::RuntimeImportReturn,
                       std::string(runtime::runtime_import_outcome_name(outcome.kind))))
    {
        return Result<void>::success();
    }
    ++result.runtime.imports_handled;
    ++result.runtime.import_returns;

    if (invocation == runtime::ExternalInvocationKind::Call)
    {
        if (boundary.boundary.continuation_block == ir::invalid_block ||
            boundary.boundary.continuation_guest_pc == 0U ||
            (boundary.boundary.continuation_guest_pc & 0x3U) != 0U)
        {
            ++result.runtime.imports_invariant_violations;
            return stop(result, ExecutionStopReason::RuntimeImportInvariantViolation,
                        "handled external call has no valid guest continuation",
                        boundary.boundary.target_guest_address, &boundary);
        }
        current_.interpreter.current_block = boundary.boundary.continuation_block;
        cpu_.pc = boundary.boundary.continuation_guest_pc;
        return Result<void>::success();
    }

    if (current_.expected_return_pc == 0U || (current_.expected_return_pc & 0x3U) != 0U)
    {
        ++result.runtime.imports_invariant_violations;
        return stop(result, ExecutionStopReason::RuntimeImportInvariantViolation,
                    "handled tail external transfer has no inherited guest return contract",
                    boundary.boundary.target_guest_address, &boundary);
    }
    if (suspended_frames_.empty())
    {
        return stop(result, ExecutionStopReason::EntryReturned,
                    "tail runtime import returned through the inherited entry contract",
                    std::nullopt, &boundary);
    }

    const auto expected_return = current_.expected_return_pc;
    current_ = std::move(suspended_frames_.back());
    suspended_frames_.pop_back();
    cpu_.pc = expected_return;
    return record_event(result, ExecutionEvent{0U, ExecutionEventKind::FunctionResume,
                                                current_.function_entry, expected_return, 0U,
                                                false, current_.call_depth,
                                                runtime::ExecutionBoundaryKind::Return,
                                                "runtime_tail_transfer_resume",
                                                import.symbol.name, {}, {}})
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
    runtime_state_.reset();
    ExecutionSessionResult result;
    result.identity = function_map_->identity();
    result.entry = entry;
    result.options = options_;
    result.relocations = load_summary_;
    if (process_image_ != nullptr)
    {
        result.process = process_image_->summary();
        result.relocations.guest_bindings_attempted = result.process->bindings.size();
        for (const auto& binding : result.process->bindings)
        {
            if (binding.provider.status == analysis::ProviderResolutionStatus::ResolvedGuestModule)
            {
                ++result.relocations.guest_bindings_resolved;
                if (binding.provider_module && binding.provider_module.value() !=
                                                     binding.consumer_module)
                {
                    ++result.relocations.cross_module_relocations;
                }
            }
            else if (binding.provider.status == analysis::ProviderResolutionStatus::AmbiguousGuestProvider)
            {
                ++result.relocations.guest_bindings_ambiguous;
            }
            else
            {
                ++result.relocations.guest_bindings_unresolved;
            }
        }
    }
    if (module_metadata_ != nullptr)
    {
        result.module_metadata = *module_metadata_;
    }
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
        {
            result.analyzed_functions += map.functions().size();
            result.precise_conflicts += map.conflicts().size();
        }
    }
    else
    {
        result.analyzed_functions = function_map_->functions().size();
        result.precise_conflicts = function_map_->conflicts().size();
    }
    std::set<GuestAddress> conflicting_functions;
    std::vector<const analysis::FunctionBoundaryConflict*> conflicts;
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
            for (const auto& conflict : map.conflicts()) conflicts.push_back(&conflict);
    }
    else
    {
        for (const auto& conflict : function_map_->conflicts()) conflicts.push_back(&conflict);
    }
    for (const auto* conflict : conflicts)
    {
        conflicting_functions.insert(conflict->first_function);
        conflicting_functions.insert(conflict->second_function);
    }
    result.conflicting_functions = conflicting_functions.size();
    const auto add_owned_bytes = [&](const auto& map) -> Result<void> {
        for (const auto& function : map.functions())
        {
            const auto owned = analysis::precise_owned_byte_count(function.owned_code_ranges);
            if (!owned || owned.value() > std::numeric_limits<memory::GuestSize>::max() -
                                  result.precise_owned_bytes)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::AnalysisBudgetExceeded, "precise ownership byte count overflows"));
            }
            result.precise_owned_bytes += owned.value();
        }
        return Result<void>::success();
    };
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
        {
            const auto owned = add_owned_bytes(map);
            if (!owned)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(owned.error());
            }
        }
    }
    else
    {
        const auto owned = add_owned_bytes(*function_map_);
        if (!owned)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(owned.error());
        }
    }
    prepare_observation_targets();
    result.observation_targets = observation_targets_;
    if (!record_event(result, ExecutionEvent{0U, ExecutionEventKind::SessionStart, 0U, 0U, 0U,
                                               false, 0U,
                                               runtime::ExecutionBoundaryKind::None, {}, {}, {}, {}}) ||
        !record_event(result, ExecutionEvent{0U, ExecutionEventKind::EntrySelected, 0U,
                                               entry.address, entry.address, true, 0U,
                                               runtime::ExecutionBoundaryKind::None,
                                               entry_selection_kind_name(entry.kind), {}, {}, {}}))
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
        interpreter_options.observed_guest_pcs =
            std::span<const memory::GuestAddress>(observation_targets_);
        interpreter_options.max_observed_guest_pcs = 32U;
        const auto step = interpreter::execute_until_boundary(
            *function, cpu_, runtime_, current_.interpreter, interpreter_options);
        if (!step)
        {
            (void)stop(result, classify_error(step.error()), step.error().message);
            break;
        }
        result.ir_operations += step.value().executed_operations;
        result.guest_blocks += step.value().executed_blocks;
        for (const auto observed_pc : step.value().observed_guest_pcs)
        {
            const auto already_recorded = std::find_if(
                result.executed_guest_instructions.begin(), result.executed_guest_instructions.end(),
                [&](const auto& observed) { return observed.guest_pc == observed_pc; });
            if (already_recorded != result.executed_guest_instructions.end()) continue;
            if (const auto observed = describe_observed_instruction(observed_pc,
                                                                    current_.call_depth))
                result.executed_guest_instructions.push_back(observed.value());
        }
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
                                                       boundary.kind, {}, {}, {}, {}}))
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
                                                       runtime::ExecutionBoundaryKind::Return, {}, {}, {}, {}}))
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
        case runtime::ExecutionBoundaryKind::UnsupportedInstruction:
            (void)stop(result, ExecutionStopReason::UnsupportedInstruction,
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
    if (process_image_ != nullptr)
    {
        for (const auto& binding : process_image_->bindings())
        {
            if (!binding.provider_address || !binding.applied) continue;
            result.provider_guest_code_entered |= std::find(
                result.executed_functions.begin(), result.executed_functions.end(),
                binding.provider_address.value()) != result.executed_functions.end();
        }
    }
    result.runtime.dso_modules_registered = runtime_state_.dso_modules_registered();
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
        if (!event.import_symbol.empty()) item["import_symbol"] = event.import_symbol;
        if (!event.function_module.empty()) item["function_module"] = event.function_module;
        if (!event.target_module.empty()) item["target_module"] = event.target_module;
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
    json executed_with_modules = json::array();
    for (std::size_t index = 0U; index < result.executed_functions.size(); ++index)
    {
        executed_with_modules.push_back(json{
            {"address", hex_address(result.executed_functions[index])},
            {"module", index < result.executed_function_modules.size()
                            ? result.executed_function_modules[index]
                            : std::string{}}});
    }
    json executed_guest_instructions = json::array();
    for (const auto& instruction : result.executed_guest_instructions)
    {
        json source_registers = json::array();
        for (const auto& register_name : instruction.source_registers)
            source_registers.push_back(register_name);
        executed_guest_instructions.push_back(json{
            {"guest_pc", hex_address(instruction.guest_pc)},
            {"module", instruction.module},
            {"mnemonic", instruction.mnemonic},
            {"executed", instruction.executed},
            {"next_guest_pc", instruction.next_guest_pc
                                  ? json(hex_address(instruction.next_guest_pc.value()))
                                  : json(nullptr)},
            {"destination_register", instruction.destination_register},
            {"source_registers", std::move(source_registers)},
            {"call_depth", instruction.call_depth}});
    }
    json observation_targets = json::array();
    for (const auto target : result.observation_targets)
        observation_targets.push_back(hex_address(target));
    json process = nullptr;
    if (result.process)
    {
        json modules = json::array();
        for (const auto& module : result.process->modules)
        {
            json ranges = json::array();
            for (const auto& range : module.mapped_ranges) ranges.push_back(range_json(range));
            json segments = json::array();
            for (const auto& segment : module.segments)
            {
                segments.push_back(json{{"kind", segment.kind},
                                        {"file_offset", segment.file_offset},
                                        {"memory_offset", segment.memory_offset},
                                        {"memory_size", segment.memory_size},
                                        {"stored_size", segment.stored_size},
                                        {"compressed", segment.compressed},
                                        {"hash_required", segment.hash_required}});
            }
            json mappings = json::array();
            for (const auto& mapping : module.mappings)
            {
                mappings.push_back(json{{"base", hex_address(mapping.base)},
                                        {"size", mapping.size},
                                        {"permissions", permissions_text(mapping.permissions)},
                                        {"read", memory::has_permission(
                                                      mapping.permissions,
                                                      memory::GuestMemoryPermissions::Read)},
                                        {"write", memory::has_permission(
                                                       mapping.permissions,
                                                       memory::GuestMemoryPermissions::Write)},
                                        {"execute", memory::has_permission(
                                                         mapping.permissions,
                                                         memory::GuestMemoryPermissions::Execute)},
                                        {"kind", memory::guest_region_kind_name(mapping.kind)}});
            }
            modules.push_back(json{
                {"logical_name", module.logical_name},
                {"name_provenance", module.name_provenance},
                {"input_size", module.input_size},
                {"sha256", module.sha256},
                {"build_id", module.build_id},
                {"nso_version", module.nso_version},
                {"nso_flags", module.nso_flags},
                {"base", hex_address(module.base)},
                {"base_provenance", analysis::module_base_provenance_name(module.base_provenance)},
                {"runtime_base_verified", module.runtime_base_verified},
                {"segments", std::move(segments)},
                {"bss_size", module.bss_size},
                {"mapped_ranges", std::move(ranges)},
                {"mappings", std::move(mappings)},
                {"metadata", json{{"mod0", module.mod0_available},
                                   {"mod0_status", module.mod0_status},
                                   {"dynamic", module.dynamic_available},
                                   {"dynamic_status", module.dynamic_status},
                                   {"provider_index_eligible", module.provider_index_eligible},
                                   {"dynamic_tags", module.dynamic_tags}}},
                {"dynamic_symbols", json{{"total", module.dynamic_symbol_count},
                                          {"defined", module.defined_symbol_count},
                                          {"undefined", module.undefined_symbol_count}}},
                {"relocations", json{{"total", module.relocation_count},
                                      {"applied", module.applied_relocations},
                                      {"unresolved", module.unresolved_relocations}}}});
        }
        json bindings = json::array();
        for (const auto& binding : result.process->bindings)
        {
            json candidates = json::array();
            for (const auto& candidate : binding.provider.candidates)
            {
                candidates.push_back(json{{"module", candidate.module},
                                          {"symbol_index", candidate.symbol_index},
                                          {"symbol", candidate.symbol},
                                          {"binding", format::symbol_binding_name(candidate.binding)},
                                          {"type", format::symbol_type_name(candidate.type)},
                                          {"visibility", format::symbol_visibility_name(candidate.visibility)},
                                          {"section_index", candidate.section_index},
                                          {"value", hex_address(candidate.value)},
                                          {"address", hex_address(candidate.address)},
                                          {"executable", candidate.executable}});
            }
            json occurrences = json::array();
            for (const auto& occurrence : binding.provider.occurrences)
            {
                occurrences.push_back(json{{"module", occurrence.module},
                                           {"symbol_index", occurrence.symbol_index},
                                           {"symbol", occurrence.symbol},
                                           {"defined", occurrence.defined},
                                           {"binding", format::symbol_binding_name(occurrence.binding)},
                                           {"type", format::symbol_type_name(occurrence.type)},
                                           {"visibility", format::symbol_visibility_name(occurrence.visibility)},
                                           {"section_index", occurrence.section_index},
                                           {"value", hex_address(occurrence.value)},
                                           {"address", occurrence.address
                                                            ? json(hex_address(occurrence.address.value()))
                                                            : json(nullptr)},
                                           {"executable", occurrence.executable},
                                           {"eligible", occurrence.eligible},
                                           {"eligibility", analysis::provider_eligibility_name(
                                                                occurrence.eligibility)}});
            }
            bindings.push_back(json{
                {"consumer_module", binding.consumer_module},
                {"consumer_symbol_index", binding.consumer_symbol_index},
                {"symbol", binding.symbol},
                {"relocation_index", binding.relocation_index},
                {"relocation", json{{"type", binding.relocation.raw_type},
                                     {"type_name", format::aarch64_relocation_type_name(binding.relocation.type)},
                                     {"source", format::relocation_source_name(binding.relocation.source)},
                                     {"target", hex_address(binding.relocation.target_address)},
                                     {"offset", hex_address(binding.relocation.offset)}}},
                {"candidate_count", binding.provider.candidates.size()},
                {"result", analysis::provider_resolution_status_name(binding.provider.status)},
                {"provider_module", binding.provider_module ? json(*binding.provider_module) : json(nullptr)},
                {"provider_symbol_index", binding.provider_symbol_index
                                                ? json(*binding.provider_symbol_index) : json(nullptr)},
                {"provider_base", binding.provider_base
                                      ? json(hex_address(*binding.provider_base)) : json(nullptr)},
                {"provider_symbol_value", binding.provider_symbol_value
                                                ? json(hex_address(*binding.provider_symbol_value)) : json(nullptr)},
                {"provider_address", binding.provider_address
                                          ? json(hex_address(*binding.provider_address)) : json(nullptr)},
                {"resolved_value", binding.resolved_value
                                        ? json(hex_address(*binding.resolved_value)) : json(nullptr)},
                {"slot_value_verified", binding.slot_value_verified},
                {"resolution_basis", binding.resolution_basis},
                {"confidence", binding.confidence},
                {"applied", binding.applied},
                {"candidates", std::move(candidates)},
                {"occurrences", std::move(occurrences)},
                {"completeness", analysis::module_set_completeness_name(binding.provider.completeness)},
                {"completeness_basis", analysis::module_set_completeness_basis_name(
                                            binding.provider.completeness_basis)}});
        }
        const auto focus = [&]() {
            json focus_occurrences = json::array();
            json focus_candidates = json::array();
            const auto lookup = result.process->focus_provider.value_or(analysis::ProviderLookup{});
            for (const auto& occurrence : lookup.occurrences)
            {
                focus_occurrences.push_back(json{{"module", occurrence.module},
                                                 {"symbol_index", occurrence.symbol_index},
                                                 {"symbol", occurrence.symbol},
                                                 {"defined", occurrence.defined},
                                                 {"binding", format::symbol_binding_name(occurrence.binding)},
                                                 {"type", format::symbol_type_name(occurrence.type)},
                                                 {"visibility", format::symbol_visibility_name(occurrence.visibility)},
                                                 {"section_index", occurrence.section_index},
                                                 {"value", hex_address(occurrence.value)},
                                                 {"guest_address", occurrence.address
                                                                       ? json(hex_address(occurrence.address.value()))
                                                                       : json(nullptr)},
                                                 {"executable", occurrence.executable},
                                                 {"eligible", occurrence.eligible},
                                                 {"eligibility", analysis::provider_eligibility_name(
                                                                      occurrence.eligibility)}});
            }
            for (const auto& candidate : lookup.candidates)
                focus_candidates.push_back(json{{"module", candidate.module},
                                                {"symbol_index", candidate.symbol_index},
                                                {"symbol", candidate.symbol},
                                                {"binding", format::symbol_binding_name(candidate.binding)},
                                                {"type", format::symbol_type_name(candidate.type)},
                                                {"visibility", format::symbol_visibility_name(candidate.visibility)},
                                                {"value", hex_address(candidate.value)},
                                                {"guest_address", hex_address(candidate.address)},
                                                {"executable", candidate.executable}});
            json provider = nullptr;
            if (lookup.selected_candidate && lookup.selected_candidate.value() < lookup.candidates.size())
            {
                const auto& selected = lookup.candidates[lookup.selected_candidate.value()];
                provider = json{{"module", selected.module},
                                {"symbol_index", selected.symbol_index},
                                {"guest_address", hex_address(selected.address)}};
            }
            bool host_handler_used = false;
            for (const auto& import : result.runtime.imports)
                host_handler_used |= import.provenance.symbol.name == "__nnmusl_init_dso";
            return json{{"__nnmusl_init_dso", json{
                {"occurrences", std::move(focus_occurrences)},
                {"eligible_provider_candidates", std::move(focus_candidates)},
                {"result", analysis::provider_resolution_status_name(lookup.status)},
                {"search_complete", lookup.completeness != analysis::ModuleSetCompleteness::Incomplete},
                {"completeness", analysis::module_set_completeness_name(lookup.completeness)},
                {"completeness_basis", analysis::module_set_completeness_basis_name(lookup.completeness_basis)},
                {"provider", std::move(provider)},
                {"selected_candidate", lookup.selected_candidate
                                             ? json(lookup.selected_candidate.value()) : json(nullptr)},
                {"provider_guest_code_entered", result.provider_guest_code_entered},
                {"host_handler_used", host_handler_used}}}};
        }();
        process = json{{"primary_module", result.process->primary_module},
                       {"layout_mode", result.process->layout_mode},
                       {"provider_search_complete", result.process->provider_search_complete},
                       {"module_set", json{{"source", result.process->source},
                                            {"completeness", analysis::module_set_completeness_name(
                                                                  result.process->completeness)},
                                            {"completeness_basis", analysis::module_set_completeness_basis_name(
                                                                  result.process->completeness_basis)},
                                            {"coherence", analysis::module_set_coherence_name(
                                                                  result.process->coherence)},
                                            {"coherence_basis", result.process->coherence_basis},
                                            {"module_load_order", result.process->module_load_order},
                                            {"module_load_order_basis", result.process->module_load_order_basis},
                                            {"module_count", result.process->module_count},
                                            {"executable_module_count", result.process->executable_module_count},
                                            {"relocations_planned", result.process->relocations_planned},
                                            {"transactional_relocation_success",
                                             result.process->transactional_relocation_success},
                                            {"ignored_entries", result.process->ignored_module_entries}}},
                       {"modules", std::move(modules)}, {"bindings", std::move(bindings)},
                       {"focus_symbols", std::move(focus)}};
    }
    json value{{"schema_version", ExecutionSessionResult::schema_version},
                {"module", json{{"logical_name", result.identity.module},
                                  {"sha256", result.identity.input_sha256},
                                  {"build_id", result.identity.build_id},
                                  {"analysis_base", hex_address(result.identity.guest_base)},
                                  {"guest_base_verified", result.identity.guest_base_verified},
                                  {"base_provenance", analysis::module_base_provenance_name(
                                                          result.identity.guest_base_provenance)},
                                  {"executable_ranges", std::move(executable_ranges)}}},
                {"relocations", json{{"parsed", result.relocations.relocations_parsed},
                                      {"applied", result.relocations.relocations_applied},
                                      {"unresolved", result.relocations.unresolved_relocations},
                                      {"guest_bindings_attempted", result.relocations.guest_bindings_attempted},
                                      {"guest_bindings_resolved", result.relocations.guest_bindings_resolved},
                                      {"guest_bindings_ambiguous", result.relocations.guest_bindings_ambiguous},
                                      {"guest_bindings_unresolved", result.relocations.guest_bindings_unresolved},
                                      {"cross_module_relocations", result.relocations.cross_module_relocations}}},
                {"analysis", json{{"functions", result.analyzed_functions},
                                   {"conflicting_functions", result.conflicting_functions},
                                   {"conflict_records", result.precise_conflicts},
                                   {"precise_conflicts", result.precise_conflicts},
                                   {"precise_owned_bytes", result.precise_owned_bytes}}},
                {"metadata", module_metadata_json(result.module_metadata)},
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
                                    {"source_pc", result.source_pc
                                                       ? json(hex_address(result.source_pc.value()))
                                                       : json(nullptr)},
                                    {"diagnostic_pc", result.diagnostic_pc
                                                           ? json(hex_address(result.diagnostic_pc.value()))
                                                           : json(nullptr)},
                                    {"current_function", hex_address(result.current_function)},
                                    {"current_function_module", result.current_function_module},
                                    {"stop_module", result.stop_module},
                                    {"target", result.target ? json(hex_address(result.target.value())) : json(nullptr)},
                                    {"target_register", result.target_register},
                                    {"target_provenance", result.target_provenance},
                                    {"executed_functions", std::move(executed)},
                                    {"executed_functions_with_modules", std::move(executed_with_modules)},
                                    {"direct_calls", result.direct_calls},
                                    {"indirect_calls", result.indirect_calls},
                                    {"function_transfers", result.function_transfers},
                                    {"returns", result.returns},
                                    {"ir_operations", result.ir_operations},
                                    {"guest_blocks", result.guest_blocks},
                                    {"maximum_call_depth", result.maximum_call_depth},
                                    {"provider_guest_code_entered", result.provider_guest_code_entered},
                                    {"observation_targets", std::move(observation_targets)},
                                    {"executed_guest_instructions",
                                     std::move(executed_guest_instructions)},
                                    {"diagnostic", result.diagnostic}}},
                {"budgets", json{{"max_ir_operations", result.options.budgets.max_ir_operations},
                                  {"max_function_transitions", result.options.budgets.max_function_transitions},
                                  {"max_call_depth", result.options.budgets.max_call_depth},
                                  {"max_events", result.options.budgets.max_events},
                                  {"max_guest_blocks", result.options.budgets.max_guest_blocks}}},
                {"call_stack_snapshot", std::move(stack)},
                {"registers", cpu_json(result.final_cpu)},
                {"runtime", json{{"imports_encountered", result.runtime.imports_encountered},
                                  {"imports_resolved", result.runtime.imports_resolved},
                                  {"imports_handled", result.runtime.imports_handled},
                                  {"imports_unimplemented", result.runtime.imports_unimplemented},
                                  {"imports_abi_violations", result.runtime.imports_abi_violations},
                                  {"imports_memory_faults", result.runtime.imports_memory_faults},
                                  {"imports_invariant_violations",
                                   result.runtime.imports_invariant_violations},
                                  {"import_returns", result.runtime.import_returns},
                                  {"dso_modules_registered",
                                   result.runtime.dso_modules_registered},
                                  {"imports", [&]() {
                                      json imports = json::array();
                                      for (const auto& observation : result.runtime.imports)
                                      {
                                          imports.push_back(runtime_import_json(observation));
                                      }
                                      return imports;
                                  }()}}},
                {"events", std::move(events)},
                {"process", std::move(process)}};
    if (result.import_boundary)
    {
        const auto& import = result.import_boundary.value();
        value["execution"]["import"] = json{{"consumer_module", import.consumer_module},
                                               {"relocation_target", hex_address(import.relocation_target)},
                                               {"relocation_offset", hex_address(import.relocation.offset)},
                                               {"relocation_index", import.relocation_index},
                                               {"symbol_index", import.symbol.symbol_index},
                                               {"symbol", import.symbol.name},
                                               {"binding", format::symbol_binding_name(import.symbol.binding)},
                                               {"symbol_type", format::symbol_type_name(import.symbol.type)},
                                               {"visibility", format::symbol_visibility_name(import.symbol.visibility)},
                                               {"section_index", import.symbol.section_index},
                                               {"undefined_in_main", import.symbol.section_index == 0U},
                                               {"relocation_type", import.relocation.raw_type},
                                               {"relocation_type_name", format::aarch64_relocation_type_name(import.relocation.type)},
                                               {"relocation_source", format::relocation_source_name(import.relocation.source)},
                                               {"addend", import.relocation.addend}};
    }
    return value.dump(2) + "\n";
}

} // namespace switchrecomp::execution
