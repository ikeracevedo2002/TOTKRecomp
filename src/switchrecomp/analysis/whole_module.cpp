#include "switchrecomp/analysis/whole_module.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/version.hpp"
#include "switchrecomp/format/mod0.hpp"
#include "switchrecomp/loader/nso_guest_loader.hpp"
#include "switchrecomp/loader/relocation_processor.hpp"
#include "switchrecomp/loader/symbol_resolver.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/lifter/lifter.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#ifdef TOTKRECOMP_HAS_LLVM
#include "switchrecomp/codegen/llvm_backend.hpp"
#endif

namespace switchrecomp::analysis
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

[[nodiscard]] std::string hex_opcode(std::uint32_t opcode)
{
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << opcode;
    return output.str();
}

[[nodiscard]] std::string semantic_family(const aarch64::DecodedInstruction& instruction)
{
    using Id = aarch64::InstructionId;
    switch (instruction.id)
    {
    case Id::Ldxr: case Id::Ldxrb: case Id::Ldxrh: case Id::Stxr: case Id::Stxrb:
    case Id::Stxrh: case Id::Ldaxr: case Id::Ldaxrb: case Id::Ldaxrh: case Id::Stlxr:
    case Id::Stlxrb: case Id::Stlxrh: case Id::Ldar: case Id::Ldarb: case Id::Ldarh:
    case Id::Stlr: case Id::Stlrb: case Id::Stlrh: case Id::Ldxp: case Id::Ldaxp:
    case Id::Stxp: case Id::Stlxp: return "atomic";
    case Id::Dmb: case Id::Dsb: case Id::Isb: return "barrier";
    case Id::Mrs: case Id::Msr: case Id::Svc: case Id::Brk: case Id::Hlt: case Id::Hvc:
    case Id::Smc: case Id::Eret: return "system";
    case Id::Ldr: case Id::Ldrb: case Id::Ldrh: case Id::Ldrsb: case Id::Ldrsh:
    case Id::Ldrsw: case Id::Str: case Id::Strb: case Id::Strh: case Id::Ldp: case Id::Stp:
    case Id::Ldur: case Id::Stur: case Id::LdrLiteral: return "memory";
    case Id::Adr: case Id::Adrp: return "address_formation";
    case Id::B: case Id::Bl: case Id::BCond: case Id::Br: case Id::Blr: case Id::Ret:
    case Id::Cbz: case Id::Cbnz: case Id::Tbz: case Id::Tbnz: return "branch";
    case Id::FpSimd: return "fp_simd";
    case Id::Unknown: return "unsupported";
    default: return "integer";
    }
}

[[nodiscard]] FunctionDiagnostic diagnostic_from_error(const Error& error, GuestAddress pc)
{
    FunctionDiagnostic diagnostic;
    diagnostic.code = error.code;
    diagnostic.pc = pc;
    diagnostic.message = error.message;
    switch (error.code)
    {
    case ErrorCode::UnsupportedInstruction:
    case ErrorCode::UnsupportedOperandForm:
    case ErrorCode::Unsupported: diagnostic.category = FailureCategory::UnsupportedInstruction; break;
    case ErrorCode::IrVerificationFailed: diagnostic.category = FailureCategory::IRVerificationFailure; break;
    case ErrorCode::InvalidBranchTarget:
    case ErrorCode::InvalidGuestAddress:
    case ErrorCode::MisalignedInstructionAddress:
    case ErrorCode::NonExecutableAddress: diagnostic.category = FailureCategory::InvalidGuestAddress; break;
    case ErrorCode::UnresolvedIndirectFlow: diagnostic.category = FailureCategory::UnresolvedIndirectFlow; break;
    case ErrorCode::FunctionBoundaryConflict: diagnostic.category = FailureCategory::FunctionBoundaryConflict; break;
    case ErrorCode::MissingImportBinding: diagnostic.category = FailureCategory::MissingImportBinding; break;
    case ErrorCode::RuntimeBoundary: diagnostic.category = FailureCategory::RuntimeBoundary; break;
    case ErrorCode::CodegenFailure:
    case ErrorCode::LlvmVerificationFailed:
    case ErrorCode::JitCompilationFailed: diagnostic.category = FailureCategory::CodegenFailure; break;
    default: diagnostic.category = FailureCategory::InvalidCFG; break;
    }
    return diagnostic;
}

[[nodiscard]] std::vector<GuestAddressRange> executable_ranges(const memory::GuestMemory& memory)
{
    std::vector<GuestAddressRange> ranges;
    for (const auto& region : memory.regions())
    {
        if (memory::has_permission(region.permissions, memory::GuestMemoryPermissions::Execute) &&
            region.size != 0U)
        {
            ranges.push_back(GuestAddressRange{region.base, region.size});
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.base < right.base;
    });
    return ranges;
}

