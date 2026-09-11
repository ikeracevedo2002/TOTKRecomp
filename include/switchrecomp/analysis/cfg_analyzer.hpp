#pragma once

#include "switchrecomp/analysis/control_flow_graph.hpp"
#include "switchrecomp/aarch64/decoder.hpp"

#include <cstddef>
#include <optional>
#include <set>

namespace switchrecomp::analysis
{

struct GuestAddressRange
{
    aarch64::GuestAddress base = 0U;
    memory::GuestSize size = 0U;

    friend constexpr bool operator==(const GuestAddressRange&, const GuestAddressRange&) = default;
};

struct AnalysisOptions
{
    std::size_t max_instructions = 65'536U;
    std::size_t max_basic_blocks = 16'384U;
    std::size_t max_pending_targets = 65'536U;
    std::optional<GuestAddressRange> allowed_code_range;
    // Strong, finalized function entries are boundaries for unconditional B
    // transfers. Conditional branches retain their ordinary CFG semantics.
    std::set<aarch64::GuestAddress> known_function_entries;
};

[[nodiscard]] Result<ControlFlowGraph> analyze_control_flow(
    const memory::GuestMemory& memory,
    aarch64::GuestAddress entry,
    const AnalysisOptions& options = {});

[[nodiscard]] Result<ControlFlowGraph> analyze_control_flow(
    const memory::GuestMemory& memory,
    const aarch64::AArch64Decoder& decoder,
    aarch64::GuestAddress entry,
    const AnalysisOptions& options = {});

} // namespace switchrecomp::analysis
