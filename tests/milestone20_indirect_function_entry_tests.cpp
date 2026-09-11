#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/indirect_target.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::FunctionEntryEvidence;
using analysis::FunctionEntryEvidenceKind;
using analysis::FunctionEntryEvidenceStrength;
using analysis::FunctionEntryTrustStatus;
using analysis::IndirectControlFlowKind;
using analysis::IndirectTargetDecisionKind;
using analysis::IndirectTargetDiscoveryOptions;
using analysis::ObservedIndirectTarget;
using memory::GuestAddress;

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

[[nodiscard]] std::uint32_t movz(std::uint8_t reg, std::uint16_t value)
{
    return 0xd2800000U | (static_cast<std::uint32_t>(value) << 5U) | reg;
}

struct Fixture
{
    memory::GuestMemory memory;
    analysis::ModuleIdentity identity;
    analysis::FinalizedFunctionMap map;
};

[[nodiscard]] Result<Fixture> fixture(GuestAddress base, std::string module)
{
    Fixture result;
    auto code = words({movz(17U, 0x1020U), 0xd61f0220U});
    code.resize(0x40U, std::byte{0});
    const auto target = words({movz(0U, 7U), 0xd65f03c0U});
    std::copy(target.begin(), target.end(), code.begin() + 0x20);
    const auto mapped = result.memory.map(
        base, std::span<const std::byte>(code.data(), code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "milestone20.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());
    result.identity.module = std::move(module);
    result.identity.build_id = "milestone20-synthetic";
    result.identity.input_sha256 = "milestone20-synthetic";
    result.identity.guest_base = base;
    result.identity.executable_ranges.push_back({base, 0x40U});
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{result.identity, &result.memory,
                                      {{base, analysis::FunctionDiscoverySource::AnalystSeed,
                                        analysis::FunctionConfidence::Manual, std::nullopt,
                                        std::nullopt, "M20 synthetic caller"}}});
    if (!map) return Result<Fixture>::failure(map.error());
    result.map = std::move(map).value();
    return Result<Fixture>::success(std::move(result));
}

[[nodiscard]] ObservedIndirectTarget observed(GuestAddress target)
{
    ObservedIndirectTarget result;
    result.source_module = "caller";
    result.source_function = 0x1000U;
    result.source_pc = 0x1004U;
    result.control_flow = IndirectControlFlowKind::Call;
    result.target_register = "x17";
    result.target = target;
    result.target_module = "caller";
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::GuestLoad;
    result.guest_load_address = 0x9000U;
    return result;
}

} // namespace

TEST_CASE("M20 normalizes SMULH as structural fallthrough without adding semantics")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(0x1000U, 0x9b4c7d4aU);
    REQUIRE(decoded);
    REQUIRE(decoded.value().id == aarch64::InstructionId::Smulh);
    REQUIRE(decoded.value().normalized);
    REQUIRE(decoded.value().control_flow.kind == aarch64::ControlFlowKind::Fallthrough);
    REQUIRE(decoded.value().disassembly.find("smulh") != std::string::npos);
}

TEST_CASE("M20 executable, aligned, and guest-loaded evidence remains insufficient alone")
{
    auto candidate = fixture(0x1000U, "caller");
    REQUIRE(candidate);
    auto target = observed(0x1020U);
    IndirectTargetDiscoveryOptions options;
    options.allow_runtime_cfg_promotion = false;
    const auto assessment = analysis::assess_indirect_target(
        target, candidate.value().memory, &candidate.value().map, nullptr, nullptr, options);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::InsufficientEvidence);
    REQUIRE_FALSE(assessment.value().decision.eligible_for_promotion);
    REQUIRE(assessment.value().certification.status ==
            analysis::FunctionCertificationStatus::InsufficientEvidence);
}

TEST_CASE("M20 exact typed function evidence can certify a bounded candidate")
{
    auto candidate = fixture(0x1000U, "caller");
    REQUIRE(candidate);
    auto target = observed(0x1020U);
    target.entry_evidence.push_back(FunctionEntryEvidence{
        FunctionEntryEvidenceKind::DynamicSymbolFunction, FunctionEntryEvidenceStrength::Exact,
        "caller", std::nullopt, "caller", 0x1020U, std::nullopt, std::nullopt, std::nullopt,
        17U, "synthetic_function", true, false, "synthetic exact function metadata"});
    IndirectTargetDiscoveryOptions options;
    options.allow_runtime_cfg_promotion = false;
    options.require_independent_static_evidence = true;
    const auto assessment = analysis::assess_indirect_target(
        target, candidate.value().memory, &candidate.value().map, nullptr, nullptr, options);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
    REQUIRE(assessment.value().certification.certified);
    REQUIRE(assessment.value().candidate.entry == 0x1020U);
}