[[nodiscard]] Result<GuestAddress> resolved_symbol_address(const format::DynamicSymbol& symbol,
                                                            GuestAddress module_base)
{
    constexpr std::uint16_t shn_abs = 0xfff1U;
    if (symbol.section_index == shn_abs)
    {
        return Result<GuestAddress>::success(symbol.value);
    }
    const auto address = checked_add_u64(module_base, symbol.value);
    if (!address)
    {
        return Result<GuestAddress>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "dynamic symbol value plus module base overflows"));
    }
    return address;
}

[[nodiscard]] json range_json(const GuestAddressRange& range)
{
    const auto end = checked_add_u64(range.base, range.size);
    return json{{"base", hex_address(range.base)}, {"size", range.size},
                {"end", end ? hex_address(end.value()) : "<overflow>"}};
}

[[nodiscard]] json identity_json(const ModuleIdentity& identity)
{
    json ranges = json::array();
    for (const auto& range : identity.executable_ranges)
    {
        ranges.push_back(range_json(range));
    }
    json flags = json::array();
    for (const auto& flag : identity.feature_flags)
    {
        flags.push_back(flag);
    }
    return json{{"module", identity.module},
                {"build_id", identity.build_id},
                {"sha256", identity.input_sha256},
                {"guest_base", hex_address(identity.guest_base)},
                {"executable_ranges", std::move(ranges)},
                {"translator_version", identity.translator_version},
                {"metadata_schema_version", identity.metadata_schema_version},
                {"llvm_version", identity.llvm_version},
                {"feature_flags", std::move(flags)}};
}

[[nodiscard]] json call_json(const CallSite& call)
{
    json value{{"address", hex_address(call.address)}, {"kind", call_kind_name(call.kind)}};
    if (call.target)
    {
        value["target"] = hex_address(call.target.value());
    }
    if (call.register_target)
    {
        value["register_target"] = aarch64::register_name(call.register_target.value());
    }
    return value;
}

[[nodiscard]] json unsupported_json(const UnsupportedRecord& record)
{
    return json{{"module", record.module},
                {"function", record.function},
                {"pc", hex_address(record.pc)},
                {"opcode", hex_opcode(record.opcode)},
                {"instruction", record.instruction},
                {"semantic_family", record.semantic_family},
                {"reason", record.reason},
                {"required_action", record.required_action}};
}

[[nodiscard]] json diagnostic_json(const FunctionDiagnostic& diagnostic)
{
    json value{{"category", failure_category_name(diagnostic.category)},
                {"code", error_code_name(diagnostic.code)},
                {"pc", hex_address(diagnostic.pc)},
                {"message", diagnostic.message}};
    if (diagnostic.opcode)
    {
        value["opcode"] = hex_opcode(diagnostic.opcode.value());
    }
    return value;
}

[[nodiscard]] json function_json(const FunctionRecord& function)
{
    json entries = json::array();
    for (const auto entry : function.entries)
    {
        entries.push_back(hex_address(entry));
    }
    json evidence = json::array();
    for (const auto& item : function.evidence)
    {
        evidence.push_back(json{{"source", function_discovery_source_name(item.source)},
                                {"confidence", function_confidence_name(item.confidence)},
                                {"entry", hex_address(item.entry)}, {"note", item.note}});
    }
    json direct_calls = json::array();
    for (const auto target : function.direct_calls)
    {
        direct_calls.push_back(hex_address(target));
    }
    json indirect_calls = json::array();
    for (const auto& call : function.indirect_calls)
    {
        indirect_calls.push_back(call_json(call));
    }
    json unresolved = json::array();
    for (const auto& item : function.unresolved_control_flow)
    {
        unresolved.push_back(json{{"pc", hex_address(item.address)},
                                  {"kind", aarch64::control_flow_kind_name(item.kind)},
                                  {"reason", item.reason}});
    }
    json unsupported = json::array();
    for (const auto& item : function.unsupported)
    {
        unsupported.push_back(unsupported_json(item));
    }
    json diagnostics = json::array();
    for (const auto& item : function.diagnostics)
    {
        diagnostics.push_back(diagnostic_json(item));
    }
    json blocks = json::array();
    std::size_t instructions = 0U;
    if (function.cfg)
    {
        instructions = function.cfg->instruction_count;
        for (const auto& [start, block] : function.cfg->blocks)
        {
            json decoded = json::array();
            for (const auto& instruction : block.instructions)
            {
                decoded.push_back(json{{"pc", hex_address(instruction.address)},
                                       {"opcode", hex_opcode(instruction.opcode)},
                                       {"id", aarch64::instruction_id_name(instruction.id)},
                                       {"instruction", instruction.disassembly}});
            }
            json successors = json::array();
            for (const auto& edge : block.successors)
            {
                successors.push_back(json{{"source", hex_address(edge.source)},
                                          {"target", hex_address(edge.target)},
                                          {"kind", edge_kind_name(edge.kind)},
                                          {"internal", edge.internal}});
            }
            json block_calls = json::array();
            for (const auto& call : block.calls)
            {
                block_calls.push_back(call_json(call));
            }
            blocks.push_back(json{{"start", hex_address(start)},
                                  {"instructions", std::move(decoded)},
                                  {"successors", std::move(successors)},
                                  {"calls", std::move(block_calls)},
                                  {"termination", block.termination}});
        }
    }
    json value{{"module", function.module},
               {"id", function.synthetic_id},
               {"canonical_entry", hex_address(function.canonical_entry)},
               {"entries", std::move(entries)},
               {"range", json{{"begin", hex_address(function.range_begin)},
                                {"end", hex_address(function.range_end)}}},
               {"source", function_discovery_source_name(function.primary_source)},
               {"confidence", function_confidence_name(function.confidence)},
               {"name", function.name.value_or(function.synthetic_id)},
               {"evidence", std::move(evidence)},
               {"block_count", blocks.size()},
               {"blocks", std::move(blocks)},
               {"instructions", instructions},
               {"direct_calls", std::move(direct_calls)},
               {"indirect_calls", std::move(indirect_calls)},
               {"unresolved_control_flow", std::move(unresolved)},
               {"translation_status", translation_status_name(function.translation_status)},
               {"unsupported", std::move(unsupported)},
               {"diagnostics", std::move(diagnostics)}};
    return value;
}

