#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/analysis/process_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::AnalysisBudgetDimension;
using analysis::AnalysisBudgetProvenanceKind;
using analysis::AnalysisBudgets;
using analysis::AnalysisStrategy;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;
using analysis::FunctionMapOptions;
using analysis::FunctionSeed;
using memory::GuestAddress;

constexpr std::uint32_t ret_instruction = 0xd65f03c0U;
constexpr std::uint32_t nop_instruction = 0xd503201fU;

void write_word(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::uint32_t branch(GuestAddress pc, GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) -
                              static_cast<std::int64_t>(pc);
    return 0x94000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

[[nodiscard]] std::uint32_t unconditional_branch(GuestAddress pc, GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) -
                              static_cast<std::int64_t>(pc);
    return 0x14000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

struct SyntheticModule
{
    memory::GuestMemory memory;
    analysis::ModuleIdentity identity;
};

[[nodiscard]] Result<SyntheticModule> make_module(std::string name, GuestAddress base,
                                                    std::size_t size = 0x100U,
                                                    std::vector<std::byte> initial_code = {})
{
    SyntheticModule module;
    std::vector<std::byte> code(size, std::byte{});
    for (std::size_t offset = 0U; offset + 4U <= code.size(); offset += 4U)
        write_word(code, offset, nop_instruction);
    write_word(code, 0U, ret_instruction);
    if (!initial_code.empty())
    {
        if (initial_code.size() > code.size())
            return Result<SyntheticModule>::failure(
                make_error(ErrorCode::InvalidArgument, "synthetic code exceeds module size"));
        std::copy(initial_code.begin(), initial_code.end(), code.begin());
    }
    const auto mapped = module.memory.map(
        base, std::span<const std::byte>(code.data(), code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m24.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<SyntheticModule>::failure(mapped.error());
    module.identity.module = std::move(name);
    module.identity.build_id = "synthetic-m24-build";
    module.identity.input_sha256 = "synthetic-m24-sha";
    module.identity.guest_base = base;
    module.identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, static_cast<memory::GuestSize>(size)});
    return Result<SyntheticModule>::success(std::move(module));
}

[[nodiscard]] FunctionSeed seed(GuestAddress address, FunctionDiscoverySource source,
                                FunctionConfidence confidence, std::string note = {})
{
    return FunctionSeed{address, source, confidence, std::nullopt, std::nullopt, std::move(note)};
}

[[nodiscard]] Result<analysis::FinalizedFunctionMap> build_map(
    SyntheticModule& module, std::vector<FunctionSeed> seeds, AnalysisBudgets budgets,
    std::vector<GuestAddress> roots)
{
    FunctionMapOptions options;
    options.budgets = std::move(budgets);
    options.execution_closure_roots.insert(roots.begin(), roots.end());
    options.continue_after_function_failure = false;
    return analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{module.identity, &module.memory, std::move(seeds)}, options);
}

} // namespace

