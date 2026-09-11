#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/common/portable_arithmetic.hpp"
#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"

#ifdef TOTKRECOMP_HAS_LLVM
#include "switchrecomp/codegen/llvm_backend.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> bytes;
    bytes.reserve(values.size() * 4U);
    for (const auto word : values)
    {
        bytes.push_back(static_cast<std::byte>(word & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 8U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 16U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 24U) & 0xffU));
    }
    return bytes;
}

struct Fixture
{
    memory::GuestMemory memory;
    memory::GuestAddress address = 0x100000U;
    analysis::ControlFlowGraph cfg;
    ir::Function function;
};

[[nodiscard]] Result<Fixture> make_fixture(std::initializer_list<std::uint32_t> code,
                                           memory::GuestAddress address = 0x100000U)
{
    Fixture fixture;
    fixture.address = address;
    const auto bytes = words(code);
    const auto mapped = fixture.memory.map(
        address, std::span<const std::byte>(bytes),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m16.umulh.text", memory::GuestRegionKind::Text);
    if (!mapped)
        return Result<Fixture>::failure(mapped.error());

    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(fixture.memory, address, options);
    if (!cfg)
        return Result<Fixture>::failure(cfg.error());
    fixture.cfg = cfg.value();
    const auto function = lifter::lift_function(fixture.cfg);
    if (!function)
        return Result<Fixture>::failure(function.error());
    fixture.function = function.value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] Result<runtime::ExecutionResult> run(Fixture& fixture, runtime::CpuState& cpu)
{
    runtime::RuntimeContext context{&fixture.memory};
    return interpreter::execute(fixture.function, cpu, context);
}

// Independent test-only oracle: binary multiplication by repeated shift and
// add into four 32-bit limbs. It intentionally does not share the production
// decomposition or any compiler-specific 128-bit extension.
[[nodiscard]] std::uint64_t oracle_high64(std::uint64_t left, std::uint64_t right)
{
    std::array<std::uint32_t, 4> product{};
    std::array<std::uint32_t, 4> addend{
        static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(left >> 32U), 0U, 0U};

    const auto add_limbs = [](std::array<std::uint32_t, 4>& destination,
                              const std::array<std::uint32_t, 4>& source) {
        std::uint64_t carry = 0U;
        for (std::size_t index = 0U; index < destination.size(); ++index)
        {
            const auto sum = static_cast<std::uint64_t>(destination[index]) + source[index] + carry;
            destination[index] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32U;
        }
    };

    for (unsigned int bit = 0U; bit < 64U; ++bit)
    {
        if (((right >> bit) & 1U) != 0U)
            add_limbs(product, addend);

        std::uint32_t carry = 0U;
        for (auto& limb : addend)
        {
            const auto shifted = (static_cast<std::uint64_t>(limb) << 1U) | carry;
            limb = static_cast<std::uint32_t>(shifted);
            carry = static_cast<std::uint32_t>(shifted >> 32U);
        }
    }
    return static_cast<std::uint64_t>(product[2]) |
           (static_cast<std::uint64_t>(product[3]) << 32U);
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void write_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
{
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
    write_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] std::vector<std::byte> synthetic_provider_nso(bool defines_provider,
                                                             bool caller,
                                                             bool provider_umulh)
{
    constexpr std::size_t text_file = 0x100U;
    constexpr std::size_t rodata_file = 0x200U;
    constexpr std::size_t data_file = 0x500U;
    constexpr std::size_t data_offset = 0x8000U;
    constexpr std::size_t string_offset = 0x1a0U;
    constexpr std::size_t hash_offset = 0x1c0U;
    constexpr std::size_t symbol_offset = 0x200U;
    constexpr std::size_t relocation_offset = 0x260U;
    const std::string symbol_name = "__nnmusl_init_dso";
    std::vector<std::byte> file(0x600U, std::byte{0});
    file[0] = std::byte{'N'};
    file[1] = std::byte{'S'};
    file[2] = std::byte{'O'};
    file[3] = std::byte{'0'};
    write_u32(file, 0x10U, text_file);
    write_u32(file, 0x18U, 0x100U);
    write_u32(file, 0x20U, rodata_file);
    write_u32(file, 0x24U, 0x100U);
    write_u32(file, 0x28U, 0x300U);
    write_u32(file, 0x30U, data_file);
    write_u32(file, 0x34U, static_cast<std::uint32_t>(data_offset));
    write_u32(file, 0x38U, 0x100U);
    write_u32(file, text_file + 4U, 0x20U);
    write_u32(file, text_file + 0x20U, format::mod0_magic);
    write_u32(file, text_file + 0x24U, 0xe0U);
    write_u32(file, text_file + 0x28U, 0x7fe0U);
    write_u32(file, text_file + 0x2cU, 0x7fe0U);
    write_u32(file, text_file + 0x30U, 0x7fe0U);
    write_u32(file, text_file + 0x34U, 0x7fe0U);
    write_u32(file, text_file + 0x38U, 0x7fe0U);
    write_u32(file, 0x60U, 0x100U);
    write_u32(file, 0x64U, 0x300U);
    write_u32(file, 0x68U, 0x100U);

    if (defines_provider && provider_umulh)
    {
        write_u32(file, text_file + 0x40U, 0xd2e00021U); // movz x1, #1, lsl #48
        write_u32(file, text_file + 0x44U, 0xd2e00022U); // movz x2, #1, lsl #48
        write_u32(file, text_file + 0x48U, 0x9bc27c20U); // umulh x0, x1, x2
        write_u32(file, text_file + 0x4cU, 0xd65f03c0U); // ret
    }
    else
    {
        write_u32(file, text_file + 0x40U, 0xd65f03c0U);
    }
    if (caller)
    {
        write_u32(file, text_file + 0x40U, 0x94000008U); // bl .plt
        write_u32(file, text_file + 0x44U, 0xd65f03c0U); // ret
        write_u32(file, text_file + 0x60U, 0xd2900410U); // movz x16, #0x8020
        write_u32(file, text_file + 0x64U, 0xf9400211U); // ldr x17, [x16]
        write_u32(file, text_file + 0x68U, 0xd61f0220U); // br x17
    }

    const auto dynamic_entry = [&](std::size_t index, std::uint64_t tag, std::uint64_t value) {
        write_u64(file, rodata_file + index * 16U, tag);
        write_u64(file, rodata_file + index * 16U + 8U, value);
    };
    dynamic_entry(0U, static_cast<std::uint64_t>(format::DynamicTag::DT_HASH), hash_offset);
    dynamic_entry(1U, static_cast<std::uint64_t>(format::DynamicTag::DT_STRTAB), string_offset);
    dynamic_entry(2U, static_cast<std::uint64_t>(format::DynamicTag::DT_STRSZ), symbol_name.size() + 2U);
    dynamic_entry(3U, static_cast<std::uint64_t>(format::DynamicTag::DT_SYMTAB), symbol_offset);
    dynamic_entry(4U, static_cast<std::uint64_t>(format::DynamicTag::DT_SYMENT), 24U);
    dynamic_entry(5U, static_cast<std::uint64_t>(format::DynamicTag::DT_JMPREL), relocation_offset);
    dynamic_entry(6U, static_cast<std::uint64_t>(format::DynamicTag::DT_PLTRELSZ), 24U);
    dynamic_entry(7U, static_cast<std::uint64_t>(format::DynamicTag::DT_PLTREL),
                  static_cast<std::uint64_t>(format::DynamicTag::DT_RELA));
    dynamic_entry(8U, static_cast<std::uint64_t>(caller ? format::DynamicTag::DT_INIT
                                                         : format::DynamicTag::DT_NULL),
                  caller ? 0x40U : 0U);
    if (caller) dynamic_entry(9U, static_cast<std::uint64_t>(format::DynamicTag::DT_NULL), 0U);

    const auto string_file = rodata_file + (string_offset - 0x100U);
    std::copy(symbol_name.begin(), symbol_name.end(),
              reinterpret_cast<char*>(file.data() + string_file + 1U));
    const auto hash_file = rodata_file + (hash_offset - 0x100U);
    write_u32(file, hash_file, 1U);
    write_u32(file, hash_file + 4U, 2U);
    write_u32(file, hash_file + 8U, 1U);
    write_u32(file, hash_file + 12U, 0U);
    write_u32(file, hash_file + 16U, 1U);
    const auto symbol_file = rodata_file + (symbol_offset - 0x100U) + 24U;
    write_u32(file, symbol_file, 1U);
    file[symbol_file + 4U] = static_cast<std::byte>(0x12U);
    write_u32(file, symbol_file + 6U, defines_provider ? 1U : 0U);
    write_u64(file, symbol_file + 8U, defines_provider ? 0x40U : 0U);
    write_u64(file, symbol_file + 16U, defines_provider && provider_umulh ? 16U : 4U);
    const auto relocation_file = rodata_file + (relocation_offset - 0x100U);
    write_u64(file, relocation_file, data_offset + 0x20U);
    write_u64(file, relocation_file + 8U, (std::uint64_t{1U} << 32U) | 1026U);
    write_u64(file, relocation_file + 16U, 0U);
    write_u32(file, 0x88U, 0U);
    write_u32(file, 0x8cU, 0U);
    write_u32(file, 0x90U, static_cast<std::uint32_t>(string_offset));
    write_u32(file, 0x94U, static_cast<std::uint32_t>(symbol_name.size() + 2U));
    write_u32(file, 0x98U, static_cast<std::uint32_t>(symbol_offset));
    write_u32(file, 0x9cU, 48U);
    return file;
}

} // namespace

TEST_CASE("M16 portable unsigned high multiply matches independent vectors and oracle")
{
    constexpr auto max = UINT64_MAX;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 12> vectors{{
        {0U, 0U},
        {0U, max},
        {1U, max},
        {max, max},
        {0x100000000ULL, 0x100000000ULL},
        {0x8000000000000000ULL, 2U},
        {0x8000000000000000ULL, 0x8000000000000000ULL},
        {0xaaaaaaaaaaaaaaaaULL, 0x5555555555555555ULL},
        {0xffff0000ffff0000ULL, 0x0000ffff0000ffffULL},
        {0x100000001ULL, 0x100000001ULL},
        {0x0123456789abcdefULL, 0xfedcba9876543210ULL},
        {0xffffffff00000000ULL, 0x0000000100000000ULL},
    }};

    for (const auto [left, right] : vectors)
    {
        REQUIRE(common::multiply_high_unsigned_64(left, right) == oracle_high64(left, right));
    }

    std::mt19937_64 random(0x4d31365f554d554cULL);
    for (unsigned int index = 0U; index < 4096U; ++index)
    {
        const auto left = random();
        const auto right = random();
        REQUIRE(common::multiply_high_unsigned_64(left, right) == oracle_high64(left, right));
    }
}

TEST_CASE("M16 decoder normalizes scalar UMULH as a project-owned X-register instruction")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);

    // umulh x0, x1, x2 (public A64 encoding; no proprietary bytes).
    const auto decoded = decoder.value()->decode(0x1000U, 0x9bc27c20U);
    REQUIRE(decoded);
    REQUIRE(decoded.value().id == aarch64::InstructionId::Umulh);
    REQUIRE(decoded.value().normalized);
    REQUIRE(decoded.value().operands.size() == 3U);
    for (const auto& operand : decoded.value().operands)
    {
        REQUIRE(operand.kind == aarch64::OperandKind::Register);
        REQUIRE(operand.reg.kind == aarch64::RegisterKind::General);
        REQUIRE(operand.reg.width == aarch64::RegisterWidth::X64);
        REQUIRE_FALSE(operand.reg.is_stack_pointer);
    }
}