[[nodiscard]] json coverage_json(const ModuleCoverage& coverage)
{
    json families = json::array();
    for (const auto& family : coverage.families)
    {
        families.push_back(json{{"family", family.family}, {"decoded", family.decoded},
                                {"supported", family.supported}, {"unsupported", family.unsupported}});
    }
    json top = json::array();
    for (const auto& item : coverage.top_unsupported)
    {
        json examples = json::array();
        for (const auto pc : item.example_pcs)
        {
            examples.push_back(hex_address(pc));
        }
        top.push_back(json{{"instruction", item.instruction},
                           {"semantic_family", item.semantic_family},
                           {"count", item.count},
                           {"functions", item.functions},
                           {"example_pcs", std::move(examples)}});
    }
    return json{{"executable_bytes", coverage.executable_bytes},
                {"decoded_instructions", coverage.decoded_instructions},
                {"supported_instructions", coverage.supported_instructions},
                {"unsupported_instructions", coverage.unsupported_instructions},
                {"decode_failures", coverage.decode_failures},
                {"function_seeds", coverage.function_seeds},
                {"functions_discovered", coverage.functions_discovered},
                {"functions_analyzed", coverage.functions_analyzed},
                {"functions_fully_lifted", coverage.functions_fully_lifted},
                {"functions_verified", coverage.functions_verified},
                {"functions_translated", coverage.functions_translated},
                {"functions_unsupported", coverage.functions_unsupported},
                {"functions_failed", coverage.functions_failed},
                {"basic_blocks", coverage.basic_blocks},
                {"cfg_edges", coverage.cfg_edges},
                {"direct_calls", coverage.direct_calls},
                {"indirect_calls", coverage.indirect_calls},
                {"resolved_indirect_calls", coverage.resolved_indirect_calls},
                {"unresolved_indirect_calls", coverage.unresolved_indirect_calls},
                {"ir_verification_failures", coverage.ir_verification_failures},
                {"runtime_import_boundaries", coverage.runtime_import_boundaries},
                {"families", std::move(families)},
                {"top_unsupported", std::move(top)}};
}

[[nodiscard]] json translation_function_json(const FunctionTranslationResult& function)
{
    json unsupported = json::array();
    for (const auto& item : function.unsupported)
    {
        unsupported.push_back(unsupported_json(item));
    }
    json diagnostics = json::array();
    for (const auto& item : function.diagnostics)
    {
        diagnostics.push_back(diagnostic_json(item));
    }
    json value{{"module", function.module},
               {"function", function.function},
               {"guest_entry", hex_address(function.guest_entry)},
               {"status", translation_status_name(function.status)},
               {"unsupported", std::move(unsupported)},
               {"diagnostics", std::move(diagnostics)}};
    if (!function.ir_text.empty())
    {
        value["ir"] = function.ir_text;
    }
    if (!function.llvm_ir.empty())
    {
        value["llvm_ir"] = function.llvm_ir;
    }
    return value;
}

[[nodiscard]] std::string name_for(const FunctionRecord& function)
{
    return function.name.value_or(function.synthetic_id);
}

