#include "switchrecomp/analysis/function_map.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace switchrecomp::analysis
{

std::string_view module_base_provenance_name(ModuleBaseProvenance provenance) noexcept
{
    switch (provenance)
    {
    case ModuleBaseProvenance::ExplicitAnalysisBase: return "explicit_analysis_base";
    case ModuleBaseProvenance::DeterministicAnalysisLayout:
        return "deterministic_analysis_layout";
    case ModuleBaseProvenance::ExternallyObserved: return "externally_observed";
    case ModuleBaseProvenance::RuntimeVerified: return "runtime_verified";
    }
    return "unknown";
}

namespace
{

using GuestAddress = memory::GuestAddress;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] std::string synthetic_id(GuestAddress address)
{
    std::ostringstream output;
    output << "sub_" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] int confidence_rank(FunctionConfidence confidence) noexcept
{
    switch (confidence)
    {
    case FunctionConfidence::Conflict: return 6;
    case FunctionConfidence::Manual: return 5;
    case FunctionConfidence::Confirmed: return 4;
    case FunctionConfidence::High: return 3;
    case FunctionConfidence::Medium: return 2;
    case FunctionConfidence::Low: return 1;
    }
    return 0;
}

[[nodiscard]] bool contains(const GuestAddressRange& range, GuestAddress address,
                            memory::GuestSize size = 1U)
{
    const auto end = checked_add_u64(range.base, range.size);
    const auto requested_end = checked_add_u64(address, size);
    return end && requested_end && address >= range.base && requested_end.value() <= end.value();
}

[[nodiscard]] bool contains_any(const std::vector<GuestAddressRange>& ranges,
                                GuestAddress address, memory::GuestSize size = 1U)
{
    return std::any_of(ranges.begin(), ranges.end(), [address, size](const auto& range) {
        return contains(range, address, size);
    });
}

[[nodiscard]] FailureCategory category_for(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::DecodeFailed:
    case ErrorCode::InstructionFetchFailed: return FailureCategory::DecodeFailure;
    case ErrorCode::UnsupportedInstruction:
    case ErrorCode::UnsupportedOperandForm:
    case ErrorCode::Unsupported: return FailureCategory::UnsupportedInstruction;
    case ErrorCode::InvalidBranchTarget:
    case ErrorCode::MisalignedInstructionAddress:
    case ErrorCode::NonExecutableAddress:
    case ErrorCode::InvalidGuestAddress: return FailureCategory::InvalidGuestAddress;
    case ErrorCode::AnalysisBudgetExceeded:
    case ErrorCode::AnalysisInstructionLimitExceeded:
    case ErrorCode::AnalysisBlockLimitExceeded:
    case ErrorCode::AnalysisWorklistLimitExceeded: return FailureCategory::AnalysisBudgetExceeded;
    case ErrorCode::InvalidControlFlow:
    case ErrorCode::AnalysisScopeViolation: return FailureCategory::InvalidCFG;
    case ErrorCode::FunctionBoundaryConflict: return FailureCategory::FunctionBoundaryConflict;
    case ErrorCode::IrVerificationFailed: return FailureCategory::IRVerificationFailure;
    case ErrorCode::MissingImportBinding: return FailureCategory::MissingImportBinding;
    case ErrorCode::RuntimeBoundary: return FailureCategory::RuntimeBoundary;
    case ErrorCode::CodegenFailure:
    case ErrorCode::LlvmVerificationFailed:
    case ErrorCode::JitCompilationFailed: return FailureCategory::CodegenFailure;
    default: return FailureCategory::InvalidCFG;
    }
}

[[nodiscard]] Error budget_error(std::string message)
{
    return make_error(ErrorCode::AnalysisBudgetExceeded, std::move(message));
}