TEST_CASE("M16 UMULH lifts to project IR, preserves NZCV, aliases operands, and advances PC")
{
    auto fixture = make_fixture({0x9bc27c20U, 0xd65f03c0U}); // umulh x0, x1, x2; ret
    REQUIRE(fixture);
    REQUIRE(ir::print(fixture.value().function).find("mul_high_unsigned") != std::string::npos);

    runtime::CpuState cpu;
    cpu.x[1] = 0x100000000ULL;
    cpu.x[2] = 0x100000000ULL;
    cpu.n = 1U;
    cpu.z = 0U;
    cpu.c = 1U;
    cpu.v = 1U;
    REQUIRE(run(fixture.value(), cpu));
    REQUIRE(cpu.x[0] == 1U);
    REQUIRE(cpu.n == 1U);
    REQUIRE(cpu.z == 0U);
    REQUIRE(cpu.c == 1U);
    REQUIRE(cpu.v == 1U);
    REQUIRE(cpu.pc == fixture.value().address + 4U);

    auto destination_first = make_fixture({0x9bc27c21U, 0xd65f03c0U}); // umulh x1, x1, x2; ret
    REQUIRE(destination_first);
    runtime::CpuState first_cpu;
    first_cpu.x[1] = 0x100000000ULL;
    first_cpu.x[2] = 0x100000000ULL;
    REQUIRE(run(destination_first.value(), first_cpu));
    REQUIRE(first_cpu.x[1] == 1U);

    auto destination_second = make_fixture({0x9bc27c22U, 0xd65f03c0U}); // umulh x2, x1, x2; ret
    REQUIRE(destination_second);
    runtime::CpuState second_cpu;
    second_cpu.x[1] = 0x100000000ULL;
    second_cpu.x[2] = 0x100000000ULL;
    REQUIRE(run(destination_second.value(), second_cpu));
    REQUIRE(second_cpu.x[2] == 1U);

    auto same_sources = make_fixture({0x9bc37c60U, 0xd65f03c0U}); // umulh x0, x3, x3; ret
    REQUIRE(same_sources);
    runtime::CpuState same_cpu;
    same_cpu.x[3] = 0x100000000ULL;
    REQUIRE(run(same_sources.value(), same_cpu));
    REQUIRE(same_cpu.x[0] == 1U);
}

