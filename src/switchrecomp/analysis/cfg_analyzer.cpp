#include "switchrecomp/analysis/cfg_analyzer.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <deque>
#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace switchrecomp::analysis
{

namespace
{

using aarch64::ControlFlowKind;
using aarch64::DecodedInstruction;
using aarch64::GuestAddress;
using memory::GuestMemory;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

class GraphBuilder
{
  public:
    GraphBuilder(const GuestMemory& memory, const aarch64::AArch64Decoder& decoder,
                 AnalysisOptions options)
        : memory_(memory), decoder_(decoder), options_(std::move(options))
    {
    }

    [[nodiscard]] Result<ControlFlowGraph> run(GuestAddress entry)
    {
        const auto valid_options = validate_options();
        if (!valid_options)
        {
            return Result<ControlFlowGraph>::failure(valid_options.error());
        }
        if ((entry & 0x3U) != 0U)
        {
            return Result<ControlFlowGraph>::failure(make_error(
                ErrorCode::MisalignedInstructionAddress,
                "CFG entry " + hex_address(entry) + " is not 4-byte aligned"));
        }
        if (options_.allowed_code_range && !contains(options_.allowed_code_range.value(), entry))
        {
            return Result<ControlFlowGraph>::failure(make_error(
                ErrorCode::AnalysisScopeViolation,
                "CFG entry " + hex_address(entry) + " is outside the allowed code range"));
        }
        const auto entry_executable = memory_.is_executable(entry, 4U);
        if (!entry_executable)
        {
            return Result<ControlFlowGraph>::failure(entry_executable.error());
        }
        if (!entry_executable.value())
        {
            return Result<ControlFlowGraph>::failure(make_error(
                ErrorCode::NonExecutableAddress,
                "CFG entry " + hex_address(entry) + " is not executable"));
        }

        graph_.entry = entry;
        leaders_.insert(entry);
        const auto initial_enqueue = enqueue(entry);
        if (!initial_enqueue)
        {
            return Result<ControlFlowGraph>::failure(initial_enqueue.error());
        }
        while (!worklist_.empty())
        {
            const auto start = worklist_.front();
            worklist_.pop_front();
            pending_.erase(start);
            if (graph_.blocks.contains(start))
            {
                continue;
            }
            if (graph_.blocks.size() >= options_.max_basic_blocks)
            {
                return Result<ControlFlowGraph>::failure(make_error(
                    ErrorCode::AnalysisBlockLimitExceeded,
                    "CFG exceeded the maximum basic-block limit"));
            }
            auto block = decode_block(start);
            if (!block)
            {
                return Result<ControlFlowGraph>::failure(block.error());
            }
            const auto published = publish_block(std::move(block.value()));
            if (!published)
            {
                return Result<ControlFlowGraph>::failure(published.error());
            }
        }

        const auto valid_graph = validate_control_flow_graph(graph_);
        if (!valid_graph)
        {
            return Result<ControlFlowGraph>::failure(valid_graph.error());
        }
        return Result<ControlFlowGraph>::success(std::move(graph_));
    }

  private:
    [[nodiscard]] Result<void> validate_options() const
    {
        if (options_.max_instructions == 0U || options_.max_basic_blocks == 0U ||
            options_.max_pending_targets == 0U)
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidArgument, "CFG safety limits must be non-zero"));
        }
        if (options_.allowed_code_range)
        {
            const auto end = checked_add_u64(options_.allowed_code_range->base,
                                              options_.allowed_code_range->size);
            if (!end)
            {
                return Result<void>::failure(make_error(
                    end.error().code, "allowed CFG code range overflows the guest address space"));
            }
            if (options_.allowed_code_range->size == 0U)
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidArgument, "allowed CFG code range is empty"));
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] static bool contains(const GuestAddressRange& range, GuestAddress address)
    {
        const auto end = checked_add_u64(range.base, range.size);
        return end && address >= range.base && address < end.value();
    }

    [[nodiscard]] Result<void> enqueue(GuestAddress address)
    {
        if (pending_.contains(address) || graph_.blocks.contains(address))
        {
            return Result<void>::success();
        }
        if (pending_.size() >= options_.max_pending_targets)
        {
            return Result<void>::failure(make_error(
                ErrorCode::AnalysisWorklistLimitExceeded,
                "CFG exceeded the maximum pending-target limit"));
        }
        pending_.insert(address);
        worklist_.push_back(address);
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> mark_leader(GuestAddress address)
    {
        leaders_.insert(address);
        return split_existing_block(address);
    }

    [[nodiscard]] Result<void> split_existing_block(GuestAddress address)
    {
        auto next = graph_.blocks.upper_bound(address);
        if (next == graph_.blocks.begin())
        {
            return Result<void>::success();
        }
        --next;
        if (next->first == address)
        {
            return Result<void>::success();
        }
        auto& block = next->second;
        std::size_t split_index = 0U;
        while (split_index < block.instructions.size() &&
               block.instructions[split_index].address != address)
        {
            ++split_index;
        }
        if (split_index == block.instructions.size())
        {
            return Result<void>::success();
        }

        BasicBlock suffix;
        suffix.start = address;
        suffix.instructions.assign(block.instructions.begin() +
                                       static_cast<std::ptrdiff_t>(split_index),
                                   block.instructions.end());
        suffix.successors = std::move(block.successors);
        suffix.termination = std::move(block.termination);
        for (const auto& call : block.calls)
        {
            if (call.address >= address)
            {
                suffix.calls.push_back(call);
            }
        }
        block.instructions.erase(block.instructions.begin() + static_cast<std::ptrdiff_t>(split_index),
                                 block.instructions.end());
        block.calls.erase(std::remove_if(block.calls.begin(), block.calls.end(),
                                         [address](const CallSite& call) {
                                             return call.address >= address;
                                         }),
                          block.calls.end());
        block.successors.clear();
        block.termination = "split at " + hex_address(address);
        block.successors.push_back(ControlFlowEdge{block.instructions.back().address,
                                                   address,
                                                   EdgeKind::Fallthrough,
                                                   true});
        graph_.blocks.emplace(address, std::move(suffix));
        return Result<void>::success();
    }

    [[nodiscard]] Result<bool> validate_direct_target(GuestAddress target,
                                                       std::string_view description)
    {
        if ((target & 0x3U) != 0U)
        {
            return Result<bool>::failure(make_error(
                ErrorCode::InvalidBranchTarget,
                std::string(description) + " target " + hex_address(target) +
                    " is not 4-byte aligned"));
        }
        const auto executable = memory_.is_executable(target, 4U);
        if (!executable)
        {
            return Result<bool>::failure(make_error(
                ErrorCode::InvalidBranchTarget,
                std::string(description) + " target " + hex_address(target) +
                    " cannot be validated as executable: " + executable.error().message));
        }
        if (!executable.value())
        {
            return Result<bool>::failure(make_error(
                ErrorCode::InvalidBranchTarget,
                std::string(description) + " target " + hex_address(target) +
                    " is mapped but not executable"));
        }
        const bool internal = !options_.allowed_code_range ||
                              contains(options_.allowed_code_range.value(), target);
        if (internal)
        {
            const auto leader = mark_leader(target);
            if (!leader)
            {
                return Result<bool>::failure(leader.error());
            }
            const auto queued = enqueue(target);
            if (!queued)
            {
                return Result<bool>::failure(queued.error());
            }
        }
        return Result<bool>::success(internal);
    }

    [[nodiscard]] Result<bool> add_fallthrough_edge(BasicBlock& block, GuestAddress source,
                                                    EdgeKind kind)
    {
        const auto target = checked_add_u64(source, 4U);
        if (!target)
        {
            return Result<bool>::failure(target.error());
        }
        const bool inside = !options_.allowed_code_range ||
                            contains(options_.allowed_code_range.value(), target.value());
        if (inside)
        {
            const auto executable = memory_.is_executable(target.value(), 4U);
            if (!executable)
            {
                return Result<bool>::failure(make_error(
                    ErrorCode::InstructionFetchFailed,
                    "fallthrough from " + hex_address(source) +
                        " cannot fetch " + hex_address(target.value()) + ": " +
                        executable.error().message));
            }
            if (!executable.value())
            {
                return Result<bool>::failure(make_error(
                    ErrorCode::InstructionFetchFailed,
                    "fallthrough from " + hex_address(source) + " reaches non-executable " +
                        hex_address(target.value())));
            }
            const auto leader = mark_leader(target.value());
            if (!leader)
            {
                return Result<bool>::failure(leader.error());
            }
            const auto queued = enqueue(target.value());
            if (!queued)
            {
                return Result<bool>::failure(queued.error());
            }
        }
        block.successors.push_back(
            ControlFlowEdge{source, target.value(), kind, inside});
        return Result<bool>::success(inside);
    }

    void add_unresolved(const DecodedInstruction& instruction, std::string reason)
    {
        graph_.unresolved.push_back(UnresolvedControlFlow{instruction.address,
                                                          instruction.control_flow.kind,
                                                          instruction.control_flow.register_target,
                                                          std::move(reason)});
    }

    [[nodiscard]] Result<BasicBlock> decode_block(GuestAddress start)
    {
        BasicBlock block;
        block.start = start;
        GuestAddress current = start;
        while (true)
        {
            if (current != start && leaders_.contains(current))
            {
                const auto edge = add_fallthrough_edge(block, block.instructions.back().address,
                                                       EdgeKind::Fallthrough);
                if (!edge)
                {
                    return Result<BasicBlock>::failure(edge.error());
                }
                block.termination = "leader boundary";
                break;
            }
            if (graph_.instruction_count >= options_.max_instructions)
            {
                return Result<BasicBlock>::failure(make_error(
                    ErrorCode::AnalysisInstructionLimitExceeded,
                    "CFG exceeded the maximum decoded-instruction limit"));
            }
            const auto instruction =
                aarch64::fetch_and_decode(memory_, decoder_, current);
            if (!instruction)
            {
                return Result<BasicBlock>::failure(instruction.error());
            }
            ++graph_.instruction_count;
            block.instructions.push_back(instruction.value());
            const auto& decoded = block.instructions.back();
            const auto kind = decoded.control_flow.kind;
            if (kind == ControlFlowKind::Fallthrough)
            {
                const auto next = checked_add_u64(current, 4U);
                if (!next)
                {
                    return Result<BasicBlock>::failure(next.error());
                }
                if (leaders_.contains(next.value()))
                {
                    const auto edge = add_fallthrough_edge(block, current, EdgeKind::Fallthrough);
                    if (!edge)
                    {
                        return Result<BasicBlock>::failure(edge.error());
                    }
                    block.termination = "leader boundary";
                    break;
                }
                if (options_.allowed_code_range &&
                    !contains(options_.allowed_code_range.value(), next.value()))
                {
                    const auto edge = add_fallthrough_edge(block, current, EdgeKind::Fallthrough);
                    if (!edge)
                    {
                        return Result<BasicBlock>::failure(edge.error());
                    }
                    block.termination = "analysis range boundary";
                    break;
                }
                current = next.value();
                continue;
            }

            if (kind == ControlFlowKind::DirectBranch ||
                kind == ControlFlowKind::ConditionalBranch)
            {
                if (!decoded.control_flow.target)
                {
                    return Result<BasicBlock>::failure(make_error(
                        ErrorCode::InvalidBranchTarget,
                        "direct control-flow instruction at " + hex_address(current) +
                            " has no target"));
                }
                const auto branch = validate_direct_target(
                    decoded.control_flow.target.value(), "direct control-flow");
                if (!branch)
                {
                    return Result<BasicBlock>::failure(branch.error());
                }
                block.successors.push_back(ControlFlowEdge{
                    current,
                    decoded.control_flow.target.value(),
                    kind == ControlFlowKind::ConditionalBranch ? EdgeKind::ConditionalTaken
                                                               : EdgeKind::Branch,
                    branch.value()});
                if (kind == ControlFlowKind::ConditionalBranch)
                {
                    const auto fallthrough =
                        add_fallthrough_edge(block, current, EdgeKind::ConditionalNotTaken);
                    if (!fallthrough)
                    {
                        return Result<BasicBlock>::failure(fallthrough.error());
                    }
                }
                block.termination = std::string(aarch64::control_flow_kind_name(kind));
                break;
            }

            if (kind == ControlFlowKind::DirectCall || kind == ControlFlowKind::IndirectCall)
            {
                CallSite call;
                call.address = current;
                call.kind = kind == ControlFlowKind::DirectCall ? CallKind::Direct
                                                                : CallKind::Indirect;
                call.target = decoded.control_flow.target;
                call.register_target = decoded.control_flow.register_target;
                block.calls.push_back(call);
                graph_.calls.push_back(call);
                const auto fallthrough =
                    add_fallthrough_edge(block, current, EdgeKind::Fallthrough);
                if (!fallthrough)
                {
                    return Result<BasicBlock>::failure(fallthrough.error());
                }
                if (kind == ControlFlowKind::IndirectCall)
                {
                    add_unresolved(decoded, "indirect call target is runtime-computed");
                }
                block.termination = std::string(aarch64::control_flow_kind_name(kind));
                break;
            }

            if (kind == ControlFlowKind::IndirectBranch || kind == ControlFlowKind::Unknown)
            {
                add_unresolved(decoded,
                               kind == ControlFlowKind::IndirectBranch
                                   ? "indirect branch target is runtime-computed"
                                   : "decoder cannot prove the instruction's control-flow behavior");
                block.termination = std::string(aarch64::control_flow_kind_name(kind));
                break;
            }

            block.termination = std::string(aarch64::control_flow_kind_name(kind));
            break;
        }
        return Result<BasicBlock>::success(std::move(block));
    }

    [[nodiscard]] Result<void> publish_block(BasicBlock block)
    {
        std::vector<std::size_t> split_points;
        for (std::size_t index = 1U; index < block.instructions.size(); ++index)
        {
            if (leaders_.contains(block.instructions[index].address))
            {
                split_points.push_back(index);
            }
        }
        split_points.push_back(block.instructions.size());

        std::size_t first = 0U;
        for (const auto end : split_points)
        {
            BasicBlock segment;
            segment.start = block.instructions[first].address;
            segment.instructions.assign(block.instructions.begin() +
                                            static_cast<std::ptrdiff_t>(first),
                                        block.instructions.begin() +
                                            static_cast<std::ptrdiff_t>(end));
            for (const auto& call : block.calls)
            {
                if (call.address >= segment.start &&
                    call.address <= segment.instructions.back().address)
                {
                    segment.calls.push_back(call);
                }
            }
            if (end != block.instructions.size())
            {
                const auto next_start = block.instructions[end].address;
                segment.successors.push_back(ControlFlowEdge{
                    segment.instructions.back().address, next_start, EdgeKind::Fallthrough, true});
                segment.termination = "split at " + hex_address(next_start);
                const auto queued = enqueue(next_start);
                if (!queued)
                {
                    return queued;
                }
            }
            else
            {
                segment.successors = std::move(block.successors);
                segment.termination = std::move(block.termination);
            }
            if (!graph_.blocks.emplace(segment.start, std::move(segment)).second)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidFormat,
                    "CFG attempted to publish two blocks at " + hex_address(block.start)));
            }
            first = end;
        }
        return Result<void>::success();
    }

    const GuestMemory& memory_;
    const aarch64::AArch64Decoder& decoder_;
    AnalysisOptions options_;
    ControlFlowGraph graph_;
    std::set<GuestAddress> leaders_;
    std::set<GuestAddress> pending_;
    std::deque<GuestAddress> worklist_;
};

} // namespace

Result<ControlFlowGraph> analyze_control_flow(const memory::GuestMemory& memory,
                                              GuestAddress entry,
                                              const AnalysisOptions& options)
{
    const auto decoder = aarch64::AArch64Decoder::create();
    if (!decoder)
    {
        return Result<ControlFlowGraph>::failure(decoder.error());
    }
    return analyze_control_flow(memory, *decoder.value(), entry, options);
}

Result<ControlFlowGraph> analyze_control_flow(const memory::GuestMemory& memory,
                                              const aarch64::AArch64Decoder& decoder,
                                              GuestAddress entry,
                                              const AnalysisOptions& options)
{
    return GraphBuilder(memory, decoder, options).run(entry);
}

} // namespace switchrecomp::analysis