[[nodiscard]] Result<WholeModuleTranslationResult> translate_with_map(
    const FinalizedFunctionMap& map, const TranslationOptions& options,
    std::vector<format::ImportSymbol> imports = {},
    std::vector<format::Relocation> relocations = {},
    std::vector<format::DynamicSymbol> symbols = {})
{
    WholeModuleTranslationResult result;
    result.identity = map.identity();
    result.mode = options.mode;
    result.function_map = map;
    result.symbols = std::move(symbols);
    result.unresolved_imports = std::move(imports);
    result.relocations = std::move(relocations);
    result.functions.reserve(map.functions().size());
    result.coverage.executable_bytes = 0U;
    for (const auto& range : map.identity().executable_ranges)
    {
        if (range.size > static_cast<memory::GuestSize>(std::numeric_limits<std::size_t>::max()) -
                           result.coverage.executable_bytes)
        {
            return Result<WholeModuleTranslationResult>::failure(make_error(
                ErrorCode::AnalysisBudgetExceeded, "executable byte coverage exceeds host size limits"));
        }
        result.coverage.executable_bytes += static_cast<std::size_t>(range.size);
    }
    result.coverage.functions_discovered = map.functions().size();
    for (const auto& function : map.functions())
    {
        result.coverage.function_seeds += function.entries.size();
        for (const auto& diagnostic : function.diagnostics)
        {
            if (diagnostic.category == FailureCategory::DecodeFailure)
            {
                ++result.coverage.decode_failures;
            }
        }
    }
    result.coverage.runtime_import_boundaries = result.unresolved_imports.size();
    result.strict_success = map.conflicts().empty() && result.unresolved_imports.empty();
    for (const auto& function : map.functions())
    {
        if (function.cfg)
        {
            ++result.coverage.functions_analyzed;
            result.coverage.basic_blocks += function.cfg->blocks.size();
            for (const auto& [unused, block] : function.cfg->blocks)
            {
                (void)unused;
                result.coverage.cfg_edges += block.successors.size();
            }
        }
        result.coverage.direct_calls += function.direct_calls.size();
        result.coverage.indirect_calls += function.indirect_calls.size();
        for (const auto& call : function.indirect_calls)
        {
            if (call.target)
            {
                ++result.coverage.resolved_indirect_calls;
            }
            else
            {
                ++result.coverage.unresolved_indirect_calls;
            }
        }
        for (const auto& item : function.unresolved_control_flow)
        {
            if (item.kind == aarch64::ControlFlowKind::IndirectBranch)
            {
                ++result.coverage.unresolved_indirect_calls;
            }
        }
    }

    std::map<std::string, CoverageFamily> families;
    struct UnsupportedAccumulator
    {
        UnsupportedSummary summary;
        std::set<std::string> functions;
    };
    std::map<std::string, UnsupportedAccumulator> unsupported;

    bool stop = false;
    for (const auto& function : map.functions())
    {
        FunctionTranslationResult translated;
        translated.module = function.module;
        translated.function = name_for(function);
        translated.guest_entry = function.canonical_entry;
        if (stop)
        {
            translated.status = TranslationStatus::Excluded;
            translated.diagnostics.push_back(FunctionDiagnostic{
                FailureCategory::InvalidCFG, ErrorCode::InvalidControlFlow, function.canonical_entry,
                std::nullopt, "excluded because strict translation stopped at an earlier failure"});
            result.functions.push_back(std::move(translated));
            continue;
        }
        if (function.translation_status == TranslationStatus::Conflict)
        {
            translated.status = TranslationStatus::Conflict;
            translated.diagnostics.push_back(FunctionDiagnostic{
                FailureCategory::FunctionBoundaryConflict, ErrorCode::FunctionBoundaryConflict,
                function.canonical_entry, std::nullopt,
                "function has a boundary conflict recorded in the canonical function map"});
            ++result.coverage.functions_failed;
            result.strict_success = false;
            result.functions.push_back(std::move(translated));
            if (options.mode == TranslationMode::Strict) stop = true;
            continue;
        }
        if (!function.cfg)
        {
            translated.status = function.translation_status;
            translated.diagnostics = function.diagnostics;
            ++result.coverage.functions_failed;
            result.strict_success = false;
            result.functions.push_back(std::move(translated));
            if (options.mode == TranslationMode::Strict) stop = true;
            continue;
        }

        for (const auto& [unused, block] : function.cfg->blocks)
        {
            (void)unused;
            for (const auto& instruction : block.instructions)
            {
                auto& family = families[semantic_family(instruction)];
                family.family = semantic_family(instruction);
                ++family.decoded;
                ++result.coverage.decoded_instructions;
                if (lifter::is_instruction_liftable(instruction))
                {
                    ++family.supported;
                    ++result.coverage.supported_instructions;
                }
                else
                {
                    ++family.unsupported;
                    ++result.coverage.unsupported_instructions;
                    UnsupportedRecord record;
                    record.module = function.module;
                    record.function = name_for(function);
                    record.pc = instruction.address;
                    record.opcode = instruction.opcode;
                    record.instruction = instruction.disassembly;
                    record.semantic_family = semantic_family(instruction);
                    record.reason = "no complete decoder → Semantic IR → verifier → interpreter chain";
                    record.required_action = "implement semantic support or provide reviewed metadata";
                    translated.unsupported.push_back(record);
                    const auto key = std::string(aarch64::instruction_id_name(instruction.id)) +
                                     "\n" + record.semantic_family;
                    auto& aggregate = unsupported[key];
                    aggregate.summary.instruction = aarch64::instruction_id_name(instruction.id);
                    aggregate.summary.semantic_family = record.semantic_family;
                    ++aggregate.summary.count;
                    aggregate.functions.insert(record.function);
                    if (aggregate.summary.example_pcs.size() < 3U)
                    {
                        aggregate.summary.example_pcs.push_back(record.pc);
                    }
                }
            }
        }

        if (!translated.unsupported.empty())
        {
            translated.status = TranslationStatus::Unsupported;
            ++result.coverage.functions_unsupported;
            result.strict_success = false;
            result.functions.push_back(std::move(translated));
            if (options.mode == TranslationMode::Strict) stop = true;
            continue;
        }

        const auto lifted = lifter::lift_function(function.cfg.value(), options.lift);
        if (!lifted)
        {
            translated.diagnostics.push_back(diagnostic_from_error(lifted.error(), function.canonical_entry));
            translated.status = lifted.error().code == ErrorCode::UnsupportedInstruction ||
                                        lifted.error().code == ErrorCode::UnsupportedOperandForm
                                    ? TranslationStatus::Unsupported
                                    : TranslationStatus::Failed;
            if (translated.status == TranslationStatus::Unsupported)
            {
                ++result.coverage.functions_unsupported;
            }
            else
            {
                ++result.coverage.functions_failed;
            }
            if (lifted.error().code == ErrorCode::IrVerificationFailed)
            {
                ++result.coverage.ir_verification_failures;
            }
            result.strict_success = false;
            result.functions.push_back(std::move(translated));
            if (options.mode == TranslationMode::Strict) stop = true;
            continue;
        }
        translated.status = TranslationStatus::Lifted;
        ++result.coverage.functions_fully_lifted;
        if (options.include_ir)
        {
            translated.ir_text = ir::print(lifted.value());
        }
        translated.ir = std::move(lifted.value());
        translated.status = TranslationStatus::Verified;
        ++result.coverage.functions_verified;

        if (options.lower_llvm)
        {
#ifdef TOTKRECOMP_HAS_LLVM
            const auto backend = codegen::LlvmBackend::create();
            if (!backend)
            {
                translated.status = TranslationStatus::Failed;
                translated.diagnostics.push_back(
                    diagnostic_from_error(backend.error(), function.canonical_entry));
            }
            else
            {
                const auto llvm_ir = backend.value()->lower_to_llvm_ir(translated.ir.value());
                if (!llvm_ir)
                {
                    translated.status = TranslationStatus::Failed;
                    translated.diagnostics.push_back(
                        diagnostic_from_error(llvm_ir.error(), function.canonical_entry));
                }
                else
                {
                    translated.llvm_ir = llvm_ir.value();
                    translated.status = TranslationStatus::Lowerable;
                }
            }
#else
            translated.status = TranslationStatus::Failed;
            translated.diagnostics.push_back(FunctionDiagnostic{
                FailureCategory::CodegenFailure, ErrorCode::CodegenFailure, function.canonical_entry,
                std::nullopt, "LLVM lowering was requested but this build has no LLVM backend"});
#endif
            if (translated.status == TranslationStatus::Failed)
            {
                ++result.coverage.functions_failed;
                result.strict_success = false;
                result.functions.push_back(std::move(translated));
                if (options.mode == TranslationMode::Strict) stop = true;
                continue;
            }
        }
        translated.status = TranslationStatus::Translated;
        ++result.coverage.functions_translated;
        result.functions.push_back(std::move(translated));
    }

    for (auto& [unused, family] : families)
    {
        (void)unused;
        result.coverage.families.push_back(std::move(family));
    }
    for (auto& [unused, aggregate] : unsupported)
    {
        (void)unused;
        aggregate.summary.functions = aggregate.functions.size();
        std::sort(aggregate.summary.example_pcs.begin(), aggregate.summary.example_pcs.end());
        result.coverage.top_unsupported.push_back(std::move(aggregate.summary));
    }
    std::sort(result.coverage.top_unsupported.begin(), result.coverage.top_unsupported.end(),
              [](const auto& left, const auto& right) {
                  if (left.count != right.count) return left.count > right.count;
                  if (left.instruction != right.instruction) return left.instruction < right.instruction;
                  return left.semantic_family < right.semantic_family;
              });
    result.strict_success = result.strict_success &&
                            result.coverage.functions_failed == 0U &&
                            result.coverage.functions_unsupported == 0U &&
                            result.coverage.functions_translated == result.coverage.functions_discovered;
    return Result<WholeModuleTranslationResult>::success(std::move(result));
}

} // namespace