TEST_CASE("M17 diagnostic lifting executes UMULH before a typed unsupported boundary")
{
    constexpr memory::GuestAddress address = 0x200000U;
    const auto bytes = words({0x9bc97d49U, 0x7a400900U, 0xd65f03c0U});
    memory::GuestMemory memory;
    REQUIRE(memory.map(address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "m17.synthetic.text", memory::GuestRegionKind::Text));
    analysis::AnalysisOptions analysis_options;
    analysis_options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(memory, address, analysis_options);
    REQUIRE(cfg);
    lifter::LiftOptions lift_options;
    lift_options.stop_at_unsupported_instruction = true;
    const auto function = lifter::lift_function(cfg.value(), lift_options);
    REQUIRE(function);

    runtime::CpuState cpu;
    cpu.x[9] = 0x100000000ULL;
    cpu.x[10] = 0x100000000ULL;
    const std::array<std::uint64_t, 1> observed_targets{address};
    runtime::ExecutionOptions execution_options;
    execution_options.observed_guest_pcs = observed_targets;
    runtime::RuntimeContext context{&memory};
    interpreter::InterpreterFrame frame;
    const auto result = interpreter::execute_until_boundary(
        function.value(), cpu, context, frame, execution_options);
    REQUIRE(result);
    REQUIRE(result.value().observed_guest_pcs == std::vector<std::uint64_t>{address});
    REQUIRE(cpu.x[9] == 1U);
    REQUIRE(result.value().boundary.kind == runtime::ExecutionBoundaryKind::UnsupportedInstruction);
    REQUIRE(result.value().boundary.source_guest_pc == address + 4U);
    REQUIRE(result.value().boundary.target_provenance.find("ccmp") != std::string::npos);
    REQUIRE(result.value().boundary.target_provenance.find("0x7a400900") == std::string::npos);
}