TEST_CASE("M24 execution-closure profile and structural envelope are finite and auditable")
{
    const auto profile = analysis::make_execution_closure_analysis_budgets();
    REQUIRE(profile.strategy == AnalysisStrategy::ExecutionClosure);
    REQUIRE(profile.max_functions != 0U);
    REQUIRE(profile.max_instructions != 0U);
    REQUIRE(profile.max_blocks != 0U);
    REQUIRE(profile.max_edges != 0U);
    REQUIRE(profile.max_seeds != 0U);
    REQUIRE(profile.max_bytes_analyzed != 0U);
    REQUIRE(profile.max_boundary_finalization_passes != 0U);
    REQUIRE(profile.provenance.max_functions.kind == AnalysisBudgetProvenanceKind::ExecutionToolProfile);
    REQUIRE(profile.provenance.max_functions.detail == "run_entry_execution_closure");

    auto module = make_module("finite", 0x1000U, 0x10U);
    REQUIRE(module);
    auto budgets = profile;
    budgets.max_functions = std::numeric_limits<std::size_t>::max();
    budgets.max_instructions = std::numeric_limits<std::size_t>::max();
    budgets.max_blocks = std::numeric_limits<std::size_t>::max();
    budgets.max_edges = std::numeric_limits<std::size_t>::max();
    budgets.max_seeds = std::numeric_limits<std::size_t>::max();
    budgets.max_bytes_analyzed = std::numeric_limits<memory::GuestSize>::max();
    auto map = build_map(module.value(), {seed(0x1000U, FunctionDiscoverySource::AnalystSeed,
                                               FunctionConfidence::Manual)},
                         budgets, {0x1000U});
    REQUIRE(map);
    const auto& accounting = map.value().accounting();
    const auto analysis_passes = profile.max_boundary_finalization_passes + 1U;
    REQUIRE(accounting.budgets.max_functions <= 4U);
    REQUIRE(accounting.budgets.max_instructions <= 4U * analysis_passes);
    REQUIRE(accounting.budgets.max_blocks <= 4U * analysis_passes);
    REQUIRE(accounting.budgets.max_seeds <= 4U);
    REQUIRE(accounting.budgets.max_edges <= 8U * analysis_passes);
    REQUIRE(accounting.budgets.max_bytes_analyzed <= 0x10U * analysis_passes);
    REQUIRE(accounting.budgets.provenance.max_functions.kind ==
            AnalysisBudgetProvenanceKind::DerivedStructuralBound);
    REQUIRE(accounting.budgets.provenance.max_bytes_analyzed.kind ==
            AnalysisBudgetProvenanceKind::DerivedStructuralBound);

    auto invalid = profile;
    invalid.max_functions = 0U;
    REQUIRE(build_map(module.value(),
                      {seed(0x1000U, FunctionDiscoverySource::AnalystSeed,
                            FunctionConfidence::Manual)},
                      invalid, {0x1000U})
                .error()
                .code == ErrorCode::InvalidArgument);
    invalid = profile;
    invalid.max_boundary_finalization_passes = 0U;
    REQUIRE(build_map(module.value(),
                      {seed(0x1000U, FunctionDiscoverySource::AnalystSeed,
                            FunctionConfidence::Manual)},
                      invalid, {0x1000U})
                .error()
                .code == ErrorCode::InvalidArgument);
}

TEST_CASE("M24 duplicate evidence and irrelevant candidates coalesce outside the closure")
{
    auto module = make_module("coalesced", 0x1000U);
    REQUIRE(module);
    auto budgets = analysis::make_execution_closure_analysis_budgets();
    std::vector<FunctionSeed> seeds{
        seed(0x1000U, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed),
        seed(0x1000U, FunctionDiscoverySource::DynamicSymbol, FunctionConfidence::Confirmed),
        seed(0x1000U, FunctionDiscoverySource::AnalystSeed, FunctionConfidence::Manual)};
    for (GuestAddress address = 0x1040U; address < 0x1080U; address += 4U)
        seeds.push_back(seed(address, FunctionDiscoverySource::Heuristic,
                             FunctionConfidence::Low, "irrelevant candidate"));

    const auto map = build_map(module.value(), std::move(seeds), budgets, {0x1000U});
    REQUIRE(map);
    REQUIRE(map.value().functions().size() == 1U);
    const auto& accounting = map.value().accounting();
    REQUIRE(accounting.initial_seed_count == 19U);
    REQUIRE(accounting.normalized_unique_seed_count == 1U);
    REQUIRE(accounting.duplicate_coalesced_seed_count == 2U);
    REQUIRE(accounting.excluded_candidate_seed_count == 16U);
    REQUIRE(accounting.functions_cfg_analyzed == 2U);
    REQUIRE(accounting.candidate_function_entries == 0U);
    REQUIRE(accounting.trusted_function_entries == 1U);
    REQUIRE(accounting.phases.initial_seeding == 19U);
    REQUIRE(accounting.seed_sources.size() == 4U);
    REQUIRE(accounting.seed_sources[0].source == FunctionDiscoverySource::ModuleEntry);
    REQUIRE(accounting.seed_sources[1].source == FunctionDiscoverySource::DynamicSymbol);
    REQUIRE(accounting.seed_sources[2].source == FunctionDiscoverySource::AnalystSeed);
    REQUIRE(accounting.seed_sources[3].source == FunctionDiscoverySource::Heuristic);
    REQUIRE(accounting.seed_sources[3].excluded == 16U);
}