[[nodiscard]] bool checked_size_add(std::size_t left, std::size_t right,
                                    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] Result<void> validate_input(const ModuleAnalysisInput& input,
                                           const FunctionMapOptions& options)
{
    if (input.memory == nullptr)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery requires guest memory"));
    }
    if (input.identity.module.empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery requires a logical module name"));
    }
    if (input.identity.executable_ranges.empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery requires executable ranges"));
    }
    const auto& budgets = options.budgets;
    if (budgets.max_functions == 0U || budgets.max_blocks == 0U || budgets.max_instructions == 0U ||
        budgets.max_edges == 0U || budgets.max_seeds == 0U || budgets.max_bytes_analyzed == 0U ||
        budgets.max_boundary_finalization_passes == 0U)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery budgets must be non-zero"));
    }

    GuestAddress previous_end = 0U;
    bool first = true;
    for (const auto& range : input.identity.executable_ranges)
    {
        if (range.size == 0U || (range.base & 0x3U) != 0U || (range.size & 0x3U) != 0U)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "executable range must be non-empty and AArch64 aligned"));
        }
        const auto end = checked_add_u64(range.base, range.size);
        if (!end)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "executable range overflows guest address space"));
        }
        if (!first && range.base < previous_end)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidFormat, "executable ranges overlap or are not sorted"));
        }
        const auto executable = input.memory->is_executable(range.base, range.size);
        if (!executable)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "executable range " + hex_address(range.base) + " cannot be validated: " +
                    executable.error().message));
        }
        if (!executable.value())
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "executable range " + hex_address(range.base) + " is not executable"));
        }
        previous_end = end.value();
        first = false;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validate_seed(const ModuleAnalysisInput& input,
                                          const FunctionSeed& seed)
{
    const auto canonical = seed.canonical_entry.value_or(seed.entry);
    for (const auto address : {seed.entry, canonical})
    {
        if ((address & 0x3U) != 0U)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function seed " + hex_address(address) + " is not AArch64 aligned"));
        }
        if (!contains_any(input.identity.executable_ranges, address, 4U))
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function seed " + hex_address(address) + " is outside executable ranges"));
        }
        const auto executable = input.memory->is_executable(address, 4U);
        if (!executable || !executable.value())
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function seed " + hex_address(address) + " is not executable"));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> add_evidence(FunctionRecord& record, const FunctionSeed& seed)
{
    if (std::find(record.entries.begin(), record.entries.end(), seed.entry) == record.entries.end())
    {
        record.entries.push_back(seed.entry);
    }
    record.evidence.push_back(DiscoveryEvidence{seed.source, seed.confidence, seed.entry, seed.note});
    if (seed.name)
    {
        if (!record.name || record.name->empty() || confidence_rank(seed.confidence) >
                                                    confidence_rank(record.confidence))
        {
            record.name = seed.name;
        }
    }
    if (confidence_rank(seed.confidence) > confidence_rank(record.confidence))
    {
        record.confidence = seed.confidence;
        record.primary_source = seed.source;
    }
    if (is_boundary_worthy_function_seed(seed))
    {
        record.entry_trust_status = FunctionEntryTrustStatus::Trusted;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> add_seed(std::map<GuestAddress, FunctionRecord>& records,
                                    std::set<GuestAddress>& pending,
                                    const ModuleAnalysisInput& input, const FunctionSeed& seed,
                                    const AnalysisBudgets& budgets, std::size_t& seed_count)
{
    const auto valid = validate_seed(input, seed);
    if (!valid)
    {
        return valid;
    }
    const auto canonical = seed.canonical_entry.value_or(seed.entry);
    auto found = records.find(canonical);
    if (found == records.end())
    {
        if (records.size() >= budgets.max_functions)
        {
            return Result<void>::failure(
                budget_error("function discovery exceeded the function budget"));
        }
        FunctionRecord record;
        record.module = input.identity.module;
        record.synthetic_id = synthetic_id(canonical);
        record.canonical_entry = canonical;
        record.primary_source = seed.source;
        record.confidence = seed.confidence;
        record.entry_trust_status = is_boundary_worthy_function_seed(seed)
                                        ? FunctionEntryTrustStatus::Trusted
                                        : FunctionEntryTrustStatus::Candidate;
        record.entries.push_back(canonical);
        if (seed.entry != canonical) record.entries.push_back(seed.entry);
        record.translation_status = TranslationStatus::Discovered;
        found = records.emplace(canonical, std::move(record)).first;
    }
    const auto evidence = add_evidence(found->second, seed);
    if (!evidence)
    {
        return evidence;
    }
    if (pending.insert(canonical).second)
    {
        if (seed_count >= budgets.max_seeds)
        {
            return Result<void>::failure(budget_error("function discovery exceeded the seed budget"));
        }
        ++seed_count;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> populate_ownership(
    FunctionRecord& record, const std::vector<GuestAddressRange>& executable_ranges)
{
    if (!record.cfg || record.cfg->blocks.empty())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidControlFlow, "cannot derive a function range from an empty CFG"));
    }
    std::vector<GuestAddress> instruction_addresses;
    instruction_addresses.reserve(record.cfg->instruction_count);
    for (const auto& [address, block] : record.cfg->blocks)
    {
        (void)address;
        for (const auto& instruction : block.instructions)
        {
            instruction_addresses.push_back(instruction.address);
        }
    }
    const auto normalized = normalize_code_ranges(instruction_addresses);
    if (!normalized)
    {
        return Result<void>::failure(normalized.error());
    }
    if (normalized.value().empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidControlFlow, "function CFG has no valid instruction range"));
    }
    for (const auto& range : normalized.value())
    {
        if (!contains_any(executable_ranges, range.base, range.size))
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function ownership range " + hex_address(range.base) +
                    " is outside executable memory"));
        }
    }
    record.owned_code_ranges = normalized.value();
    record.range_begin = record.owned_code_ranges.front().base;
    const auto envelope_end = checked_add_u64(record.owned_code_ranges.back().base,
                                               record.owned_code_ranges.back().size);
    if (!envelope_end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress, "function ownership envelope overflows"));
    }
    record.range_end = envelope_end.value();
    record.direct_calls.clear();
    record.indirect_calls.clear();
    for (const auto& call : record.cfg->calls)
    {
        if (call.kind == CallKind::Direct && call.target)
        {
            record.direct_calls.push_back(call.target.value());
        }
        else if (call.kind == CallKind::Indirect)
        {
            record.indirect_calls.push_back(call);
        }
    }
    record.unresolved_control_flow = record.cfg->unresolved;
    std::sort(record.direct_calls.begin(), record.direct_calls.end());
    record.direct_calls.erase(std::unique(record.direct_calls.begin(), record.direct_calls.end()),
                              record.direct_calls.end());
    std::sort(record.indirect_calls.begin(), record.indirect_calls.end(),
              [](const CallSite& left, const CallSite& right) {
                  if (left.address != right.address) return left.address < right.address;
                  if (left.target.has_value() != right.target.has_value())
                  {
                      return left.target.has_value() < right.target.has_value();
                  }
                  if (left.target && right.target && left.target.value() != right.target.value())
                  {
                      return left.target.value() < right.target.value();
                  }
                  const auto left_register = left.register_target
                                                 ? static_cast<unsigned int>(left.register_target->index)
                                                 : 256U;
                  const auto right_register = right.register_target
                                                  ? static_cast<unsigned int>(right.register_target->index)
                                                  : 256U;
                  return left_register < right_register;
              });
    return Result<void>::success();
}

