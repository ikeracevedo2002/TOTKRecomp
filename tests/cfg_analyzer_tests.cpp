#include "switchrecomp/analysis/cfg_analyzer.hpp"

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using switchrecomp::ErrorCode;
using switchrecomp::analysis::AnalysisOptions;
using switchrecomp::analysis::ControlFlowEdge;
using switchrecomp::analysis::EdgeKind;
using switchrecomp::analysis::GuestAddressRange;
using switchrecomp::analysis::analyze_control_flow;
using switchrecomp::analysis::validate_control_flow_graph;
using switchrecomp::memory::GuestAddress;
using switchrecomp::memory::GuestMemory;
using switchrecomp::memory::GuestMemoryPermissions;
using switchrecomp::memory::GuestRegionKind;

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size() * 4U);
    for (const auto value : values)
    {
        result.push_back(static_cast<std::byte>(value & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    }
    return result;
}

[[nodiscard]] GuestMemory make_code(GuestAddress base, std::initializer_list<std::uint32_t> code)
{
    GuestMemory memory;
    const auto bytes = words(code);
    REQUIRE(memory.map(base, bytes, GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute,
                       ".text", GuestRegionKind::Text));
    return memory;
}

[[nodiscard]] const ControlFlowEdge* find_edge(
    const switchrecomp::analysis::BasicBlock& block, EdgeKind kind)
{
    for (const auto& edge : block.successors)
    {
        if (edge.kind == kind)
        {
            return &edge;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("CFG analyzes straight-line code as one terminating block")
{
    // add x0, x0, x1; sub x2, x2, #1; ret
    const auto memory = make_code(0x1000U, {0x8b010000U, 0xd1000442U, 0xd65f03c0U});
    const auto graph = analyze_control_flow(memory, 0x1000U);
    REQUIRE(graph);
    REQUIRE(graph.value().blocks.size() == 1U);
    REQUIRE(graph.value().instruction_count == 3U);
    REQUIRE(graph.value().blocks.at(0x1000U).instructions.size() == 3U);
    REQUIRE(graph.value().blocks.at(0x1000U).successors.empty());
    REQUIRE(validate_control_flow_graph(graph.value()));
}

TEST_CASE("CFG does not follow an unconditional branch fallthrough")
{
    // b target; nop; target: ret
    const auto memory = make_code(0x1000U, {0x14000002U, 0xd503201fU, 0xd65f03c0U});
    const auto graph = analyze_control_flow(memory, 0x1000U);
    REQUIRE(graph);
    REQUIRE(graph.value().blocks.size() == 2U);
    REQUIRE_FALSE(graph.value().blocks.contains(0x1004U));
    const auto& block = graph.value().blocks.at(0x1000U);
    REQUIRE(block.successors.size() == 1U);
    REQUIRE(block.successors.front().kind == EdgeKind::Branch);
    REQUIRE(block.successors.front().target == 0x1008U);
}

TEST_CASE("CFG handles conditional diamonds and converging leaders")
{
    // cmp x0, #0; b.eq true; mov x1, #1; b end; true: mov x1, #2; nop; end: ret
    const auto memory = make_code(0x1000U, {0xf100001fU, 0x54000060U, 0xd2800021U,
                                           0x14000003U, 0xd2800041U, 0xd503201fU,
                                           0xd65f03c0U});
    const auto graph = analyze_control_flow(memory, 0x1000U);
    REQUIRE(graph);
    REQUIRE(graph.value().blocks.contains(0x1000U));
    REQUIRE(graph.value().blocks.contains(0x1008U));
    REQUIRE(graph.value().blocks.contains(0x1010U));
    REQUIRE(graph.value().blocks.contains(0x1018U));
    REQUIRE(graph.value().blocks.at(0x1000U).successors.size() == 2U);
    REQUIRE(find_edge(graph.value().blocks.at(0x1000U), EdgeKind::ConditionalTaken) != nullptr);
    REQUIRE(find_edge(graph.value().blocks.at(0x1000U), EdgeKind::ConditionalNotTaken) != nullptr);
    REQUIRE(graph.value().blocks.at(0x1008U).successors.front().target == 0x1018U);
    REQUIRE(validate_control_flow_graph(graph.value()));
}

TEST_CASE("CFG terminates on loops and preserves a backward edge")
{
    // subs x0, x0, #1; b.ne loop; ret
    const auto memory = make_code(0x1000U, {0xf1000400U, 0x54ffffe1U, 0xd65f03c0U});
    const auto graph = analyze_control_flow(memory, 0x1000U);
    REQUIRE(graph);
    REQUIRE(graph.value().blocks.size() == 2U);
    const auto& loop = graph.value().blocks.at(0x1000U);
    REQUIRE(loop.successors.size() == 2U);
    REQUIRE(loop.successors[0].target == 0x1000U);
    REQUIRE(loop.successors[1].target == 0x1008U);
}

TEST_CASE("CFG records direct and indirect calls but keeps the return site")
{
    // bl function; add x0, x0, #1; ret; function: ret
    const auto memory = make_code(0x1000U, {0x94000002U, 0x91000400U, 0xd65f03c0U,
                                           0xd65f03c0U});
    const auto graph = analyze_control_flow(memory, 0x1000U);
    REQUIRE(graph);
    REQUIRE(graph.value().calls.size() == 1U);
    REQUIRE(graph.value().calls.front().target == 0x1008U);
    REQUIRE(graph.value().blocks.at(0x1000U).successors.front().target == 0x1004U);
    REQUIRE_FALSE(graph.value().blocks.contains(0x1008U));

    const auto indirect_memory = make_code(0x2000U, {0xd63f0100U, 0xd65f03c0U});
    const auto indirect = analyze_control_flow(indirect_memory, 0x2000U);
    REQUIRE(indirect);
    REQUIRE(indirect.value().calls.size() == 1U);
    REQUIRE_FALSE(indirect.value().calls.front().target.has_value());
    REQUIRE(indirect.value().unresolved.size() == 1U);
    REQUIRE(indirect.value().blocks.at(0x2000U).successors.front().target == 0x2004U);
}

TEST_CASE("CFG preserves unresolved indirect branches and terminates the block")
{
    const auto memory = make_code(0x1000U, {0xd61f0120U});
    const auto graph = analyze_control_flow(memory, 0x1000U);
    REQUIRE(graph);
    REQUIRE(graph.value().blocks.at(0x1000U).successors.empty());
    REQUIRE(graph.value().unresolved.size() == 1U);
    REQUIRE(graph.value().unresolved.front().address == 0x1000U);
}

TEST_CASE("CFG handles compare-and-test branches with two successors")
{
    // cbz x0, +8; nop; ret
    const auto cbz_memory = make_code(0x1000U, {0xb4000040U, 0xd503201fU, 0xd65f03c0U});
    const auto cbz = analyze_control_flow(cbz_memory, 0x1000U);
    REQUIRE(cbz);
    REQUIRE(cbz.value().blocks.at(0x1000U).successors.size() == 2U);

    // tbnz x0, #3, +8; nop; ret
    const auto tbz_memory = make_code(0x2000U, {0x37180040U, 0xd503201fU, 0xd65f03c0U});
    const auto tbz = analyze_control_flow(tbz_memory, 0x2000U);
    REQUIRE(tbz);
    REQUIRE(tbz.value().blocks.at(0x2000U).successors.size() == 2U);
}

TEST_CASE("CFG rejects malformed, unaligned and non-executable direct targets")
{
    // b +4 targets an address immediately after the only mapped instruction.
    const auto unmapped = make_code(0x1000U, {0x14000001U});
    const auto invalid = analyze_control_flow(unmapped, 0x1000U);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == ErrorCode::InvalidBranchTarget);

    GuestMemory non_executable;
    const auto bytes = words({0xd503201fU});
    REQUIRE(non_executable.map(0x3000U, bytes, GuestMemoryPermissions::Read, ".rodata",
                               GuestRegionKind::Rodata));
    const auto denied = analyze_control_flow(non_executable, 0x3000U);
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error().code == ErrorCode::NonExecutableAddress);

    const auto aligned = make_code(0x4000U, {0x14000000U});
    const auto graph = analyze_control_flow(aligned, 0x4000U);
    REQUIRE(graph);
    REQUIRE(graph.value().blocks.at(0x4000U).successors.front().target == 0x4000U);
}

TEST_CASE("CFG enforces range and resource limits explicitly")
{
    const auto memory = make_code(0x1000U, {0xd503201fU, 0xd503201fU, 0xd65f03c0U});
    AnalysisOptions instruction_limit;
    instruction_limit.max_instructions = 1U;
    const auto limited = analyze_control_flow(memory, 0x1000U, instruction_limit);
    REQUIRE_FALSE(limited);
    REQUIRE(limited.error().code == ErrorCode::AnalysisInstructionLimitExceeded);

    AnalysisOptions range;
    range.allowed_code_range = GuestAddressRange{0x1000U, 4U};
    const auto bounded = analyze_control_flow(memory, 0x1000U, range);
    REQUIRE(bounded);
    REQUIRE(bounded.value().blocks.size() == 1U);
    REQUIRE(bounded.value().blocks.at(0x1000U).successors.front().internal == false);
}

TEST_CASE("CFG splits a block when a later branch targets its middle")
{
    // add; add; b.eq back_to_middle; ret
    const auto memory = make_code(0x1000U, {0x8b010000U, 0x8b010000U, 0x54ffffe0U,
                                           0xd65f03c0U});
    const auto graph = analyze_control_flow(memory, 0x1000U);
    REQUIRE(graph);
    REQUIRE(graph.value().blocks.contains(0x1000U));
    REQUIRE(graph.value().blocks.contains(0x1004U));
    REQUIRE(graph.value().blocks.at(0x1000U).instructions.size() == 1U);
    REQUIRE(graph.value().blocks.at(0x1004U).instructions.size() == 2U);
    REQUIRE(validate_control_flow_graph(graph.value()));
}

TEST_CASE("CFG rendering is deterministic and retains unresolved diagnostics")
{
    const auto memory = make_code(0x1000U, {0xd61f0120U});
    const auto first = analyze_control_flow(memory, 0x1000U);
    const auto second = analyze_control_flow(memory, 0x1000U);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(switchrecomp::analysis::render_control_flow_graph(first.value()) ==
            switchrecomp::analysis::render_control_flow_graph(second.value()));
}
