#include "switchrecomp/analysis/control_flow_graph.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace switchrecomp::analysis
{

namespace
{

[[nodiscard]] std::string hex_address(aarch64::GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] std::string hex_opcode(std::uint32_t opcode)
{
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << opcode;
    return output.str();
}

} // namespace

std::string_view edge_kind_name(EdgeKind kind) noexcept
{
    switch (kind)
    {
    case EdgeKind::Fallthrough:
        return "fallthrough";
    case EdgeKind::Branch:
        return "branch";
    case EdgeKind::ConditionalTaken:
        return "taken";
    case EdgeKind::ConditionalNotTaken:
        return "fallthrough";
    }
    return "unknown";
}

std::string_view call_kind_name(CallKind kind) noexcept
{
    switch (kind)
    {
    case CallKind::Direct:
        return "direct";
    case CallKind::Indirect:
        return "indirect";
    }
    return "unknown";
}

Result<void> validate_control_flow_graph(const ControlFlowGraph& graph)
{
    std::set<aarch64::GuestAddress> instruction_addresses;
    for (const auto& [start, block] : graph.blocks)
    {
        if ((start & 0x3U) != 0U || block.start != start || block.instructions.empty())
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidFormat,
                "CFG contains an empty, misaligned, or incorrectly keyed basic block"));
        }

        aarch64::GuestAddress expected = start;
        for (const auto& instruction : block.instructions)
        {
            if (instruction.address != expected || (instruction.address & 0x3U) != 0U ||
                !instruction_addresses.insert(instruction.address).second)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidFormat,
                    "CFG instruction addresses are not unique and monotonically increasing"));
            }
            const auto next = checked_add_u64(expected, 4U);
            if (!next)
            {
                return Result<void>::failure(next.error());
            }
            expected = next.value();
        }

        for (const auto& edge : block.successors)
        {
            if (edge.source != block.instructions.back().address)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidFormat, "CFG edge source does not match block terminator"));
            }
            if (edge.internal && !graph.blocks.contains(edge.target))
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidFormat,
                    "CFG internal edge points to an address that is not a block leader"));
            }
        }

        const auto& last = block.instructions.back();
        const auto kind = last.control_flow.kind;
        const auto has_kind = [&block](EdgeKind wanted) {
            return std::any_of(block.successors.begin(), block.successors.end(),
                               [wanted](const ControlFlowEdge& edge) {
                                   return edge.kind == wanted;
                               });
        };
        if (kind == aarch64::ControlFlowKind::Return ||
            kind == aarch64::ControlFlowKind::IndirectBranch ||
            kind == aarch64::ControlFlowKind::Trap ||
            kind == aarch64::ControlFlowKind::Exception ||
            kind == aarch64::ControlFlowKind::Unknown)
        {
            if (!block.successors.empty())
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidFormat,
                    "terminal CFG instruction unexpectedly has a normal successor"));
            }
        }
        if (kind == aarch64::ControlFlowKind::DirectBranch &&
            (has_kind(EdgeKind::Fallthrough) || has_kind(EdgeKind::ConditionalNotTaken)))
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidFormat, "unconditional branch unexpectedly has fallthrough"));
        }
        if (kind == aarch64::ControlFlowKind::ConditionalBranch)
        {
            if (!has_kind(EdgeKind::ConditionalTaken) ||
                !has_kind(EdgeKind::ConditionalNotTaken))
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidFormat,
                    "conditional branch does not have taken and not-taken successors"));
            }
        }
    }
    return Result<void>::success();
}

std::string render_control_flow_graph(const ControlFlowGraph& graph)
{
    std::ostringstream output;
    output << "entry: " << hex_address(graph.entry) << '\n'
           << "blocks: " << graph.blocks.size() << '\n'
           << "instructions: " << graph.instruction_count << '\n';
    for (const auto& [start, block] : graph.blocks)
    {
        output << "\nblock " << hex_address(start) << '\n';
        for (const auto& instruction : block.instructions)
        {
            output << "  " << hex_address(instruction.address) << "  "
                   << hex_opcode(instruction.opcode) << "  " << instruction.disassembly << '\n';
        }
        output << "  successors:\n";
        for (const auto& edge : block.successors)
        {
            output << "    " << edge_kind_name(edge.kind) << " -> " << hex_address(edge.target)
                   << (edge.internal ? "" : " (external)") << '\n';
        }
    }

    output << "\ncalls:\n";
    std::vector<CallSite> calls = graph.calls;
    std::sort(calls.begin(), calls.end(), [](const CallSite& left, const CallSite& right) {
        return left.address < right.address;
    });
    for (const auto& call : calls)
    {
        output << "  " << hex_address(call.address) << " " << call_kind_name(call.kind);
        if (call.target)
        {
            output << " -> " << hex_address(call.target.value());
        }
        if (call.register_target)
        {
            output << " via " << aarch64::register_name(call.register_target.value());
        }
        output << '\n';
    }

    output << "unresolved:\n";
    std::vector<UnresolvedControlFlow> unresolved = graph.unresolved;
    std::sort(unresolved.begin(), unresolved.end(),
              [](const UnresolvedControlFlow& left, const UnresolvedControlFlow& right) {
                  return left.address < right.address;
              });
    for (const auto& item : unresolved)
    {
        output << "  " << hex_address(item.address) << " "
               << aarch64::control_flow_kind_name(item.kind);
        if (item.register_target)
        {
            output << " via " << aarch64::register_name(item.register_target.value());
        }
        if (!item.reason.empty())
        {
            output << ": " << item.reason;
        }
        output << '\n';
    }
    return output.str();
}

} // namespace switchrecomp::analysis