TEST_CASE("M16 UMULH honors architecturally valid XZR source and destination aliases")
{
    auto zero_source = make_fixture({0x9bc27fe0U, 0xd65f03c0U}); // umulh x0, xzr, x2; ret
    REQUIRE(zero_source);
    runtime::CpuState source_cpu;
    source_cpu.x[2] = UINT64_MAX;
    REQUIRE(run(zero_source.value(), source_cpu));
    REQUIRE(source_cpu.x[0] == 0U);

    auto zero_destination = make_fixture({0x9bc27c3fU, 0xd65f03c0U}); // umulh xzr, x1, x2; ret
    REQUIRE(zero_destination);
    runtime::CpuState destination_cpu;
    destination_cpu.x[0] = 0x123456789abcdef0ULL;
    destination_cpu.x[1] = UINT64_MAX;
    destination_cpu.x[2] = UINT64_MAX;
    REQUIRE(run(zero_destination.value(), destination_cpu));
    REQUIRE(destination_cpu.x[0] == 0x123456789abcdef0ULL);
}

TEST_CASE("M16 verifier rejects non-64-bit unsigned high multiply combinations deterministically")
{
    ir::Function function("invalid_umulh", 0x1000U);
    const auto block = function.add_block(0x1000U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto left = builder.constant(ir::i32_type(), 1U);
    const auto right = builder.constant(ir::i32_type(), 2U);
    REQUIRE(left);
    REQUIRE(right);
    ir::Instruction invalid;
    invalid.opcode = ir::Opcode::MulHighUnsigned;
    invalid.result_type = ir::i32_type();
    invalid.operands = {left.value(), right.value()};
    REQUIRE(builder.emit(invalid));
    REQUIRE(builder.set_terminator(ir::Terminator{}));
    const auto verified = ir::verify(function);
    REQUIRE_FALSE(verified);
    REQUIRE(verified.error().code == ErrorCode::IrVerificationFailed);
    REQUIRE(verified.error().message.find("mul_high_unsigned") != std::string::npos);
}

TEST_CASE("M16 invalid guest provider remains typed and is not HLE-replaced")
{
    memory::GuestMemory memory;
    const auto invalid_base = std::numeric_limits<memory::GuestAddress>::max() - 0x20U;
    REQUIRE(memory.map(invalid_base, 0x20U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "sdk.text", memory::GuestRegionKind::Text));
    format::DynamicSymbolTable symbols;
    symbols.symbols.push_back(format::DynamicSymbol{
        8767U, 0U, "__nnmusl_init_dso", format::SymbolBinding::Global,
        format::SymbolType::Function, format::SymbolVisibility::Default, 1U, 0x40U, 4U});
    const std::array<analysis::ProcessSymbolSource, 1> sources{
        analysis::ProcessSymbolSource{"sdk", invalid_base, &symbols}};
    const auto namespace_result = analysis::ProcessSymbolNamespace::build_audited(memory, sources);
    REQUIRE(namespace_result);
    const auto lookup = namespace_result.value().lookup(
        "__nnmusl_init_dso", analysis::ModuleSetCompleteness::DeclaredComplete,
        analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion);
    REQUIRE(lookup.status == analysis::ProviderResolutionStatus::ProviderIneligible);
    REQUIRE(lookup.selected_candidate == std::nullopt);
    REQUIRE(lookup.candidates.empty());
    REQUIRE(lookup.occurrences.size() == 1U);
    REQUIRE(lookup.occurrences.front().eligibility == analysis::ProviderEligibility::InvalidAddress);
}

TEST_CASE("M16 synthetic guest provider relocation enters UMULH code without runtime fallback")
{
    const auto main_bytes = synthetic_provider_nso(false, true, false);
    const auto sdk_bytes = synthetic_provider_nso(true, false, true);
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"main", main_bytes, 0U},
        analysis::ProcessModuleInput{"sdk", sdk_bytes, 0x100000U}};
    analysis::ProcessImageOptions image_options;
    image_options.primary_module = "main";
    image_options.provider_search_complete = true;
    image_options.module_options.seed_text_entry = false;
    auto process = analysis::load_process_image(inputs, image_options);
    if (!process)
        UNSCOPED_INFO(error_code_name(process.error().code) << ": " << process.error().message);
    REQUIRE(process);
    REQUIRE(process.value().bindings().size() == 1U);
    REQUIRE(process.value().bindings().front().provider_address ==
            std::optional<memory::GuestAddress>(0x100040U));
    REQUIRE(process.value().bindings().front().slot_value_verified);

    std::vector<analysis::FinalizedFunctionMap> maps;
    for (const auto& module : process.value().modules())
    {
        const auto map = analysis::FunctionMapBuilder::build(
            analysis::ModuleAnalysisInput{module.identity, &process.value().memory(), module.seeds});
        REQUIRE(map);
        maps.push_back(std::move(map).value());
    }
    const auto process_map = analysis::ProcessFunctionMap::build(std::move(maps));
    REQUIRE(process_map);
    const auto* main_map = process_map.value().map_for(0x40U);
    REQUIRE(main_map != nullptr);
    const auto entry = execution::select_entry(main_map->identity(),
                                               execution::EntrySelectionKind::DynamicInit);
    REQUIRE(entry);

    bool runtime_handler_called = false;
    runtime::RuntimeImportRegistry registry;
    runtime::RuntimeImportDescriptor descriptor;
    descriptor.symbol_name = "__nnmusl_init_dso";
    descriptor.subsystem = runtime::RuntimeSubsystem::DynamicLoader;
    descriptor.support = runtime::RuntimeSupportStatus::Implemented;
    descriptor.evidence.confidence = "synthetic";
    REQUIRE(registry.register_import(
        descriptor, [&runtime_handler_called](runtime::RuntimeImportContext&) {
            runtime_handler_called = true;
            return Result<runtime::RuntimeImportOutcome>::success(
                runtime::RuntimeImportOutcome::handled());
        }));

    execution::ExecutionSession session(process.value().memory(), process_map.value(), process.value(),
                                         {}, {}, &registry);
    const auto result = session.run(entry.value());
    if (!result)
        UNSCOPED_INFO(error_code_name(result.error().code) << ": " << result.error().message);
    REQUIRE(result);
    REQUIRE(result.value().provider_guest_code_entered);
    REQUIRE(result.value().final_cpu.x[0] == 0x100000000ULL);
    REQUIRE(result.value().runtime.imports_encountered == 0U);
    REQUIRE_FALSE(runtime_handler_called);
    REQUIRE(result.value().function_transfers == 1U);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(std::find(result.value().executed_function_modules.begin(),
                      result.value().executed_function_modules.end(), "sdk") !=
            result.value().executed_function_modules.end());
    const auto provider_entry = std::find_if(
        result.value().events.begin(), result.value().events.end(), [](const auto& event) {
            return event.kind == execution::ExecutionEventKind::FunctionEnter &&
                   event.function_module == "sdk";
        });
    REQUIRE(provider_entry != result.value().events.end());
    REQUIRE(provider_entry->guest_pc == 0x100040U);
    REQUIRE(provider_entry->function_entry == 0x100040U);
    REQUIRE(provider_entry->call_depth == 1U);
    REQUIRE(result.value().observation_targets == std::vector<memory::GuestAddress>{0x100048U});
    REQUIRE(result.value().executed_guest_instructions.size() == 1U);
    REQUIRE(result.value().executed_guest_instructions.front().guest_pc == 0x100048U);
    REQUIRE(result.value().executed_guest_instructions.front().module == "sdk");
    REQUIRE(result.value().executed_guest_instructions.front().executed);
    REQUIRE(result.value().executed_guest_instructions.front().next_guest_pc ==
            std::optional<memory::GuestAddress>(0x10004cU));
    REQUIRE(result.value().executed_guest_instructions.front().destination_register == "x0");
    REQUIRE(result.value().executed_guest_instructions.front().source_registers ==
            std::vector<std::string>{"x1", "x2"});
    REQUIRE(result.value().executed_guest_instructions.front().call_depth == 1U);
}