[[nodiscard]] Result<FunctionBoundaryConflict> make_conflict(const FunctionRecord& first,
                                                             const FunctionRecord& second,
                                                             std::string module)
{
    const FunctionRecord* normalized_first = &first;
    const FunctionRecord* normalized_second = &second;
    if (normalized_second->canonical_entry < normalized_first->canonical_entry)
    {
        std::swap(normalized_first, normalized_second);
    }
    const auto overlap = intersect_owned_ranges(normalized_first->owned_code_ranges,
                                                 normalized_second->owned_code_ranges);
    if (!overlap)
    {
        return Result<FunctionBoundaryConflict>::failure(overlap.error());
    }
    if (overlap.value().empty())
    {
        return Result<FunctionBoundaryConflict>::failure(make_error(
            ErrorCode::FunctionBoundaryConflict,
            "cannot create a function conflict without precise instruction overlap"));
    }
    return Result<FunctionBoundaryConflict>::success(FunctionBoundaryConflict{
        std::move(module),
        normalized_first->canonical_entry,
        normalized_second->canonical_entry,
        GuestAddressRange{normalized_first->range_begin,
                          normalized_first->range_end - normalized_first->range_begin},
        GuestAddressRange{normalized_second->range_begin,
                          normalized_second->range_end - normalized_second->range_begin},
        overlap.value(),
        normalized_first->primary_source,
        normalized_second->primary_source,
        normalized_first->confidence,
        normalized_second->confidence,
        "unresolved; manual review required"});
}

[[nodiscard]] bool ranges_overlap(const FunctionRecord& left,
                                  const FunctionRecord& right) noexcept
{
    return owned_ranges_overlap(left.owned_code_ranges, right.owned_code_ranges);
}

} // namespace

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    std::span<const GuestAddress> instruction_addresses)
{
    std::vector<GuestAddress> addresses(instruction_addresses.begin(), instruction_addresses.end());
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());

    std::vector<GuestAddressRange> ranges;
    ranges.reserve(addresses.size());
    for (const auto address : addresses)
    {
        if ((address & 0x3U) != 0U)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "owned instruction address " + hex_address(address) +
                    " is not AArch64 instruction aligned"));
        }
        const auto end = checked_add_u64(address, 4U);
        if (!end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "owned instruction address " + hex_address(address) + " overflows"));
        }
        if (ranges.empty())
        {
            ranges.push_back(GuestAddressRange{address, 4U});
            continue;
        }

        auto& previous = ranges.back();
        const auto previous_end = checked_add_u64(previous.base, previous.size);
        if (!previous_end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "owned code range overflows"));
        }
        if (address <= previous_end.value())
        {
            if (end.value() > previous_end.value())
            {
                previous.size = end.value() - previous.base;
            }
        }
        else
        {
            ranges.push_back(GuestAddressRange{address, 4U});
        }
    }
    return Result<std::vector<GuestAddressRange>>::success(std::move(ranges));
}

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    const std::vector<GuestAddress>& instruction_addresses)
{
    return normalize_code_ranges(
        std::span<const GuestAddress>(instruction_addresses.data(), instruction_addresses.size()));
}

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    std::span<const GuestAddressRange> input_ranges)
{
    std::vector<GuestAddressRange> ranges(input_ranges.begin(), input_ranges.end());
    std::sort(ranges.begin(), ranges.end(), [](const GuestAddressRange& left,
                                               const GuestAddressRange& right) {
        if (left.base != right.base) return left.base < right.base;
        return left.size < right.size;
    });

    std::vector<GuestAddressRange> normalized;
    normalized.reserve(ranges.size());
    for (const auto& range : ranges)
    {
        if (range.size == 0U || (range.base & 0x3U) != 0U || (range.size & 0x3U) != 0U)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "code range must be non-empty and AArch64 aligned"));
        }
        const auto end = checked_add_u64(range.base, range.size);
        if (!end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "code range overflows the guest address space"));
        }
        if (normalized.empty())
        {
            normalized.push_back(range);
            continue;
        }
        auto& previous = normalized.back();
        const auto previous_end = checked_add_u64(previous.base, previous.size);
        if (!previous_end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "normalized code range overflows"));
        }
        if (range.base <= previous_end.value())
        {
            if (end.value() > previous_end.value())
            {
                previous.size = end.value() - previous.base;
            }
        }
        else
        {
            normalized.push_back(range);
        }
    }
    return Result<std::vector<GuestAddressRange>>::success(std::move(normalized));
}

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    const std::vector<GuestAddressRange>& input_ranges)
{
    return normalize_code_ranges(
        std::span<const GuestAddressRange>(input_ranges.data(), input_ranges.size()));
}

bool owned_ranges_overlap(const std::vector<GuestAddressRange>& left,
                          const std::vector<GuestAddressRange>& right) noexcept
{
    std::size_t left_index = 0U;
    std::size_t right_index = 0U;
    while (left_index < left.size() && right_index < right.size())
    {
        const auto left_end = checked_add_u64(left[left_index].base, left[left_index].size);
        const auto right_end = checked_add_u64(right[right_index].base, right[right_index].size);
        if (!left_end || !right_end)
        {
            return false;
        }
        if (left[left_index].base < right_end.value() &&
            right[right_index].base < left_end.value())
        {
            return true;
        }
        if (left_end.value() <= right_end.value())
        {
            ++left_index;
        }
        else
        {
            ++right_index;
        }
    }
    return false;
}