std::string_view translation_mode_name(TranslationMode mode) noexcept
{
    switch (mode)
    {
    case TranslationMode::Strict: return "strict";
    case TranslationMode::Diagnostic: return "diagnostic";
    }
    return "unknown";
}

Result<LoadedModule> load_prepared_nso(std::span<const std::byte> file_bytes,
                                       const PreparedModuleOptions& options)
{
    const auto digest = sha256_bytes(file_bytes);
    if (!digest)
    {
        return Result<LoadedModule>::failure(digest.error());
    }
    const auto header = format::parse_nso_header(file_bytes);
    if (!header)
    {
        return Result<LoadedModule>::failure(header.error());
    }
    const auto image = format::materialize_nso(file_bytes, header.value(), options.materialization_limits);
    if (!image)
    {
        return Result<LoadedModule>::failure(image.error());
    }

    memory::GuestMemory guest_memory(options.memory_limits);
    const auto loaded = loader::load_nso(image.value(), guest_memory,
                                         loader::NsoGuestLoadOptions{options.module_base});
    if (!loaded)
    {
        return Result<LoadedModule>::failure(loaded.error());
    }
    const auto metadata = format::parse_module_metadata(guest_memory, options.module_base,
                                                         options.metadata_options);
    if (!metadata)
    {
        return Result<LoadedModule>::failure(metadata.error());
    }

    std::optional<format::DynamicSymbolTable> symbols;
    std::vector<format::Relocation> relocations;
    if (metadata.value().dynamic)
    {
        if (metadata.value().dynamic->symtab)
        {
            const auto parsed_symbols = format::DynamicSymbolTable::parse(
                guest_memory, metadata.value().dynamic.value(), options.metadata_options.dynamic);
            if (!parsed_symbols)
            {
                return Result<LoadedModule>::failure(parsed_symbols.error());
            }
            symbols = std::move(parsed_symbols.value());
        }

        const auto rela_entries = format::parse_rela_table(
            guest_memory, metadata.value().dynamic.value(), options.metadata_options.dynamic);
        if (!rela_entries)
        {
            return Result<LoadedModule>::failure(rela_entries.error());
        }
        const auto rela = format::make_relocations(rela_entries.value());
        if (!rela)
        {
            return Result<LoadedModule>::failure(rela.error());
        }
        relocations = std::move(rela.value());

        const auto jmprel_entries = format::parse_jmprel_table(
            guest_memory, metadata.value().dynamic.value(), options.metadata_options.dynamic);
        if (!jmprel_entries)
        {
            return Result<LoadedModule>::failure(jmprel_entries.error());
        }
        const auto jmprel = format::make_relocations(jmprel_entries.value());
        if (!jmprel)
        {
            return Result<LoadedModule>::failure(jmprel.error());
        }
        relocations.insert(relocations.end(), jmprel.value().begin(), jmprel.value().end());
        if (!relocations.empty() && !symbols)
        {
            return Result<LoadedModule>::failure(make_error(
                ErrorCode::MissingImportBinding,
                "dynamic relocations are present without a dynamic symbol table"));
        }
        if (options.apply_relocations && !relocations.empty())
        {
            loader::SymbolResolver resolver(symbols.value(), options.module_base);
            const auto applied = loader::apply_relocations(guest_memory, relocations, resolver,
                                                            options.relocation_options);
            if (!applied)
            {
                return Result<LoadedModule>::failure(applied.error());
            }
        }
    }

    const auto ranges = executable_ranges(guest_memory);
    if (ranges.empty())
    {
        return Result<LoadedModule>::failure(make_error(
            ErrorCode::InvalidFormat, "prepared NSO has no executable mapping"));
    }
    ModuleIdentity identity;
    identity.module = options.module_name.empty() ? "main" : options.module_name;
    identity.build_id = format::module_id_hex(header.value());
    identity.input_sha256 = sha256_to_hex(digest.value());
    identity.guest_base = options.module_base;
    identity.executable_ranges = ranges;
    identity.translator_version = switchrecomp::version;
    identity.metadata_schema_version = 1U;
#ifdef TOTKRECOMP_HAS_LLVM
    identity.llvm_version = "LLVM 18";
#else
    identity.llvm_version = "disabled";
#endif
    identity.feature_flags = {"whole_module_analysis", "semantic_ir", "strict_unsupported"};

    LoadedModule result{std::move(identity), std::move(image).value(), std::move(guest_memory),
                        std::move(metadata).value(), std::move(symbols), std::move(relocations), {}, {}};
    if (result.symbols)
    {
        result.unresolved_imports = result.symbols->imports();
    }
    if (options.seed_text_entry)
    {
        const auto text_entry = checked_add_u64(options.module_base, result.image.text.memory_offset);
        if (!text_entry)
        {
            return Result<LoadedModule>::failure(make_error(
                ErrorCode::ArithmeticOverflow, "module base plus text offset overflows"));
        }
        result.seeds.push_back(FunctionSeed{text_entry.value(), FunctionDiscoverySource::ModuleEntry,
                                            FunctionConfidence::Confirmed, std::nullopt, std::nullopt,
                                            "prepared NSO text entry candidate"});
    }
    if (result.metadata.dynamic)
    {
        const auto add_pointer_seed = [&](const auto& pointer, FunctionDiscoverySource source,
                                          FunctionConfidence confidence, std::string note) {
            if (pointer && pointer->address != 0U)
            {
                result.seeds.push_back(FunctionSeed{pointer->address, source, confidence,
                                                    std::nullopt, std::nullopt, std::move(note)});
            }
        };
        add_pointer_seed(result.metadata.dynamic->init, FunctionDiscoverySource::AnalystSeed,
                         FunctionConfidence::High, "DT_INIT candidate");
        add_pointer_seed(result.metadata.dynamic->fini, FunctionDiscoverySource::AnalystSeed,
                         FunctionConfidence::High, "DT_FINI candidate");
    }
    if (result.symbols)
    {
        for (const auto& symbol : result.symbols->symbols)
        {
            if (!symbol.is_defined() || symbol.type != format::SymbolType::Function || symbol.value == 0U)
            {
                continue;
            }
            const auto address = resolved_symbol_address(symbol, options.module_base);
            if (!address)
            {
                return Result<LoadedModule>::failure(address.error());
            }
            result.seeds.push_back(FunctionSeed{address.value(), FunctionDiscoverySource::DynamicSymbol,
                                                FunctionConfidence::Confirmed, std::nullopt,
                                                symbol.name.empty() ? std::nullopt
                                                                    : std::optional<std::string>(symbol.name),
                                                "defined dynamic function symbol"});
        }
        for (const auto& relocation : result.relocations)
        {
            const auto* symbol = result.symbols->at(relocation.symbol_index);
            if (symbol == nullptr || !symbol->is_defined() || symbol->type != format::SymbolType::Function)
            {
                continue;
            }
            const auto address = resolved_symbol_address(*symbol, options.module_base);
            if (!address)
            {
                return Result<LoadedModule>::failure(address.error());
            }
            result.seeds.push_back(FunctionSeed{address.value(), FunctionDiscoverySource::RelocationReference,
                                                FunctionConfidence::High, std::nullopt,
                                                symbol->name.empty() ? std::nullopt
                                                                     : std::optional<std::string>(symbol->name),
                                                "relocation references a defined function symbol"});
        }
    }
    result.seeds.insert(result.seeds.end(), options.seeds.begin(), options.seeds.end());
    return Result<LoadedModule>::success(std::move(result));
}