TEST_CASE("M20 relocation-backed function metadata is exact evidence")
{
    auto candidate = fixture(0x1000U, "caller");
    REQUIRE(candidate);
    auto target = observed(0x1020U);
    target.entry_evidence.push_back(FunctionEntryEvidence{
        FunctionEntryEvidenceKind::RelocationFunctionTarget, FunctionEntryEvidenceStrength::Exact,
        "caller", 0x9000U, "caller", 0x1020U, 3U,
        format::AArch64RelocationType::Abs64, format::RelocationSource::Rela, 23U,
        "relocated_function", true, true, "relocation resolves to declared function metadata"});
    IndirectTargetDiscoveryOptions options;
    options.allow_runtime_cfg_promotion = false;
    options.require_independent_static_evidence = true;
    const auto assessment = analysis::assess_indirect_target(
        target, candidate.value().memory, &candidate.value().map, nullptr, nullptr, options);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
    REQUIRE(assessment.value().certification.certified);
    REQUIRE(std::any_of(assessment.value().entry_evidence.begin(),
                        assessment.value().entry_evidence.end(), [](const auto& evidence) {
                            return evidence.kind == FunctionEntryEvidenceKind::RelocationFunctionTarget &&
                                   evidence.strength == FunctionEntryEvidenceStrength::Exact &&
                                   evidence.target_declared_function;
                        }));
}

TEST_CASE("M20 relocation-like supporting evidence without function metadata is rejected")
{
    auto candidate = fixture(0x1000U, "caller");
    REQUIRE(candidate);
    auto target = observed(0x1020U);
    target.entry_evidence.push_back(FunctionEntryEvidence{
        FunctionEntryEvidenceKind::RelocatedFunctionPointer,
        FunctionEntryEvidenceStrength::Supporting, "caller", 0x9000U, "caller", 0x1020U,
        4U, format::AArch64RelocationType::Relative, format::RelocationSource::Rela, 0U, {},
        false, true, "rebasing-only relocation"});
    IndirectTargetDiscoveryOptions options;
    options.allow_runtime_cfg_promotion = false;
    options.require_independent_static_evidence = true;
    const auto assessment = analysis::assess_indirect_target(
        target, candidate.value().memory, &candidate.value().map, nullptr, nullptr, options);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::InsufficientEvidence);
    REQUIRE_FALSE(assessment.value().certification.certified);
}

TEST_CASE("M20 promotion is transactional and idempotent")
{
    auto candidate = fixture(0x1000U, "caller");
    REQUIRE(candidate);
    const auto input = analysis::ModuleAnalysisInput{
        candidate.value().identity, &candidate.value().memory,
        {{0x1000U, analysis::FunctionDiscoverySource::AnalystSeed,
          analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
          "M20 synthetic caller"}}};
    const auto before = candidate.value().map.functions().size();
    auto refined = analysis::refine_function_map(candidate.value().map, input, observed(0x1020U));
    REQUIRE(refined);
    REQUIRE(refined.value().assessment.decision.promoted);
    REQUIRE(refined.value().map.find(0x1020U) != nullptr);
    REQUIRE(refined.value().map.find(0x1020U)->entry_trust_status ==
            FunctionEntryTrustStatus::Trusted);
    REQUIRE(candidate.value().map.functions().size() == before);

    const auto again = analysis::refine_function_map(refined.value().map, input,
                                                      observed(0x1020U));
    REQUIRE(again);
    REQUIRE(again.value().assessment.decision.kind == IndirectTargetDecisionKind::TrustedExistingEntry);
    REQUIRE(again.value().map.functions().size() == refined.value().map.functions().size());
}

TEST_CASE("M20 cross-module candidate ownership uses process guest addresses")
{
    auto caller = fixture(0x1000U, "caller");
    auto callee = fixture(0x2000U, "callee");
    REQUIRE(caller);
    REQUIRE(callee);
    const auto maps = analysis::ProcessFunctionMap::build(
        {caller.value().map, callee.value().map});
    REQUIRE(maps);
    auto target = observed(0x2020U);
    target.source_module = "caller";
    target.target_module = "callee";
    target.source_function = 0x1000U;
    const auto assessment = analysis::assess_indirect_target(
        target, callee.value().memory, nullptr, &maps.value(), nullptr);
    REQUIRE(assessment);
    REQUIRE(assessment.value().validation.target_module == "callee");
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
}

TEST_CASE("M20 evidence ordering is independent of insertion order")
{
    auto candidate = fixture(0x1000U, "caller");
    REQUIRE(candidate);
    auto first = observed(0x1020U);
    auto second = first;
    first.entry_evidence.push_back(FunctionEntryEvidence{
        FunctionEntryEvidenceKind::GuestLoadedPointer, FunctionEntryEvidenceStrength::Supporting,
        "caller", 0x9000U, "caller", 0x1020U, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, {}, false, false, "loaded"});
    first.entry_evidence.push_back(FunctionEntryEvidence{
        FunctionEntryEvidenceKind::ObservedIndirectCall, FunctionEntryEvidenceStrength::Supporting,
        "caller", 0x1004U, "caller", 0x1020U, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, {}, false, false, "call"});
    second.entry_evidence = {first.entry_evidence[1], first.entry_evidence[0]};
    const auto one = analysis::assess_indirect_target(
        first, candidate.value().memory, &candidate.value().map);
    const auto two = analysis::assess_indirect_target(
        second, candidate.value().memory, &candidate.value().map);
    REQUIRE(one);
    REQUIRE(two);
    REQUIRE(one.value().entry_evidence == two.value().entry_evidence);
}