TEST_CASE("M24 direct-call closure remains transitive while an irrelevant module stays bounded")
{
    auto caller_code = std::vector<std::byte>(0x100U, std::byte{});
    for (std::size_t offset = 0U; offset < caller_code.size(); offset += 4U)
        write_word(caller_code, offset, nop_instruction);
    write_word(caller_code, 0U, branch(0x1000U, 0x1010U));
    write_word(caller_code, 4U, ret_instruction);
    write_word(caller_code, 0x10U, ret_instruction);
    auto caller = make_module("main", 0x1000U, 0x100U, std::move(caller_code));
    auto irrelevant = make_module("sdk", 0x2000U);
    REQUIRE(caller);
    REQUIRE(irrelevant);

    auto budgets = analysis::make_execution_closure_analysis_budgets();
    const auto main_map = build_map(
        caller.value(), {seed(0x1000U, FunctionDiscoverySource::ModuleEntry,
                               FunctionConfidence::Confirmed)},
        budgets, {0x1000U});
    REQUIRE(main_map);
    REQUIRE(main_map.value().functions().size() == 2U);
    REQUIRE(main_map.value().accounting().direct_call_discoveries >= 1U);
    REQUIRE(main_map.value().accounting().phases.direct_call_expansion >= 1U);

    std::vector<FunctionSeed> irrelevant_seeds;
    for (GuestAddress address = 0x2000U; address < 0x2100U; address += 4U)
        irrelevant_seeds.push_back(seed(address, FunctionDiscoverySource::DynamicSymbol,
                                        FunctionConfidence::Low, "irrelevant module candidate"));
    const auto sdk_map = build_map(irrelevant.value(), std::move(irrelevant_seeds), budgets, {0x2000U});
    REQUIRE(sdk_map);
    REQUIRE(sdk_map.value().functions().size() == 1U);
    REQUIRE(sdk_map.value().accounting().excluded_candidate_seed_count == 63U);

    auto process_map = analysis::ProcessFunctionMap::build(
        {main_map.value(), sdk_map.value()});
    REQUIRE(process_map);
    REQUIRE(process_map.value().maps().size() == 2U);
}

TEST_CASE("M24 budget failures retain exact typed exhaustion context")
{
    auto module = make_module("budgets", 0x1000U);
    REQUIRE(module);
    const auto make_options = [] {
        return analysis::make_execution_closure_analysis_budgets();
    };

    SECTION("function dimension")
    {
        auto budgets = make_options();
        budgets.max_functions = 1U;
        const auto result = build_map(
            module.value(), {seed(0x1000U, FunctionDiscoverySource::AnalystSeed,
                                   FunctionConfidence::Manual),
                             seed(0x1004U, FunctionDiscoverySource::AnalystSeed,
                                  FunctionConfidence::Manual)},
            budgets, {0x1000U, 0x1004U});
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::AnalysisBudgetExceeded);
        REQUIRE(result.error().budget_context);
        REQUIRE(result.error().budget_context->dimension == "functions");
        REQUIRE(result.error().budget_context->consumed == 1U);
        REQUIRE(result.error().budget_context->limit == 1U);
        REQUIRE(result.error().budget_context->module == "budgets");
        REQUIRE(result.error().budget_context->phase == "initial_seeding");
        REQUIRE(result.error().budget_context->pending_work == 1U);
    }

    SECTION("instruction, block, edge, byte, and seed dimensions")
    {
        auto budgets = make_options();
        budgets.max_instructions = 1U;
        auto instruction = build_map(module.value(),
                                     {seed(0x1000U, FunctionDiscoverySource::AnalystSeed,
                                           FunctionConfidence::Manual)},
                                     budgets, {0x1000U});
        REQUIRE_FALSE(instruction);
        REQUIRE(instruction.error().budget_context->dimension == "instructions");

        budgets = make_options();
        budgets.max_blocks = 1U;
        budgets.max_instructions = 32U;
        auto branch_module = make_module(
            "branch", 0x5000U, 0x40U,
            [&] {
                std::vector<std::byte> code(0x40U, std::byte{});
                write_word(code, 0U, unconditional_branch(0x5000U, 0x5010U));
                write_word(code, 0x10U, ret_instruction);
                return code;
            }());
        REQUIRE(branch_module);
        auto block = build_map(branch_module.value(),
                               {seed(0x5000U, FunctionDiscoverySource::AnalystSeed,
                                     FunctionConfidence::Manual)},
                               budgets, {0x5000U});
        REQUIRE_FALSE(block);
        REQUIRE(block.error().budget_context->dimension == "blocks");

        budgets = make_options();
        budgets.max_edges = 1U;
        budgets.max_instructions = 32U;
        auto edge = build_map(branch_module.value(),
                              {seed(0x5000U, FunctionDiscoverySource::AnalystSeed,
                                    FunctionConfidence::Manual)},
                              budgets, {0x5000U});
        REQUIRE_FALSE(edge);
        REQUIRE(edge.error().budget_context->dimension == "edges");

        budgets = make_options();
        budgets.max_bytes_analyzed = 4U;
        budgets.max_instructions = 32U;
        auto bytes = build_map(module.value(),
                               {seed(0x1000U, FunctionDiscoverySource::AnalystSeed,
                                     FunctionConfidence::Manual)},
                               budgets, {0x1000U});
        REQUIRE_FALSE(bytes);
        REQUIRE(bytes.error().budget_context->dimension == "bytes_analyzed");

        budgets = make_options();
        budgets.max_seeds = 1U;
        budgets.max_instructions = 32U;
        auto seeds = build_map(module.value(),
                               {seed(0x1000U, FunctionDiscoverySource::AnalystSeed,
                                     FunctionConfidence::Manual),
                                seed(0x1004U, FunctionDiscoverySource::AnalystSeed,
                                     FunctionConfidence::Manual)},
                               budgets, {0x1000U, 0x1004U});
        REQUIRE_FALSE(seeds);
        REQUIRE(seeds.error().budget_context->dimension == "seeds");
    }
}