Result<WholeModuleTranslationResult> translate_module(const ModuleAnalysisInput& input,
                                                       const TranslationOptions& options)
{
    const auto map = FunctionMapBuilder::build(input, options.function_map);
    if (!map)
    {
        return Result<WholeModuleTranslationResult>::failure(map.error());
    }
    return translate_with_map(map.value(), options);
}

Result<WholeModuleTranslationResult> translate_module(const LoadedModule& module,
                                                       const TranslationOptions& options)
{
    const ModuleAnalysisInput input{module.identity, &module.memory, module.seeds};
    const auto translated = translate_module(input, options);
    if (!translated)
    {
        return translated;
    }
    auto result = std::move(translated).value();
    result.symbols = module.symbols ? module.symbols->symbols : std::vector<format::DynamicSymbol>{};
    result.unresolved_imports = module.unresolved_imports;
    result.relocations = module.relocations;
    result.coverage.runtime_import_boundaries = result.unresolved_imports.size();
    result.strict_success = result.strict_success && result.unresolved_imports.empty();
    return Result<WholeModuleTranslationResult>::success(std::move(result));
}

std::string render_function_map_json(const FinalizedFunctionMap& map)
{
    json functions = json::array();
    for (const auto& function : map.functions())
    {
        functions.push_back(function_json(function));
    }
    json conflicts = json::array();
    for (const auto& conflict : map.conflicts())
    {
        conflicts.push_back(json{{"module", conflict.module},
                                 {"first_function", hex_address(conflict.first_function)},
                                 {"second_function", hex_address(conflict.second_function)},
                                 {"first_range", range_json(conflict.first_range)},
                                 {"second_range", range_json(conflict.second_range)},
                                 {"first_source", function_discovery_source_name(conflict.first_source)},
                                 {"second_source", function_discovery_source_name(conflict.second_source)},
                                 {"first_confidence", function_confidence_name(conflict.first_confidence)},
                                 {"second_confidence", function_confidence_name(conflict.second_confidence)},
                                 {"resolution", conflict.resolution}});
    }
    return json{{"schema_version", 1U},
                {"input", identity_json(map.identity())},
                {"module", map.identity().module},
                {"functions", std::move(functions)},
                {"conflicts", std::move(conflicts)}}
        .dump(2);
}