#ifdef TOTKRECOMP_HAS_LLVM
TEST_CASE("M16 LLVM UMULH lowering matches the interpreter")
{
    auto fixture = make_fixture({0x9bc27c20U, 0xd65f03c0U});
    REQUIRE(fixture);
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);

    runtime::CpuState reference_cpu;
    reference_cpu.x[1] = 0xfedcba9876543210ULL;
    reference_cpu.x[2] = 0x0123456789abcdefULL;
    reference_cpu.n = 1U;
    reference_cpu.z = 1U;
    reference_cpu.c = 0U;
    reference_cpu.v = 1U;
    runtime::RuntimeContext reference_context{&fixture.value().memory};
    const auto reference = interpreter::execute(fixture.value().function, reference_cpu,
                                                 reference_context);
    REQUIRE(reference);

    auto native_cpu = reference_cpu;
    runtime::RuntimeContext native_context{&fixture.value().memory};
    const auto native = backend.value()->execute(fixture.value().function, native_cpu, native_context);
    REQUIRE(native);
    REQUIRE(native_cpu.x == reference_cpu.x);
    REQUIRE(native_cpu.sp == reference_cpu.sp);
    REQUIRE(native_cpu.pc == reference_cpu.pc);
    REQUIRE(native_cpu.n == reference_cpu.n);
    REQUIRE(native_cpu.z == reference_cpu.z);
    REQUIRE(native_cpu.c == reference_cpu.c);
    REQUIRE(native_cpu.v == reference_cpu.v);
    REQUIRE(native.value().status == reference.value().status);
}
#endif