Result<std::vector<GuestAddressRange>> intersect_owned_ranges(
    const std::vector<GuestAddressRange>& left, const std::vector<GuestAddressRange>& right)
{
    const auto normalized_left = normalize_code_ranges(left);
    if (!normalized_left)
    {
        return Result<std::vector<GuestAddressRange>>::failure(normalized_left.error());
    }
    const auto normalized_right = normalize_code_ranges(right);
    if (!normalized_right)
    {
        return Result<std::vector<GuestAddressRange>>::failure(normalized_right.error());
    }

    std::vector<GuestAddressRange> intersections;
    std::size_t left_index = 0U;
    std::size_t right_index = 0U;
    while (left_index < normalized_left.value().size() &&
           right_index < normalized_right.value().size())
    {
        const auto& left_range = normalized_left.value()[left_index];
        const auto& right_range = normalized_right.value()[right_index];
        const auto left_end = checked_add_u64(left_range.base, left_range.size);
        const auto right_end = checked_add_u64(right_range.base, right_range.size);
        if (!left_end || !right_end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "owned range intersection overflows"));
        }
        const auto begin = std::max(left_range.base, right_range.base);
        const auto end = std::min(left_end.value(), right_end.value());
        if (begin < end)
        {
            intersections.push_back(GuestAddressRange{begin, end - begin});
        }
        if (left_end.value() <= right_end.value())
        {
            ++left_index;
        }
        else
        {
            ++right_index;
        }
    }
    return Result<std::vector<GuestAddressRange>>::success(std::move(intersections));
}

Result<memory::GuestSize> precise_owned_byte_count(
    const std::vector<GuestAddressRange>& ranges)
{
    memory::GuestSize total = 0U;
    const auto normalized = normalize_code_ranges(ranges);
    if (!normalized)
    {
        return Result<memory::GuestSize>::failure(normalized.error());
    }
    for (const auto& range : normalized.value())
    {
        if (range.size > std::numeric_limits<memory::GuestSize>::max() - total)
        {
            return Result<memory::GuestSize>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "precise owned-byte count overflows"));
        }
        total += range.size;
    }
    return Result<memory::GuestSize>::success(total);
}

bool function_owns_address(const FunctionRecord& function, GuestAddress address) noexcept
{
    for (const auto& range : function.owned_code_ranges)
    {
        const auto end = checked_add_u64(range.base, range.size);
        if (end && address >= range.base && address < end.value() &&
            ((address - range.base) & 0x3U) == 0U)
        {
            return true;
        }
    }
    return false;
}

bool is_boundary_worthy_function_seed(const FunctionSeed& seed) noexcept
{
    if (seed.source == FunctionDiscoverySource::ObservedIndirectTarget)
    {
        return true;
    }
    if (seed.source == FunctionDiscoverySource::TextStartCandidate ||
        seed.source == FunctionDiscoverySource::Heuristic)
    {
        return false;
    }
    if (seed.source == FunctionDiscoverySource::ManualOverride ||
        seed.confidence == FunctionConfidence::Manual)
    {
        return true;
    }
    return seed.confidence == FunctionConfidence::Confirmed ||
           seed.confidence == FunctionConfidence::High;
}

std::string_view function_discovery_source_name(FunctionDiscoverySource source) noexcept
{
    switch (source)
    {
    case FunctionDiscoverySource::ModuleEntry: return "module_entry";
    case FunctionDiscoverySource::TextStartCandidate: return "text_start_candidate";
    case FunctionDiscoverySource::DynamicSymbol: return "dynamic_symbol";
    case FunctionDiscoverySource::Export: return "export";
    case FunctionDiscoverySource::DirectCall: return "direct_call";
    case FunctionDiscoverySource::RelocationReference: return "relocation_reference";
    case FunctionDiscoverySource::AnalystSeed: return "analyst_seed";
    case FunctionDiscoverySource::ManualOverride: return "manual_override";
    case FunctionDiscoverySource::JumpTable: return "jump_table";
    case FunctionDiscoverySource::ObservedIndirectTarget: return "observed_indirect_target";
    case FunctionDiscoverySource::Heuristic: return "heuristic";
    }
    return "unknown";
}

std::string_view entry_point_kind_name(EntryPointKind kind) noexcept
{
    switch (kind)
    {
    case EntryPointKind::TextStartCandidate: return "text_start_candidate";
    case EntryPointKind::DynamicInit: return "dynamic_init";
    case EntryPointKind::DynamicFini: return "dynamic_fini";
    case EntryPointKind::VerifiedProcessEntry: return "verified_process_entry";
    case EntryPointKind::AnalystOverride: return "analyst_override";
    }
    return "unknown";
}

std::string_view function_confidence_name(FunctionConfidence confidence) noexcept
{
    switch (confidence)
    {
    case FunctionConfidence::Confirmed: return "confirmed";
    case FunctionConfidence::High: return "high";
    case FunctionConfidence::Medium: return "medium";
    case FunctionConfidence::Low: return "low";
    case FunctionConfidence::Manual: return "manual";
    case FunctionConfidence::Conflict: return "conflict";
    }
    return "unknown";
}

std::string_view translation_status_name(TranslationStatus status) noexcept
{
    switch (status)
    {
    case TranslationStatus::Discovered: return "discovered";
    case TranslationStatus::Analyzed: return "analyzed";
    case TranslationStatus::Lifted: return "lifted";
    case TranslationStatus::Verified: return "verified";
    case TranslationStatus::Lowerable: return "lowerable";
    case TranslationStatus::Translated: return "translated";
    case TranslationStatus::Unsupported: return "unsupported";
    case TranslationStatus::Excluded: return "excluded";
    case TranslationStatus::Failed: return "failed";
    case TranslationStatus::Conflict: return "conflict";
    }
    return "unknown";
}