std::string render_translation_report_json(const WholeModuleTranslationResult& result)
{
    json functions = json::array();
    for (const auto& function : result.functions)
    {
        functions.push_back(translation_function_json(function));
    }
    json imports = json::array();
    for (const auto& import : result.unresolved_imports)
    {
        imports.push_back(json{{"symbol_index", import.symbol_index}, {"name", import.name},
                               {"binding", format::symbol_binding_name(import.binding)},
                               {"type", format::symbol_type_name(import.type)}});
    }
    json calls = json::array();
    for (const auto& function : result.function_map.functions())
    {
        for (const auto target : function.direct_calls)
        {
            calls.push_back(json{{"module", function.module},
                                 {"function", function.synthetic_id},
                                 {"kind", "direct"},
                                 {"target", hex_address(target)}});
        }
        for (const auto& call : function.indirect_calls)
        {
            auto value = call_json(call);
            value["module"] = function.module;
            value["function"] = function.synthetic_id;
            calls.push_back(std::move(value));
        }
    }
    json unsupported = json::array();
    for (const auto& function : result.functions)
    {
        for (const auto& record : function.unsupported)
        {
            unsupported.push_back(unsupported_json(record));
        }
    }
    return json{{"schema_version", 1U},
                {"input", identity_json(result.identity)},
                {"module", result.identity.module},
                {"mode", translation_mode_name(result.mode)},
                {"strict_success", result.strict_success},
                {"function_map", json::parse(render_function_map_json(result.function_map))},
                {"functions", std::move(functions)},
                {"calls", std::move(calls)},
                {"unsupported", std::move(unsupported)},
                {"symbols", [&result]() {
                    json values = json::array();
                    for (const auto& symbol : result.symbols)
                    {
                        values.push_back(json{{"index", symbol.index}, {"name", symbol.name},
                                              {"binding", format::symbol_binding_name(symbol.binding)},
                                              {"type", format::symbol_type_name(symbol.type)},
                                              {"visibility", format::symbol_visibility_name(symbol.visibility)},
                                              {"value", hex_address(symbol.value)}, {"size", symbol.size}});
                    }
                    return values;
                }()},
                {"imports", std::move(imports)},
                {"relocations", [&result]() {
                    json values = json::array();
                    for (const auto& relocation : result.relocations)
                    {
                        values.push_back(json{{"offset", hex_address(relocation.offset)},
                                              {"target", hex_address(relocation.target_address)},
                                              {"raw_type", relocation.raw_type},
                                              {"type", format::aarch64_relocation_type_name(relocation.type)},
                                              {"symbol_index", relocation.symbol_index},
                                              {"addend", relocation.addend}});
                    }
                    return values;
                }()},
                {"coverage", coverage_json(result.coverage)}}
        .dump(2);
}

