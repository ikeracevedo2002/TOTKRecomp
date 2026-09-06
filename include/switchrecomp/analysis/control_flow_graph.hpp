#pragma once

#include "switchrecomp/aarch64/instruction.hpp"
#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace switchrecomp::analysis
{

enum class EdgeKind : std::uint8_t
{
    Fallthrough,
    Branch,
    ConditionalTaken,
    ConditionalNotTaken,
};

enum class CallKind : std::uint8_t
{
    Direct,
    Indirect,
};

struct ControlFlowEdge
{
    aarch64::GuestAddress source = 0U;
    aarch64::GuestAddress target = 0U;
    EdgeKind kind = EdgeKind::Fallthrough;
    bool internal = true;
};

struct CallSite
{
    aarch64::GuestAddress address = 0U;
    CallKind kind = CallKind::Direct;
    std::optional<aarch64::GuestAddress> target;
    std::optional<aarch64::Register> register_target;
};

struct UnresolvedControlFlow
{
    aarch64::GuestAddress address = 0U;
    aarch64::ControlFlowKind kind = aarch64::ControlFlowKind::Unknown;
    std::optional<aarch64::Register> register_target;
    std::string reason;
};

struct BasicBlock
{
    aarch64::GuestAddress start = 0U;
    std::vector<aarch64::DecodedInstruction> instructions;
    std::vector<ControlFlowEdge> successors;
    std::vector<CallSite> calls;
    std::string termination;
};

struct ControlFlowGraph
{
    aarch64::GuestAddress entry = 0U;
    std::map<aarch64::GuestAddress, BasicBlock> blocks;
    std::vector<CallSite> calls;
    std::vector<UnresolvedControlFlow> unresolved;
    std::size_t instruction_count = 0U;
};

[[nodiscard]] std::string_view edge_kind_name(EdgeKind kind) noexcept;
[[nodiscard]] std::string_view call_kind_name(CallKind kind) noexcept;

[[nodiscard]] Result<void> validate_control_flow_graph(const ControlFlowGraph& graph);
[[nodiscard]] std::string render_control_flow_graph(const ControlFlowGraph& graph);

} // namespace switchrecomp::analysis