std::string_view function_entry_trust_status_name(FunctionEntryTrustStatus status) noexcept
{
    switch (status)
    {
    case FunctionEntryTrustStatus::Candidate: return "candidate";
    case FunctionEntryTrustStatus::Trusted: return "trusted";
    case FunctionEntryTrustStatus::Conflict: return "conflict";
    }
    return "candidate";
}

std::string_view failure_category_name(FailureCategory category) noexcept
{
    switch (category)
    {
    case FailureCategory::DecodeFailure: return "decode_failure";
    case FailureCategory::InvalidInstruction: return "invalid_instruction";
    case FailureCategory::InvalidCFG: return "invalid_cfg";
    case FailureCategory::UnsupportedInstruction: return "unsupported_instruction";
    case FailureCategory::UnsupportedSemantic: return "unsupported_semantic";
    case FailureCategory::UnresolvedIndirectFlow: return "unresolved_indirect_flow";
    case FailureCategory::FunctionBoundaryConflict: return "function_boundary_conflict";
    case FailureCategory::InvalidGuestAddress: return "invalid_guest_address";
    case FailureCategory::AnalysisBudgetExceeded: return "analysis_budget_exceeded";
    case FailureCategory::IRVerificationFailure: return "ir_verification_failure";
    case FailureCategory::MissingImportBinding: return "missing_import_binding";
    case FailureCategory::RuntimeBoundary: return "runtime_boundary";
    case FailureCategory::CodegenFailure: return "codegen_failure";
    }
    return "unknown";
}

const FunctionRecord* FinalizedFunctionMap::find(GuestAddress entry) const noexcept
{
    const auto found = std::lower_bound(
        functions_.begin(), functions_.end(), entry,
        [](const FunctionRecord& function, GuestAddress value) {
            return function.canonical_entry < value;
        });
    if (found != functions_.end() && found->canonical_entry == entry)
    {
        return &*found;
    }
    for (const auto& function : functions_)
    {
        if (std::binary_search(function.entries.begin(), function.entries.end(), entry))
        {
            return &function;
        }
    }
    return nullptr;
}

std::vector<const FunctionRecord*> FinalizedFunctionMap::find_owners(
    GuestAddress address) const
{
    std::vector<const FunctionRecord*> owners;
    for (const auto& function : functions_)
    {
        if (function_owns_address(function, address))
        {
            owners.push_back(&function);
        }
    }
    return owners;
}