std::string render_translation_report(const WholeModuleTranslationResult& result)
{
    std::ostringstream output;
    output << "Module\n------\n"
           << "Name: " << result.identity.module << '\n'
           << "Build ID: " << result.identity.build_id << '\n'
           << "SHA-256: " << result.identity.input_sha256 << '\n'
           << "Guest base: " << hex_address(result.identity.guest_base) << "\n\n"
           << "Analysis\n--------\n"
           << "Executable bytes: " << result.coverage.executable_bytes << '\n'
           << "Functions discovered: " << result.coverage.functions_discovered << '\n'
           << "Functions analyzed: " << result.coverage.functions_analyzed << '\n'
           << "Basic blocks: " << result.coverage.basic_blocks << '\n'
           << "Decoded instructions: " << result.coverage.decoded_instructions << '\n'
           << "Supported instructions: " << result.coverage.supported_instructions << '\n'
           << "Unsupported instructions: " << result.coverage.unsupported_instructions << "\n\n"
           << "Translation\n-----------\n"
           << "Functions lifted: " << result.coverage.functions_fully_lifted << '\n'
           << "Functions verified: " << result.coverage.functions_verified << '\n'
           << "Functions translated: " << result.coverage.functions_translated << '\n'
           << "Functions unsupported: " << result.coverage.functions_unsupported << '\n'
           << "Functions failed: " << result.coverage.functions_failed << "\n\n"
           << "Calls\n-----\n"
           << "Direct calls: " << result.coverage.direct_calls << '\n'
           << "Indirect calls: " << result.coverage.indirect_calls << '\n'
           << "Resolved indirect calls: " << result.coverage.resolved_indirect_calls << '\n'
           << "Unresolved indirect calls: " << result.coverage.unresolved_indirect_calls << "\n\n"
           << "Boundaries\n----------\n"
           << "Unresolved imports/runtime boundaries: " << result.coverage.runtime_import_boundaries << '\n'
           << "Function boundary conflicts: " << result.function_map.conflicts().size() << '\n'
           << "IR verification failures: " << result.coverage.ir_verification_failures << "\n\n"
           << "Top unsupported\n----------------\n";
    if (result.coverage.top_unsupported.empty())
    {
        output << "none\n";
    }
    else
    {
        for (const auto& item : result.coverage.top_unsupported)
        {
            output << item.instruction << " [" << item.semantic_family << "]: " << item.count
                   << " occurrences in " << item.functions << " functions; examples=";
            for (std::size_t index = 0U; index < item.example_pcs.size(); ++index)
            {
                if (index != 0U) output << ',';
                output << hex_address(item.example_pcs[index]);
            }
            output << '\n';
        }
    }
    output << "\nMode: " << translation_mode_name(result.mode)
           << " (" << (result.strict_success ? "strict-success" : "blocked") << ")\n";
    return output.str();
}

} // namespace switchrecomp::analysis
