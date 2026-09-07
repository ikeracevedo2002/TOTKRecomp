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
        budgets.max_edges == 0U || budgets.max_seeds == 0U || budgets.max_bytes_analyzed == 0U)
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

[[nodiscard]] Result<void> populate_range(FunctionRecord& record)
{
    if (!record.cfg || record.cfg->blocks.empty())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidControlFlow, "cannot derive a function range from an empty CFG"));
    }
    GuestAddress begin = UINT64_MAX;
    GuestAddress end = 0U;
    for (const auto& [address, block] : record.cfg->blocks)
    {
        begin = std::min(begin, address);
        if (!block.instructions.empty())
        {
            const auto block_end = checked_add_u64(block.instructions.back().address, 4U);
            if (!block_end)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidGuestAddress, "function instruction range overflows"));
            }
            end = std::max(end, block_end.value());
        }
    }
    if (begin == UINT64_MAX || end <= begin)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidControlFlow, "function CFG has no valid instruction range"));
    }
    record.range_begin = begin;
    record.range_end = end;
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

[[nodiscard]] FunctionBoundaryConflict make_conflict(const FunctionRecord& first,
                                                     const FunctionRecord& second,
                                                     std::string module)
{
    return FunctionBoundaryConflict{
        std::move(module),
        first.canonical_entry,
        second.canonical_entry,
        GuestAddressRange{first.range_begin, first.range_end - first.range_begin},
        GuestAddressRange{second.range_begin, second.range_end - second.range_begin},
        first.primary_source,
        second.primary_source,
        first.confidence,
        second.confidence,
        "unresolved; manual review required"};
}

} // namespace