TEST_CASE("M24 provider indexing remains complete independently of CFG closure")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x3000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "synthetic.m24.provider", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable symbols;
    symbols.symbols.push_back(format::DynamicSymbol{
        1U, 0U, "provider", format::SymbolBinding::Global, format::SymbolType::Function,
        format::SymbolVisibility::Default, 1U, 0x10U, 4U});
    const std::array<analysis::ProcessSymbolSource, 1> sources{
        analysis::ProcessSymbolSource{"sdk", 0x3000U, &symbols}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    REQUIRE(namespace_result.value().lookup(
                 "provider", analysis::ModuleSetCompleteness::Incomplete,
                 analysis::ModuleSetCompletenessBasis::LegacyConfigFalse).status ==
             analysis::ProviderResolutionStatus::ProviderSearchIncomplete);
    const auto complete = namespace_result.value().lookup(
        "provider", analysis::ModuleSetCompleteness::DeclaredComplete,
        analysis::ModuleSetCompletenessBasis::ExplicitInventory);
    REQUIRE(complete.status == analysis::ProviderResolutionStatus::ResolvedGuestModule);
    REQUIRE(complete.candidates.size() == 1U);
    REQUIRE(complete.candidates.front().address == 0x3010U);

    REQUIRE(memory.map(0x4000U, 0x40U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "synthetic.m24.provider2", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable second_symbols;
    second_symbols.symbols.push_back(symbols.symbols.front());
    const std::array<analysis::ProcessSymbolSource, 2> ambiguous_sources{
        analysis::ProcessSymbolSource{"sdk", 0x3000U, &symbols},
        analysis::ProcessSymbolSource{"sdk2", 0x4000U, &second_symbols}};
    const auto ambiguous_namespace = analysis::ProcessSymbolNamespace::build_audited(
        memory, ambiguous_sources);
    REQUIRE(ambiguous_namespace);
    REQUIRE(ambiguous_namespace.value().lookup(
                 "provider", analysis::ModuleSetCompleteness::DeclaredComplete,
                 analysis::ModuleSetCompletenessBasis::ExplicitInventory).status ==
             analysis::ProviderResolutionStatus::AmbiguousGuestProvider);
}

TEST_CASE("M24 accounting and closure reports are invariant under seed permutation")
{
    auto first_module = make_module("deterministic", 0x4000U);
    auto second_module = make_module("deterministic", 0x4000U);
    REQUIRE(first_module);
    REQUIRE(second_module);
    const std::vector<FunctionSeed> first_seeds{
        seed(0x4000U, FunctionDiscoverySource::DynamicSymbol, FunctionConfidence::Confirmed),
        seed(0x4000U, FunctionDiscoverySource::AnalystSeed, FunctionConfidence::Manual),
        seed(0x4040U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low)};
    auto second_seeds = first_seeds;
    std::reverse(second_seeds.begin(), second_seeds.end());
    const auto budgets = analysis::make_execution_closure_analysis_budgets();
    const auto first = build_map(first_module.value(), first_seeds, budgets, {0x4000U});
    const auto second = build_map(second_module.value(), second_seeds, budgets, {0x4000U});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(analysis::render_analysis_accounting_json(first.value().accounting()) ==
            analysis::render_analysis_accounting_json(second.value().accounting()));
    REQUIRE(analysis::render_function_map_json(first.value()) ==
            analysis::render_function_map_json(second.value()));
}