Result<FinalizedFunctionMap> FunctionMapBuilder::build(const ModuleAnalysisInput& input,
                                                        const FunctionMapOptions& options)
{
    const auto valid_input = validate_input(input, options);
    if (!valid_input)
    {
        return Result<FinalizedFunctionMap>::failure(valid_input.error());
    }

    std::map<GuestAddress, FunctionRecord> records;
    std::set<GuestAddress> pending;
    std::set<GuestAddress> processed;
    std::set<GuestAddress> known_function_entries = options.cfg.known_function_entries;
    std::size_t seed_count = 0U;
    for (const auto& seed : input.seeds)
    {
        const auto added = add_seed(records, pending, input, seed, options.budgets, seed_count);
        if (!added)
        {
            return Result<FinalizedFunctionMap>::failure(added.error());
        }
        if (is_boundary_worthy_function_seed(seed))
        {
            known_function_entries.insert(seed.canonical_entry.value_or(seed.entry));
            known_function_entries.insert(seed.entry);
        }
    }
    if (records.empty())
    {
        return Result<FinalizedFunctionMap>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery requires at least one seed"));
    }

    AnalysisOptions cfg_options = options.cfg;
    cfg_options.max_instructions = std::min(cfg_options.max_instructions,
                                            options.budgets.max_instructions);
    cfg_options.max_basic_blocks = std::min(cfg_options.max_basic_blocks, options.budgets.max_blocks);
    cfg_options.max_pending_targets = std::min(cfg_options.max_pending_targets,
                                               options.budgets.max_seeds);
    if (!cfg_options.allowed_code_range && input.identity.executable_ranges.size() == 1U)
    {
        cfg_options.allowed_code_range = input.identity.executable_ranges.front();
    }
    cfg_options.known_function_entries = known_function_entries;

    std::size_t analyzed_instructions = 0U;
    std::size_t analyzed_blocks = 0U;
    std::size_t analyzed_edges = 0U;
    memory::GuestSize analyzed_bytes = 0U;
    const auto account_graph = [&](const ControlFlowGraph& graph) -> Result<void> {
        std::size_t next_instructions = 0U;
        std::size_t next_blocks = 0U;
        if (!checked_size_add(analyzed_instructions, graph.instruction_count, next_instructions) ||
            !checked_size_add(analyzed_blocks, graph.blocks.size(), next_blocks))
        {
            return Result<void>::failure(
                budget_error("function discovery host-size budget overflow"));
        }

        std::size_t graph_edges = graph.unresolved.size();
        for (const auto& [unused, block] : graph.blocks)
        {
            (void)unused;
            std::size_t block_edges = 0U;
            if (!checked_size_add(block.successors.size(), block.calls.size(), block_edges) ||
                !checked_size_add(graph_edges, block_edges, graph_edges))
            {
                return Result<void>::failure(
                    budget_error("function discovery edge budget overflow"));
            }
        }
        std::size_t next_edges = 0U;
        if (!checked_size_add(analyzed_edges, graph_edges, next_edges))
        {
            return Result<void>::failure(
                budget_error("function discovery edge budget overflow"));
        }
        if (graph.instruction_count >
            static_cast<std::size_t>(std::numeric_limits<memory::GuestSize>::max() / 4U))
        {
            return Result<void>::failure(
                budget_error("function CFG byte count overflows the guest size type"));
        }
        const auto graph_bytes = static_cast<memory::GuestSize>(graph.instruction_count) * 4U;
        if (analyzed_bytes > std::numeric_limits<memory::GuestSize>::max() - graph_bytes)
        {
            return Result<void>::failure(budget_error("function discovery byte budget overflow"));
        }
        const auto next_bytes = analyzed_bytes + graph_bytes;
        if (next_instructions > options.budgets.max_instructions ||
            next_blocks > options.budgets.max_blocks || next_edges > options.budgets.max_edges ||
            next_bytes > options.budgets.max_bytes_analyzed)
        {
            return Result<void>::failure(
                budget_error("function discovery exceeded a module analysis budget"));
        }
        analyzed_instructions = next_instructions;
        analyzed_blocks = next_blocks;
        analyzed_edges = next_edges;
        analyzed_bytes = next_bytes;
        return Result<void>::success();
    };
    while (!pending.empty())
    {
        const auto entry = *pending.begin();
        pending.erase(pending.begin());
        if (!processed.insert(entry).second)
        {
            continue;
        }
        auto record = records.find(entry);
        if (record == records.end())
        {
            return Result<FinalizedFunctionMap>::failure(
                make_error(ErrorCode::InvalidFormat, "function discovery lost a pending seed"));
        }

        const auto graph = analyze_control_flow(*input.memory, entry, cfg_options);
        if (!graph)
        {
            record->second.translation_status = TranslationStatus::Failed;
            record->second.diagnostics.push_back(FunctionDiagnostic{
                category_for(graph.error().code), graph.error().code, entry, std::nullopt,
                graph.error().message});
            if (!options.continue_after_function_failure)
            {
                return Result<FinalizedFunctionMap>::failure(graph.error());
            }
            continue;
        }

        const auto& graph_value = graph.value();
        const auto accounted = account_graph(graph_value);
        if (!accounted)
        {
            return Result<FinalizedFunctionMap>::failure(accounted.error());
        }

        record->second.cfg = graph_value;
        const auto range = populate_ownership(record->second, input.identity.executable_ranges);
        if (!range)
        {
            record->second.translation_status = TranslationStatus::Failed;
            record->second.diagnostics.push_back(FunctionDiagnostic{
                category_for(range.error().code), range.error().code, entry, std::nullopt,
                range.error().message});
            if (!options.continue_after_function_failure)
            {
                return Result<FinalizedFunctionMap>::failure(range.error());
            }
            continue;
        }
        record->second.translation_status = TranslationStatus::Analyzed;

        for (const auto target : record->second.direct_calls)
        {
            if (contains_any(input.identity.executable_ranges, target, 4U))
            {
                const FunctionSeed seed{target, FunctionDiscoverySource::DirectCall,
                                        FunctionConfidence::High, std::nullopt, std::nullopt,
                                        "validated direct BL target"};
                const auto added = add_seed(records, pending, input, seed, options.budgets,
                                            seed_count);
                if (!added)
                {
                    return Result<FinalizedFunctionMap>::failure(added.error());
                }
                known_function_entries.insert(target);
                cfg_options.known_function_entries = known_function_entries;
            }
        }
    }

    // Discovery deliberately permits enough ordinary CFG traversal to expose
    // direct BL evidence. Once that fixed point is known, re-analyze every
    // successful function with the complete strong-entry set so no stale CFG
    // can retain ownership across a later-discovered function boundary.
    for (std::size_t pass = 0U; pass < options.budgets.max_boundary_finalization_passes; ++pass)
    {
        bool boundary_set_changed = false;
        cfg_options.known_function_entries = known_function_entries;
        for (auto& [entry, record] : records)
        {
            if (!record.cfg && record.translation_status != TranslationStatus::Discovered)
            {
                continue;
            }
            const auto graph = analyze_control_flow(*input.memory, entry, cfg_options);
            if (!graph)
            {
                record.cfg.reset();
                record.owned_code_ranges.clear();
                record.range_begin = 0U;
                record.range_end = 0U;
                record.direct_calls.clear();
                record.indirect_calls.clear();
                record.unresolved_control_flow.clear();
                record.translation_status = TranslationStatus::Failed;
                record.diagnostics.push_back(FunctionDiagnostic{
                    category_for(graph.error().code), graph.error().code, entry, std::nullopt,
                    "boundary-aware finalization: " + graph.error().message});
                continue;
            }
            const auto accounted = account_graph(graph.value());
            if (!accounted)
            {
                return Result<FinalizedFunctionMap>::failure(accounted.error());
            }
            record.cfg = graph.value();
            const auto range = populate_ownership(record, input.identity.executable_ranges);
            if (!range)
            {
                record.translation_status = TranslationStatus::Failed;
                record.diagnostics.push_back(FunctionDiagnostic{
                    category_for(range.error().code), range.error().code, entry, std::nullopt,
                    range.error().message});
                continue;
            }
            record.translation_status = TranslationStatus::Analyzed;
            for (const auto target : record.direct_calls)
            {
                if (!contains_any(input.identity.executable_ranges, target, 4U))
                {
                    continue;
                }
                const FunctionSeed seed{target, FunctionDiscoverySource::DirectCall,
                                        FunctionConfidence::High, std::nullopt, std::nullopt,
                                        "validated direct BL target"};
                const auto added = add_seed(records, pending, input, seed, options.budgets,
                                            seed_count);
                if (!added)
                {
                    return Result<FinalizedFunctionMap>::failure(added.error());
                }
                if (known_function_entries.insert(target).second)
                {
                    boundary_set_changed = true;
                }
            }
        }
        pending.clear();
        if (!boundary_set_changed)
        {
            break;
        }
        if (pass + 1U == options.budgets.max_boundary_finalization_passes)
        {
            return Result<FinalizedFunctionMap>::failure(budget_error(
                "function boundary finalization did not reach a fixed point within its budget"));
        }
    }

    FinalizedFunctionMap result;
    result.identity_ = input.identity;
    for (const auto& seed : input.seeds)
    {
        if (seed.source != FunctionDiscoverySource::ModuleEntry ||
            seed.confidence != FunctionConfidence::Confirmed)
        {
            continue;
        }
        const auto address = seed.canonical_entry.value_or(seed.entry);
        const auto already_recorded = std::any_of(
            result.identity_.entry_points.begin(), result.identity_.entry_points.end(),
            [address](const EntryPointEvidence& evidence) {
                return evidence.kind == EntryPointKind::VerifiedProcessEntry &&
                       evidence.address == address;
            });
        if (!already_recorded)
        {
            result.identity_.entry_points.push_back(EntryPointEvidence{
                address, EntryPointKind::VerifiedProcessEntry, "confirmed module-entry seed",
                FunctionConfidence::Confirmed, true,
                "verified runtime entry supplied by the launch model"});
        }
    }
    std::sort(result.identity_.entry_points.begin(), result.identity_.entry_points.end(),
              [](const EntryPointEvidence& left, const EntryPointEvidence& right) {
                  if (left.address != right.address) return left.address < right.address;
                  if (left.kind != right.kind) return left.kind < right.kind;
                  if (left.provenance != right.provenance) return left.provenance < right.provenance;
                  if (left.confidence != right.confidence) return left.confidence < right.confidence;
                  if (left.verified_runtime_entry != right.verified_runtime_entry)
                  {
                      return left.verified_runtime_entry < right.verified_runtime_entry;
                  }
                  return left.note < right.note;
              });
    result.functions_.reserve(records.size());
    for (auto& [unused, record] : records)
    {
        (void)unused;
        std::sort(record.entries.begin(), record.entries.end());
        record.entries.erase(std::unique(record.entries.begin(), record.entries.end()),
                             record.entries.end());
        std::sort(record.evidence.begin(), record.evidence.end(),
                  [](const DiscoveryEvidence& left, const DiscoveryEvidence& right) {
                      return std::tie(left.entry, left.source, left.confidence, left.note) <
                             std::tie(right.entry, right.source, right.confidence, right.note);
                  });
        if (!record.name)
        {
            record.name = record.synthetic_id;
        }
        result.functions_.push_back(std::move(record));
    }
    std::sort(result.functions_.begin(), result.functions_.end(),
              [](const FunctionRecord& left, const FunctionRecord& right) {
                  return left.canonical_entry < right.canonical_entry;
              });

    // Compute the complete conflict set only after all fixed-point discovery
    // and range population has finished. The result functions are already in
    // canonical-entry order, so each pair is considered exactly once and its
    // identity is independent of seed/discovery order.
    for (std::size_t left = 0U; left < result.functions_.size(); ++left)
    {
        if (!result.functions_[left].cfg)
        {
            continue;
        }
        for (std::size_t right = left + 1U; right < result.functions_.size(); ++right)
        {
            if (!result.functions_[right].cfg ||
                !ranges_overlap(result.functions_[left], result.functions_[right]))
            {
                continue;
            }
            const auto conflict = make_conflict(result.functions_[left], result.functions_[right],
                                                input.identity.module);
            if (!conflict)
            {
                return Result<FinalizedFunctionMap>::failure(conflict.error());
            }
            result.conflicts_.push_back(conflict.value());
        }
    }
    // Mark records only after all conflict records have captured the original
    // discovery confidence. The evidence vector remains the provenance source
    // even after the finalized record receives Conflict status.
    for (const auto& conflict : result.conflicts_)
    {
        const auto first = std::lower_bound(
            result.functions_.begin(), result.functions_.end(), conflict.first_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        const auto second = std::lower_bound(
            result.functions_.begin(), result.functions_.end(), conflict.second_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        if (first != result.functions_.end() && first->canonical_entry == conflict.first_function)
        {
            first->confidence = FunctionConfidence::Conflict;
            first->entry_trust_status = FunctionEntryTrustStatus::Conflict;
            first->translation_status = TranslationStatus::Conflict;
        }
        if (second != result.functions_.end() &&
            second->canonical_entry == conflict.second_function)
        {
            second->confidence = FunctionConfidence::Conflict;
            second->entry_trust_status = FunctionEntryTrustStatus::Conflict;
            second->translation_status = TranslationStatus::Conflict;
        }
    }
    result.frozen_ = true;
    const auto valid = validate_finalized_function_map(result);
    if (!valid)
    {
        return Result<FinalizedFunctionMap>::failure(valid.error());
    }
    return Result<FinalizedFunctionMap>::success(std::move(result));
}

Result<void> validate_finalized_function_map(const FinalizedFunctionMap& map)
{
    if (!map.frozen_)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidFormat, "function map must be frozen before validation"));
    }
    GuestAddress previous = 0U;
    bool first = true;
    std::set<GuestAddress> all_entries;
    for (const auto& function : map.functions_)
    {
        if (function.module != map.identity_.module || function.entries.empty() ||
            function.synthetic_id.empty())
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidFormat, "function map contains incomplete function identity"));
        }
        if ((function.translation_status == TranslationStatus::Conflict) !=
            (function.entry_trust_status == FunctionEntryTrustStatus::Conflict))
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function map trust status does not match its conflict status"));
        }
        if (!first && function.canonical_entry <= previous)
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidFormat, "function map is not sorted by canonical entry"));
        }
        previous = function.canonical_entry;
        first = false;
        for (const auto entry : function.entries)
        {
            if ((entry & 0x3U) != 0U || !all_entries.insert(entry).second ||
                !contains_any(map.identity_.executable_ranges, entry, 4U))
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidGuestAddress,
                               "function map contains an invalid or duplicate entry"));
            }
        }
        if (function.cfg)
        {
            std::vector<GuestAddress> instruction_addresses;
            instruction_addresses.reserve(function.cfg->instruction_count);
            for (const auto& [unused, block] : function.cfg->blocks)
            {
                (void)unused;
                for (const auto& instruction : block.instructions)
                {
                    instruction_addresses.push_back(instruction.address);
                }
            }
            const auto expected_ranges = normalize_code_ranges(instruction_addresses);
            if (!expected_ranges || expected_ranges.value() != function.owned_code_ranges ||
                function.owned_code_ranges.empty() ||
                function.range_begin != function.owned_code_ranges.front().base)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidGuestAddress,
                    "function precise ownership does not match its decoded CFG"));
            }
            GuestAddress previous_end = 0U;
            bool first_range = true;
            for (const auto& range : function.owned_code_ranges)
            {
                if (range.size == 0U || (range.base & 0x3U) != 0U ||
                    (range.size & 0x3U) != 0U)
                {
                    return Result<void>::failure(make_error(
                        ErrorCode::InvalidGuestAddress,
                        "function ownership contains an empty or misaligned range"));
                }
                const auto end = checked_add_u64(range.base, range.size);
                if (!end || !contains_any(map.identity_.executable_ranges, range.base, range.size) ||
                    (!first_range && range.base <= previous_end))
                {
                    return Result<void>::failure(make_error(
                        ErrorCode::InvalidGuestAddress,
                        "function ownership is outside executable memory or not normalized"));
                }
                previous_end = end.value();
                first_range = false;
            }
            if (function.range_end != previous_end || function.range_end <= function.range_begin)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidGuestAddress,
                    "function envelope does not bound its precise ownership"));
            }
            const auto valid_cfg = validate_control_flow_graph(function.cfg.value());
            if (!valid_cfg)
            {
                return Result<void>::failure(valid_cfg.error());
            }
        }
    }

    std::set<std::pair<GuestAddress, GuestAddress>> recorded_conflicts;
    std::optional<std::pair<GuestAddress, GuestAddress>> previous_conflict;
    for (const auto& conflict : map.conflicts_)
    {
        if (conflict.module != map.identity_.module ||
            conflict.first_function >= conflict.second_function ||
            !recorded_conflicts.emplace(conflict.first_function, conflict.second_function).second ||
            (previous_conflict &&
             *previous_conflict >=
                 std::make_pair(conflict.first_function, conflict.second_function)))
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function map contains a duplicate or non-normalized conflict"));
        }
        previous_conflict =
            std::make_pair(conflict.first_function, conflict.second_function);

        const auto first = std::lower_bound(
            map.functions_.begin(), map.functions_.end(), conflict.first_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        const auto second = std::lower_bound(
            map.functions_.begin(), map.functions_.end(), conflict.second_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        if (first == map.functions_.end() || first->canonical_entry != conflict.first_function ||
            second == map.functions_.end() || second->canonical_entry != conflict.second_function ||
            !first->cfg || !second->cfg ||
            conflict.first_range.base != first->range_begin ||
            conflict.first_range.size != first->range_end - first->range_begin ||
            conflict.second_range.base != second->range_begin ||
            conflict.second_range.size != second->range_end - second->range_begin)
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function map contains a phantom or stale boundary conflict"));
        }
        const auto expected_overlap =
            intersect_owned_ranges(first->owned_code_ranges, second->owned_code_ranges);
        const auto normalized_overlap = normalize_code_ranges(conflict.overlap_ranges);
        if (!expected_overlap || !normalized_overlap || expected_overlap.value().empty() ||
            normalized_overlap.value() != conflict.overlap_ranges ||
            expected_overlap.value() != conflict.overlap_ranges)
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function conflict does not identify the exact normalized ownership overlap"));
        }
    }

    std::set<std::pair<GuestAddress, GuestAddress>> expected_conflicts;
    for (std::size_t left = 0U; left < map.functions_.size(); ++left)
    {
        if (!map.functions_[left].cfg)
        {
            continue;
        }
        for (std::size_t right = left + 1U; right < map.functions_.size(); ++right)
        {
            if (!map.functions_[right].cfg)
            {
                continue;
            }
            if (!ranges_overlap(map.functions_[left], map.functions_[right]))
            {
                continue;
            }
            const auto pair = std::make_pair(map.functions_[left].canonical_entry,
                                             map.functions_[right].canonical_entry);
            expected_conflicts.insert(pair);
            if (recorded_conflicts.find(pair) == recorded_conflicts.end())
            {
                return Result<void>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "overlapping function ranges were not recorded as a conflict"));
            }
            if (map.functions_[left].confidence != FunctionConfidence::Conflict ||
                map.functions_[left].translation_status != TranslationStatus::Conflict ||
                map.functions_[right].confidence != FunctionConfidence::Conflict ||
                map.functions_[right].translation_status != TranslationStatus::Conflict)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "overlapping functions were not marked as conflicting"));
            }
        }
    }
    if (recorded_conflicts != expected_conflicts)
    {
        return Result<void>::failure(make_error(
            ErrorCode::FunctionBoundaryConflict,
            "finalized function map conflict set does not match precise ownership"));
    }
    return Result<void>::success();
}

} // namespace switchrecomp::analysis