std::string_view function_discovery_source_name(FunctionDiscoverySource source) noexcept
{
    switch (source)
    {
    case FunctionDiscoverySource::ModuleEntry: return "module_entry";
    case FunctionDiscoverySource::DynamicSymbol: return "dynamic_symbol";
    case FunctionDiscoverySource::Export: return "export";
    case FunctionDiscoverySource::DirectCall: return "direct_call";
    case FunctionDiscoverySource::RelocationReference: return "relocation_reference";
    case FunctionDiscoverySource::AnalystSeed: return "analyst_seed";
    case FunctionDiscoverySource::ManualOverride: return "manual_override";
    case FunctionDiscoverySource::JumpTable: return "jump_table";
    case FunctionDiscoverySource::Heuristic: return "heuristic";
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
    std::size_t seed_count = 0U;
    for (const auto& seed : input.seeds)
    {
        const auto added = add_seed(records, pending, input, seed, options.budgets, seed_count);
        if (!added)
        {
            return Result<FinalizedFunctionMap>::failure(added.error());
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

    std::size_t analyzed_instructions = 0U;
    std::size_t analyzed_blocks = 0U;
    std::size_t analyzed_edges = 0U;
    memory::GuestSize analyzed_bytes = 0U;
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
        std::size_t next_instructions = 0U;
        std::size_t next_blocks = 0U;
        if (!checked_size_add(analyzed_instructions, graph_value.instruction_count,
                              next_instructions) ||
            !checked_size_add(analyzed_blocks, graph_value.blocks.size(), next_blocks))
        {
            return Result<FinalizedFunctionMap>::failure(
                budget_error("function discovery host-size budget overflow"));
        }
        std::size_t graph_edges = graph_value.unresolved.size();
        for (const auto& [unused, block] : graph_value.blocks)
        {
            (void)unused;
            std::size_t block_edges = 0U;
            if (!checked_size_add(block.successors.size(), block.calls.size(), block_edges) ||
                !checked_size_add(graph_edges, block_edges, graph_edges))
            {
                return Result<FinalizedFunctionMap>::failure(
                    budget_error("function discovery edge budget overflow"));
            }
        }
        std::size_t next_edges = 0U;
        if (!checked_size_add(analyzed_edges, graph_edges, next_edges))
        {
            return Result<FinalizedFunctionMap>::failure(
                budget_error("function discovery edge budget overflow"));
        }
        if (graph_value.instruction_count >
            static_cast<std::size_t>(std::numeric_limits<memory::GuestSize>::max() / 4U))
        {
            return Result<FinalizedFunctionMap>::failure(
                budget_error("function CFG byte count overflows the guest size type"));
        }
        const auto graph_bytes = static_cast<memory::GuestSize>(graph_value.instruction_count) * 4U;
        const auto next_bytes = analyzed_bytes > std::numeric_limits<memory::GuestSize>::max() - graph_bytes
                                    ? std::numeric_limits<memory::GuestSize>::max()
                                    : analyzed_bytes + graph_bytes;
        if (next_instructions > options.budgets.max_instructions ||
            next_blocks > options.budgets.max_blocks || next_edges > options.budgets.max_edges ||
            next_bytes < analyzed_bytes || next_bytes > options.budgets.max_bytes_analyzed)
        {
            return Result<FinalizedFunctionMap>::failure(
                budget_error("function discovery exceeded a module analysis budget"));
        }
        analyzed_instructions = next_instructions;
        analyzed_blocks = next_blocks;
        analyzed_edges = next_edges;
        analyzed_bytes = next_bytes;

        record->second.cfg = graph_value;
        const auto range = populate_range(record->second);
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
            }
        }
    }

    FinalizedFunctionMap result;
    result.identity_ = input.identity;
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

    std::vector<std::size_t> by_range(result.functions_.size());
    for (std::size_t index = 0U; index < by_range.size(); ++index)
    {
        by_range[index] = index;
    }
    std::sort(by_range.begin(), by_range.end(), [&](std::size_t left, std::size_t right) {
        if (result.functions_[left].range_begin != result.functions_[right].range_begin)
        {
            return result.functions_[left].range_begin < result.functions_[right].range_begin;
        }
        return result.functions_[left].canonical_entry < result.functions_[right].canonical_entry;
    });
    std::vector<std::size_t> active;
    for (const auto index : by_range)
    {
        const auto& current = result.functions_[index];
        if (current.cfg)
        {
            active.erase(std::remove_if(active.begin(), active.end(), [&](std::size_t active_index) {
                            return result.functions_[active_index].range_end <= current.range_begin;
                        }),
                         active.end());
            for (const auto active_index : active)
            {
                const auto& previous = result.functions_[active_index];
                if (previous.range_begin < current.range_end &&
                    current.range_begin < previous.range_end)
                {
                    result.conflicts_.push_back(
                        make_conflict(previous, current, input.identity.module));
                    result.functions_[active_index].confidence = FunctionConfidence::Conflict;
                    result.functions_[active_index].translation_status = TranslationStatus::Conflict;
                    result.functions_[index].confidence = FunctionConfidence::Conflict;
                    result.functions_[index].translation_status = TranslationStatus::Conflict;
                }
            }
            active.push_back(index);
        }
    }
    std::sort(result.conflicts_.begin(), result.conflicts_.end(),
              [](const FunctionBoundaryConflict& left, const FunctionBoundaryConflict& right) {
                  return std::tie(left.first_function, left.second_function) <
                         std::tie(right.first_function, right.second_function);
              });
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
            if (function.range_end <= function.range_begin ||
                !contains_any(map.identity_.executable_ranges, function.range_begin,
                              function.range_end - function.range_begin))
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidGuestAddress, "function range is outside executable memory"));
            }
            const auto valid_cfg = validate_control_flow_graph(function.cfg.value());
            if (!valid_cfg)
            {
                return Result<void>::failure(valid_cfg.error());
            }
        }
    }

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
            const auto overlap = map.functions_[left].range_begin < map.functions_[right].range_end &&
                                 map.functions_[right].range_begin < map.functions_[left].range_end;
            if (!overlap)
            {
                continue;
            }
            const auto conflict = std::find_if(
                map.conflicts_.begin(), map.conflicts_.end(), [&](const auto& item) {
                    return item.first_function == map.functions_[left].canonical_entry &&
                           item.second_function == map.functions_[right].canonical_entry;
                });
            if (conflict == map.conflicts_.end())
            {
                return Result<void>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "overlapping function ranges were not recorded as a conflict"));
            }
        }
    }
    return Result<void>::success();
}

} // namespace switchrecomp::analysis
