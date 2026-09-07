#include "switchrecomp/analysis/whole_module.hpp"
#include "switchrecomp/runtime/dispatcher.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;

[[nodiscard]] analysis::FunctionSeed seed(std::uint64_t entry,
                                          FunctionDiscoverySource source,
                                          FunctionConfidence confidence)
{
    return analysis::FunctionSeed{entry, source, confidence, std::nullopt, std::nullopt, {}};
}

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

[[nodiscard]] memory::GuestMemory make_code(std::uint64_t base,
                                             std::span<const std::byte> code)
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(base, code, memory::GuestMemoryPermissions::Read |
                                      memory::GuestMemoryPermissions::Execute,
                        "m10.text", memory::GuestRegionKind::Text));
    return memory;
}

[[nodiscard]] analysis::ModuleAnalysisInput input_for(const memory::GuestMemory& memory,
                                                       std::uint64_t base,
                                                       std::uint64_t size,
                                                       std::vector<analysis::FunctionSeed> seeds)
{
    analysis::ModuleIdentity identity;
    identity.module = "synthetic-main";
    identity.build_id = "synthetic-build";
    identity.input_sha256 = "synthetic-sha256";
    identity.guest_base = base;
    identity.executable_ranges.push_back(analysis::GuestAddressRange{base, size});
    identity.translator_version = version;
    return analysis::ModuleAnalysisInput{std::move(identity), &memory, std::move(seeds)};
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    REQUIRE(offset + 4U <= bytes.size());
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void write_i32(std::vector<std::byte>& bytes, std::size_t offset, std::int32_t value)
{
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
}

void write_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
{
    REQUIRE(offset + 8U <= bytes.size());
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
    write_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] std::vector<std::byte> make_synthetic_nso()
{
    constexpr std::size_t text_file = 0x100U;
    constexpr std::size_t rodata_file = 0x300U;
    constexpr std::size_t data_file = 0x400U;
    std::vector<std::byte> file(0x410U, std::byte{0});
    file[0] = std::byte{'N'};
    file[1] = std::byte{'S'};
    file[2] = std::byte{'O'};
    file[3] = std::byte{'0'};
    write_u32(file, 0x004U, 0U);
    write_u32(file, 0x008U, 0U);
    write_u32(file, 0x00cU, 0U);
    write_u32(file, 0x010U, static_cast<std::uint32_t>(text_file));
    write_u32(file, 0x014U, 0U);
    write_u32(file, 0x018U, 0x200U);
    write_u32(file, 0x01cU, 0U);
    write_u32(file, 0x020U, static_cast<std::uint32_t>(rodata_file));
    write_u32(file, 0x024U, 0x200U);
    write_u32(file, 0x028U, 0x100U);
    write_u32(file, 0x02cU, 0U);
    write_u32(file, 0x030U, static_cast<std::uint32_t>(data_file));
    write_u32(file, 0x034U, 0x300U);
    write_u32(file, 0x038U, 0x10U);
    write_u32(file, 0x03cU, 0U);
    for (std::size_t index = 0U; index < 32U; ++index)
    {
        file[0x040U + index] = static_cast<std::byte>(index + 1U);
    }
    write_u32(file, 0x060U, 0x200U);
    write_u32(file, 0x064U, 0x100U);
    write_u32(file, 0x068U, 0x10U);
    write_u32(file, 0x088U, 0U);
    write_u32(file, 0x08cU, 0U);
    write_u32(file, 0x090U, 0U);
    write_u32(file, 0x094U, 0U);
    write_u32(file, 0x098U, 0U);
    write_u32(file, 0x09cU, 0U);

    std::vector<std::byte> text(0x200U, std::byte{0});
    write_u32(text, 0x000U, 0U);
    write_u32(text, 0x004U, 0x20U);
    write_u32(text, 0x020U, format::mod0_magic);
    write_i32(text, 0x024U, 0x1e0);
    write_i32(text, 0x028U, 0x2e0);
    write_i32(text, 0x02cU, 0x2e0);
    write_i32(text, 0x030U, 0x2e0);
    write_i32(text, 0x034U, 0x2e0);
    write_i32(text, 0x038U, 0x2e0);
    write_u32(text, 0x100U, 0x94000008U); // bl 0x120
    write_u32(text, 0x104U, 0xd65f03c0U); // ret
    write_u32(text, 0x120U, 0x8b010000U); // add x0, x0, x1
    write_u32(text, 0x124U, 0xd65f03c0U); // ret
    std::copy(text.begin(), text.end(), file.begin() + static_cast<std::ptrdiff_t>(text_file));

    write_u64(file, rodata_file, 0U); // DT_NULL
    write_u64(file, rodata_file + 8U, 0U);
    return file;
}

std::uint32_t dispatcher_stub(runtime::CpuState* cpu, runtime::RuntimeContext* runtime)
{
    if (cpu != nullptr && runtime != nullptr) cpu->x[0] = 0x55U;
    return 7U;
}

} // namespace

TEST_CASE("M10 fixed-point discovery records direct BL provenance and ignores unreachable data")
{
    const auto code = words({0x94000008U, 0xd65f03c0U, 0xd503201fU, 0xd503201fU,
                             0xd503201fU, 0xd503201fU, 0xd503201fU, 0xd503201fU,
                             0x8b010000U, 0xd65f03c0U});
    const auto memory = make_code(0x1000U, code);
    const auto input = input_for(memory, 0x1000U, code.size(),
                                 {seed(0x1000U, FunctionDiscoverySource::ModuleEntry,
                                       FunctionConfidence::Confirmed)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    REQUIRE(map.value().frozen());
    REQUIRE(map.value().functions().size() == 2U);
    const auto* called = map.value().find(0x1020U);
    REQUIRE(called != nullptr);
    REQUIRE(called->primary_source == FunctionDiscoverySource::DirectCall);
    REQUIRE(called->confidence == FunctionConfidence::High);
    REQUIRE_FALSE(map.value().find(0x1008U));
    REQUIRE(map.value().functions()[0].direct_calls == std::vector<std::uint64_t>{0x1020U});

    analysis::TranslationOptions diagnostic_options;
    diagnostic_options.mode = analysis::TranslationMode::Diagnostic;
    const auto translated = analysis::translate_module(input, diagnostic_options);
    REQUIRE(translated);
    REQUIRE(translated.value().coverage.functions_translated == 2U);
    REQUIRE(translated.value().coverage.direct_calls == 1U);
    REQUIRE(translated.value().strict_success);
}

TEST_CASE("M10 function map reports boundary conflicts without choosing silently")
{
    const auto code = words({0xd503201fU, 0xd503201fU, 0xd65f03c0U, 0xd503201fU});
    const auto memory = make_code(0x2000U, code);
    const auto input = input_for(
        memory, 0x2000U, code.size(),
        {seed(0x2000U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual),
         seed(0x2004U, FunctionDiscoverySource::Heuristic, FunctionConfidence::Low)});
    const auto map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(map);
    REQUIRE(map.value().conflicts().size() == 1U);
    REQUIRE(map.value().functions().at(0).translation_status == analysis::TranslationStatus::Conflict);
    REQUIRE(map.value().functions().at(1).translation_status == analysis::TranslationStatus::Conflict);
}

TEST_CASE("M10 discovery rejects invalid seeds and enforces bounded fixed-point growth")
{
    const auto code = words({0x94000004U, 0xd65f03c0U, 0xd503201fU,
                             0x8b010000U, 0xd65f03c0U});
    const auto memory = make_code(0x3000U, code);
    auto invalid = input_for(memory, 0x3000U, code.size(),
                             {seed(0x3002U, FunctionDiscoverySource::ManualOverride,
                                   FunctionConfidence::Manual)});
    REQUIRE_FALSE(analysis::FunctionMapBuilder::build(invalid));
    REQUIRE(analysis::FunctionMapBuilder::build(invalid).error().code == ErrorCode::InvalidGuestAddress);

    auto limited = input_for(memory, 0x3000U, code.size(),
                             {seed(0x3000U, FunctionDiscoverySource::ModuleEntry,
                                   FunctionConfidence::Confirmed)});
    analysis::FunctionMapOptions options;
    options.budgets.max_functions = 1U;
    const auto result = analysis::FunctionMapBuilder::build(limited, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::AnalysisBudgetExceeded);
}

TEST_CASE("M10 strict and diagnostic translation distinguish unsupported instructions")
{
    const auto code = words({0xc87f7c20U, 0xd65f03c0U, 0x8b010000U, 0xd65f03c0U});
    const auto memory = make_code(0x4000U, code);
    const auto input = input_for(
        memory, 0x4000U, code.size(),
        {seed(0x4000U, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed),
         seed(0x4008U, FunctionDiscoverySource::ManualOverride, FunctionConfidence::Manual)});

    const auto strict = analysis::translate_module(input);
    REQUIRE(strict);
    REQUIRE_FALSE(strict.value().strict_success);
    REQUIRE(strict.value().coverage.functions_unsupported == 1U);
    REQUIRE(strict.value().coverage.functions_translated == 0U);
    REQUIRE(strict.value().functions.size() == 2U);
    REQUIRE(strict.value().functions[1].status == analysis::TranslationStatus::Excluded);

    analysis::TranslationOptions diagnostic_options;
    diagnostic_options.mode = analysis::TranslationMode::Diagnostic;
    const auto diagnostic = analysis::translate_module(input, diagnostic_options);
    REQUIRE(diagnostic);
    REQUIRE_FALSE(diagnostic.value().strict_success);
    REQUIRE(diagnostic.value().coverage.functions_unsupported == 1U);
    REQUIRE(diagnostic.value().coverage.functions_translated == 1U);
    REQUIRE(diagnostic.value().coverage.unsupported_instructions == 1U);
    REQUIRE(diagnostic.value().coverage.top_unsupported.front().instruction == "ldxp");
}

TEST_CASE("M10 frozen guest dispatcher validates guest addresses and is safe for concurrent reads")
{
    const auto code = words({0xd503201fU, 0xd65f03c0U});
    const auto memory = make_code(0x5000U, code);
    runtime::GuestFunctionRegistry registry(memory);
    REQUIRE(registry.add(runtime::FunctionRegistration{0x5000U, 0x5000U, 0x5008U,
                                                       "synthetic-main", dispatcher_stub}));
    REQUIRE(registry.freeze());
    REQUIRE(registry.frozen());
    REQUIRE(registry.lookup(0x5000U));
    REQUIRE_FALSE(registry.lookup(0x5004U));
    REQUIRE(registry.lookup(0x5002U).error().code == ErrorCode::InvalidGuestAddress);
    REQUIRE(registry.lookup(0x6000U).error().code == ErrorCode::UnknownGuestFunction);

    runtime::CpuState cpu{};
    runtime::RuntimeContext context{const_cast<memory::GuestMemory*>(&memory)};
    REQUIRE(registry.dispatch(0x5000U, cpu, context));
    REQUIRE(cpu.x[0] == 0x55U);
    REQUIRE(registry.add(runtime::FunctionRegistration{0x5004U, 0x5000U, 0x5008U,
                                                       "synthetic-main", dispatcher_stub}).error().code ==
            ErrorCode::FunctionRegistryFrozen);

    std::atomic<unsigned int> failures{0U};
    std::vector<std::thread> workers;
    for (unsigned int index = 0U; index < 8U; ++index)
    {
        workers.emplace_back([&registry, &failures] {
            for (unsigned int iteration = 0U; iteration < 1000U; ++iteration)
            {
                if (!registry.lookup(0x5000U)) ++failures;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(failures.load() == 0U);
}

TEST_CASE("M10 synthetic NSO crosses loading, MOD0, dynamic metadata, discovery and reporting")
{
    constexpr std::uint64_t module_base = 0x100000U;
    auto bytes = make_synthetic_nso();
    analysis::PreparedModuleOptions options;
    options.module_name = "synthetic-main.nso";
    options.module_base = module_base;
    options.seed_text_entry = false;
    options.seeds.push_back(analysis::FunctionSeed{
        module_base + 0x100U, FunctionDiscoverySource::AnalystSeed,
        FunctionConfidence::Manual, std::nullopt, std::nullopt, "synthetic entry"});
    const auto loaded = analysis::load_prepared_nso(bytes, options);
    if (!loaded) UNSCOPED_INFO(loaded.error().message);
    REQUIRE(loaded);
    REQUIRE(loaded.value().metadata.mod0.has_value());
    REQUIRE(loaded.value().metadata.dynamic.has_value());
    REQUIRE_FALSE(loaded.value().symbols.has_value());
    REQUIRE(loaded.value().identity.executable_ranges.size() == 1U);

    analysis::TranslationOptions translation_options;
    translation_options.mode = analysis::TranslationMode::Diagnostic;
    const auto result = analysis::translate_module(loaded.value(), translation_options);
    REQUIRE(result);
    REQUIRE(result.value().coverage.functions_discovered == 2U);
    REQUIRE(result.value().coverage.functions_translated == 2U);
    REQUIRE(result.value().coverage.direct_calls == 1U);
    const auto first_report = analysis::render_translation_report_json(result.value());
    const auto second_report = analysis::render_translation_report_json(result.value());
    REQUIRE(first_report == second_report);
    REQUIRE(first_report.find("/workspace") == std::string::npos);
    REQUIRE(first_report.find("synthetic-main.nso") != std::string::npos);
}
