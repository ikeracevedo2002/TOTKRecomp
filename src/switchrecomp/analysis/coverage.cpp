#include "switchrecomp/analysis/coverage.hpp"

#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/aarch64/instruction.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace switchrecomp::analysis
{

namespace
{

[[nodiscard]] std::string hex_address(memory::GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] std::string json_escape(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char character : value)
    {
        if (character == '\\' || character == '"')
        {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

[[nodiscard]] bool count_order(const CoverageOpcodeCount& left,
                               const CoverageOpcodeCount& right) noexcept
{
    if (left.count != right.count)
    {
        return left.count > right.count;
    }
    return left.opcode < right.opcode;
}

// The analysis library must not depend on the lifter (the lifter links analysis),
// so the liftability classification is mirrored here. Keep this table and the
// FP/SIMD table below in sync with lifter::is_instruction_liftable in
// src/switchrecomp/lifter/lifter.cpp.
[[nodiscard]] bool common_liftable(const aarch64::DecodedInstruction& instruction) noexcept
{
    const auto id = instruction.id;
    if (id == aarch64::InstructionId::Mrs || id == aarch64::InstructionId::Msr)
    {
        return instruction.system_register == aarch64::SystemRegister::TpidrEl0 ||
               (id == aarch64::InstructionId::Mrs &&
                instruction.system_register == aarch64::SystemRegister::TpidrroEl0);
    }
    if ((id == aarch64::InstructionId::Dmb || id == aarch64::InstructionId::Dsb ||
         id == aarch64::InstructionId::Isb) &&
        instruction.barrier_option == aarch64::BarrierOption::Invalid)
    {
        return false;
    }
    switch (id)
    {
    case aarch64::InstructionId::Udf:
    case aarch64::InstructionId::Nop:
    case aarch64::InstructionId::Add: case aarch64::InstructionId::Adds:
    case aarch64::InstructionId::Adc: case aarch64::InstructionId::Adcs:
    case aarch64::InstructionId::Sbc: case aarch64::InstructionId::Sbcs:
    case aarch64::InstructionId::Ngc: case aarch64::InstructionId::Ngcs:
    case aarch64::InstructionId::Sub: case aarch64::InstructionId::Subs:
    case aarch64::InstructionId::And: case aarch64::InstructionId::Ands:
    case aarch64::InstructionId::Orr: case aarch64::InstructionId::Orn:
    case aarch64::InstructionId::Eor: case aarch64::InstructionId::Eon:
    case aarch64::InstructionId::Bic: case aarch64::InstructionId::Bics:
    case aarch64::InstructionId::Mov: case aarch64::InstructionId::Mvn:
    case aarch64::InstructionId::Cmp: case aarch64::InstructionId::Cmn:
    case aarch64::InstructionId::Ccmp: case aarch64::InstructionId::Ccmn:
    case aarch64::InstructionId::Tst: case aarch64::InstructionId::Neg:
    case aarch64::InstructionId::Negs: case aarch64::InstructionId::Csel:
    case aarch64::InstructionId::Csinc: case aarch64::InstructionId::Csinv:
    case aarch64::InstructionId::Csneg: case aarch64::InstructionId::Cset:
    case aarch64::InstructionId::Csetm: case aarch64::InstructionId::Cinc:
    case aarch64::InstructionId::Cinv: case aarch64::InstructionId::Cneg:
    case aarch64::InstructionId::Movz: case aarch64::InstructionId::Movk:
    case aarch64::InstructionId::Movn: case aarch64::InstructionId::Lsl:
    case aarch64::InstructionId::Lsr: case aarch64::InstructionId::Asr:
    case aarch64::InstructionId::Ror: case aarch64::InstructionId::Ubfm:
    case aarch64::InstructionId::Sbfm: case aarch64::InstructionId::Bfm:
    case aarch64::InstructionId::Extr:
    case aarch64::InstructionId::Mul: case aarch64::InstructionId::Madd:
    case aarch64::InstructionId::Msub: case aarch64::InstructionId::Mneg:
    case aarch64::InstructionId::Umulh: case aarch64::InstructionId::Smulh:
    case aarch64::InstructionId::Umaddl: case aarch64::InstructionId::Umsubl:
    case aarch64::InstructionId::Smaddl: case aarch64::InstructionId::Smsubl:
    case aarch64::InstructionId::Crc32: case aarch64::InstructionId::Prfm:
    case aarch64::InstructionId::Rev: case aarch64::InstructionId::Rev16:
    case aarch64::InstructionId::Udiv: case aarch64::InstructionId::Sdiv:
    case aarch64::InstructionId::Adr: case aarch64::InstructionId::Adrp:
    case aarch64::InstructionId::Ldr: case aarch64::InstructionId::Ldrb:
    case aarch64::InstructionId::Ldrh: case aarch64::InstructionId::Ldrsb:
    case aarch64::InstructionId::Ldrsh: case aarch64::InstructionId::Ldrsw:
    case aarch64::InstructionId::Str: case aarch64::InstructionId::Strb:
    case aarch64::InstructionId::Strh: case aarch64::InstructionId::Ldp:
    case aarch64::InstructionId::Stp: case aarch64::InstructionId::Ldur:
    case aarch64::InstructionId::Stur: case aarch64::InstructionId::LdrLiteral:
    case aarch64::InstructionId::B: case aarch64::InstructionId::Bl:
    case aarch64::InstructionId::BCond: case aarch64::InstructionId::Br:
    case aarch64::InstructionId::Blr: case aarch64::InstructionId::Ret:
    case aarch64::InstructionId::Cbz: case aarch64::InstructionId::Cbnz:
    case aarch64::InstructionId::Tbz: case aarch64::InstructionId::Tbnz:
    case aarch64::InstructionId::Ldxr: case aarch64::InstructionId::Ldxrb:
    case aarch64::InstructionId::Ldxrh: case aarch64::InstructionId::Ldaxr:
    case aarch64::InstructionId::Ldaxrb: case aarch64::InstructionId::Ldaxrh:
    case aarch64::InstructionId::Stxr: case aarch64::InstructionId::Stxrb:
    case aarch64::InstructionId::Stxrh: case aarch64::InstructionId::Stlxr:
    case aarch64::InstructionId::Stlxrb: case aarch64::InstructionId::Stlxrh:
    case aarch64::InstructionId::Ldar: case aarch64::InstructionId::Ldarb:
    case aarch64::InstructionId::Ldarh: case aarch64::InstructionId::Stlr:
    case aarch64::InstructionId::Stlrb: case aarch64::InstructionId::Stlrh:
    case aarch64::InstructionId::Clrex: case aarch64::InstructionId::Dmb:
    case aarch64::InstructionId::Dsb: case aarch64::InstructionId::Isb:
    case aarch64::InstructionId::Mrs: case aarch64::InstructionId::Msr:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool fp_simd_liftable(const aarch64::DecodedInstruction& instruction) noexcept
{
    const auto vector_register = [](const aarch64::Operand& operand) {
        return operand.kind == aarch64::OperandKind::Register &&
               operand.reg.kind == aarch64::RegisterKind::Vector && operand.reg.index < 32U;
    };
    const auto structure_liftable = [&]() {
        using Op = aarch64::SimdOperation;
        const auto op = instruction.simd_operation;
        const auto fixed_count = op == Op::Ld1r || op == Op::Ld2r || op == Op::Ld3r || op == Op::Ld4r
                                     ? op == Op::Ld1r ? 1U : op == Op::Ld2r ? 2U : op == Op::Ld3r ? 3U : 4U
                                     : op == Op::Ld2 ? 2U : op == Op::Ld3 ? 3U : op == Op::Ld4 ? 4U : 0U;
        const auto memory = std::find_if(
            instruction.operands.begin(), instruction.operands.end(),
            [](const aarch64::Operand& operand) { return operand.kind == aarch64::OperandKind::Memory; });
        if (memory == instruction.operands.end()) return false;
        const auto destination_count = static_cast<std::size_t>(
            memory - instruction.operands.begin());
        if ((op != Op::Ld1 && destination_count != fixed_count) ||
            (op == Op::Ld1 && (destination_count < 1U || destination_count > 4U)) ||
            (instruction.operands.size() != destination_count + 1U &&
             instruction.operands.size() != destination_count + 2U))
            return false;
        const bool register_post_index = instruction.operands.size() == destination_count + 2U;
        if (register_post_index &&
            (memory->memory.addressing != aarch64::MemoryAddressingMode::PostIndex ||
             instruction.operands.back().kind != aarch64::OperandKind::Register ||
             instruction.operands.back().reg.kind != aarch64::RegisterKind::General ||
             instruction.operands.back().reg.width != aarch64::RegisterWidth::X64))
            return false;
        if (memory->memory.base.kind != aarch64::RegisterKind::General ||
            memory->memory.base.width != aarch64::RegisterWidth::X64 ||
            (memory->memory.addressing != aarch64::MemoryAddressingMode::Base &&
             memory->memory.addressing != aarch64::MemoryAddressingMode::PreIndex &&
             memory->memory.addressing != aarch64::MemoryAddressingMode::PostIndex))
            return false;
        if (!vector_register(instruction.operands[0]) ||
            instruction.operands[0].arrangement == aarch64::VectorArrangement::Invalid)
            return false;
        const auto arrangement = instruction.operands[0].arrangement;
        const auto lanes = aarch64::vector_lane_count(arrangement);
        const auto bits = aarch64::vector_element_bits(arrangement);
        if (lanes == 0U || bits == 0U || bits % 8U != 0U)
            return false;
        const bool lane_load = op == Op::Ld1 && instruction.operands[0].vector_index >= 0;
        if (lane_load && (destination_count != 1U ||
                          static_cast<std::uint8_t>(instruction.operands[0].vector_index) >= lanes))
            return false;
        if ((op == Op::Ld2 || op == Op::Ld3 || op == Op::Ld4) &&
            arrangement == aarch64::VectorArrangement::D1)
            return false;
        for (std::size_t destination = 0U; destination < destination_count; ++destination)
        {
            if (!vector_register(instruction.operands[destination]) ||
                instruction.operands[destination].arrangement != arrangement ||
                instruction.operands[destination].reg.index !=
                    static_cast<std::uint8_t>((instruction.operands[0].reg.index + destination) % 32U))
                return false;
        }
        return true;
    };
    switch (instruction.simd_operation)
    {
    case aarch64::SimdOperation::None:
    case aarch64::SimdOperation::Fmadd:
    case aarch64::SimdOperation::Fmsub:
    case aarch64::SimdOperation::Fnmadd:
    case aarch64::SimdOperation::Fnmsub:
        return false;
    case aarch64::SimdOperation::Movi:
    case aarch64::SimdOperation::Mvni:
        return instruction.operands.size() == 2U && vector_register(instruction.operands[0]) &&
               (instruction.operands[0].arrangement == aarch64::VectorArrangement::B8 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::B16 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::H4 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::H8 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::S2 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::S4 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::D1 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::D2) &&
               instruction.operands[1].kind == aarch64::OperandKind::Immediate;
    case aarch64::SimdOperation::Faddp:
        return instruction.operands.size() == 3U && vector_register(instruction.operands[0]) &&
               vector_register(instruction.operands[1]) && vector_register(instruction.operands[2]) &&
               (instruction.operands[0].arrangement == aarch64::VectorArrangement::S2 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::S4 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::D2);
    case aarch64::SimdOperation::Bif:
    case aarch64::SimdOperation::Bit:
    case aarch64::SimdOperation::Bsl:
        return instruction.operands.size() == 3U && vector_register(instruction.operands[0]) &&
               vector_register(instruction.operands[1]) && vector_register(instruction.operands[2]) &&
               (instruction.operands[0].arrangement == aarch64::VectorArrangement::B8 ||
                instruction.operands[0].arrangement == aarch64::VectorArrangement::B16);
    case aarch64::SimdOperation::Ld1:
    case aarch64::SimdOperation::Ld1r:
    case aarch64::SimdOperation::Ld2:
    case aarch64::SimdOperation::Ld2r:
    case aarch64::SimdOperation::Ld3:
    case aarch64::SimdOperation::Ld3r:
    case aarch64::SimdOperation::Ld4:
    case aarch64::SimdOperation::Ld4r:
        return structure_liftable();
    case aarch64::SimdOperation::St1:
        return instruction.operands.size() == 2U &&
               instruction.operands[0].kind == aarch64::OperandKind::Register &&
               instruction.operands[0].reg.kind == aarch64::RegisterKind::Vector &&
               instruction.operands[0].arrangement != aarch64::VectorArrangement::Invalid &&
               instruction.operands[0].vector_index >= 0 &&
               instruction.operands[1].kind == aarch64::OperandKind::Memory;
    default:
        return true;
    }
}

} // namespace

Result<CoverageReport> scan_coverage(const memory::GuestMemory& memory, memory::GuestAddress base,
                                     memory::GuestSize size, std::string module,
                                     const CoverageOptions& options)
{
    if ((base & 0x3U) != 0U || (size & 0x3U) != 0U || size == 0U)
    {
        return Result<CoverageReport>::failure(make_error(
            ErrorCode::InvalidArgument, "coverage range must be non-empty and 4-byte aligned"));
    }
    const auto end = checked_add_u64(base, size);
    if (!end)
    {
        return Result<CoverageReport>::failure(end.error());
    }
    const auto executable = memory.is_executable(base, size);
    if (!executable)
    {
        return Result<CoverageReport>::failure(executable.error());
    }
    if (!executable.value())
    {
        return Result<CoverageReport>::failure(make_error(
            ErrorCode::NonExecutableAddress, "coverage range is not executable guest memory"));
    }
    CoverageReport report;
    report.module = std::move(module);
    report.base = base;
    report.size = size;
    std::map<std::string, std::size_t> instruction_counts;
    std::map<std::string, std::size_t> unsupported_counts;
    const auto instruction_count = static_cast<std::size_t>(size / 4U);
    if (instruction_count > options.max_instructions)
    {
        return Result<CoverageReport>::failure(make_error(
            ErrorCode::AnalysisInstructionLimitExceeded,
            "coverage range exceeds the configured instruction limit"));
    }

    struct WorkerReport
    {
        std::size_t decoded = 0U;
        std::size_t liftable = 0U;
        std::size_t unsupported = 0U;
        std::size_t decode_failures = 0U;
        std::map<std::string, std::size_t> instruction_frequency;
        std::map<std::string, std::size_t> unsupported_frequency;
        std::vector<std::size_t> first_unsupported_indices;
    };

    const auto worker_count = std::min(std::max<std::size_t>(options.workers, 1U),
                                        instruction_count);
    std::vector<std::optional<Result<WorkerReport>>> worker_reports(worker_count);
    const auto scan_range = [&](std::size_t worker) -> Result<WorkerReport> {
        const auto worker_decoder = aarch64::AArch64Decoder::create();
        if (!worker_decoder)
        {
            return Result<WorkerReport>::failure(worker_decoder.error());
        }
        const auto quotient = instruction_count / worker_count;
        const auto remainder = instruction_count % worker_count;
        const auto begin = worker * quotient + std::min(worker, remainder);
        const auto end = begin + quotient + (worker < remainder ? 1U : 0U);
        WorkerReport result;
        for (std::size_t index = begin; index < end; ++index)
        {
            const auto address = base + static_cast<memory::GuestAddress>(index * 4U);
            const auto decoded = aarch64::fetch_and_decode(memory, *worker_decoder.value(), address);
            if (!decoded)
            {
                ++result.decode_failures;
                continue;
            }
            ++result.decoded;
            const auto name = std::string(aarch64::instruction_id_name(decoded.value().id));
            ++result.instruction_frequency[name];
            const bool liftable = decoded.value().normalized &&
                                  (decoded.value().id == aarch64::InstructionId::FpSimd
                                       ? fp_simd_liftable(decoded.value())
                                       : common_liftable(decoded.value()));
            if (liftable)
            {
                ++result.liftable;
            }
            else
            {
                ++result.unsupported;
                ++result.unsupported_frequency[name];
                if (result.first_unsupported_indices.size() < 16U)
                {
                    result.first_unsupported_indices.push_back(index);
                }
            }
        }
        return Result<WorkerReport>::success(std::move(result));
    };

    std::exception_ptr worker_exception;
    std::mutex worker_exception_mutex;
    const auto run_worker = [&](std::size_t worker) {
        try
        {
            worker_reports[worker] = scan_range(worker);
        }
        catch (...)
        {
            std::lock_guard lock(worker_exception_mutex);
            if (worker_exception == nullptr)
            {
                worker_exception = std::current_exception();
            }
        }
    };
    if (worker_count == 1U)
    {
        run_worker(0U);
    }
    else
    {
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        try
        {
            for (std::size_t worker = 0U; worker < worker_count; ++worker)
            {
                workers.emplace_back(run_worker, worker);
            }
        }
        catch (...)
        {
            for (auto& worker : workers)
            {
                if (worker.joinable()) worker.join();
            }
            return Result<CoverageReport>::failure(make_error(
                ErrorCode::ResourceLimit, "unable to create the coverage worker pool"));
        }
        for (auto& worker : workers)
        {
            worker.join();
        }
    }
    if (worker_exception != nullptr)
    {
        return Result<CoverageReport>::failure(make_error(
            ErrorCode::ResourceLimit, "coverage worker failed while scanning the range"));
    }
    for (std::size_t worker = 0U; worker < worker_count; ++worker)
    {
        if (!worker_reports[worker].has_value())
        {
            return Result<CoverageReport>::failure(make_error(
                ErrorCode::ResourceLimit, "coverage worker did not publish a result"));
        }
        const auto& result = worker_reports[worker].value();
        if (!result)
        {
            return Result<CoverageReport>::failure(result.error());
        }
        report.decoded += result.value().decoded;
        report.liftable += result.value().liftable;
        report.unsupported += result.value().unsupported;
        report.decode_failures += result.value().decode_failures;
        for (const auto& [opcode, count] : result.value().instruction_frequency)
        {
            instruction_counts[opcode] += count;
        }
        for (const auto& [opcode, count] : result.value().unsupported_frequency)
        {
            unsupported_counts[opcode] += count;
        }
        for (const auto index : result.value().first_unsupported_indices)
        {
            if (report.first_unsupported_addresses.size() == 16U) break;
            report.first_unsupported_addresses.push_back(
                base + static_cast<memory::GuestAddress>(index * 4U));
        }
    }
    for (const auto& [opcode, count] : instruction_counts)
    {
        report.instruction_frequency.push_back(CoverageOpcodeCount{opcode, count});
    }
    for (const auto& [opcode, count] : unsupported_counts)
    {
        report.unsupported_frequency.push_back(CoverageOpcodeCount{opcode, count});
    }
    std::sort(report.instruction_frequency.begin(), report.instruction_frequency.end(), count_order);
    std::sort(report.unsupported_frequency.begin(), report.unsupported_frequency.end(), count_order);
    return Result<CoverageReport>::success(std::move(report));
}

std::string render_coverage(const CoverageReport& report)
{
    std::ostringstream output;
    output << "module: " << (report.module.empty() ? "<memory>" : report.module) << '\n'
           << "range: " << hex_address(report.base) << " + 0x" << std::hex << report.size << std::dec
           << '\n'
           << "decoded instructions: " << report.decoded << '\n'
           << "liftable instructions: " << report.liftable << '\n'
           << "unsupported instructions: " << report.unsupported << '\n'
           << "decode failures: " << report.decode_failures << '\n'
           << "lift success: "
           << (report.decoded == 0U ? 0.0 : (100.0 * static_cast<double>(report.liftable) /
                                              static_cast<double>(report.decoded)))
           << "%\n\n"
           << "instruction frequency (count, descending):\n";
    for (const auto& item : report.instruction_frequency)
    {
        output << "  " << item.opcode << ": " << item.count << '\n';
    }
    output << "unsupported frequency (count, descending):\n";
    for (const auto& item : report.unsupported_frequency)
    {
        output << "  " << item.opcode << ": " << item.count << '\n';
    }
    output << "first unsupported addresses:\n";
    for (const auto address : report.first_unsupported_addresses)
    {
        output << "  " << hex_address(address) << '\n';
    }
    return output.str();
}

std::string render_coverage_json(const CoverageReport& report)
{
    std::ostringstream output;
    output << "{\"schema_version\":" << report.schema_version << ",\"module\":\""
           << json_escape(report.module) << "\",\"base\":\"" << hex_address(report.base)
           << "\",\"size\":" << report.size << ",\"decoded\":" << report.decoded
           << ",\"liftable\":" << report.liftable << ",\"unsupported\":" << report.unsupported
           << ",\"decode_failures\":" << report.decode_failures << ",\"lift_rate\":"
           << (report.decoded == 0U ? 0.0 : static_cast<double>(report.liftable) /
                                                   static_cast<double>(report.decoded))
           << ",\"instruction_frequency\":[";
    for (std::size_t index = 0U; index < report.instruction_frequency.size(); ++index)
    {
        if (index != 0U)
        {
            output << ',';
        }
        const auto& item = report.instruction_frequency[index];
        output << "{\"opcode\":\"" << json_escape(item.opcode) << "\",\"count\":"
               << item.count << '}';
    }
    output << "],\"unsupported_instructions\":[";
    for (std::size_t index = 0U; index < report.unsupported_frequency.size(); ++index)
    {
        if (index != 0U)
        {
            output << ',';
        }
        const auto& item = report.unsupported_frequency[index];
        output << "{\"opcode\":\"" << json_escape(item.opcode) << "\",\"count\":"
               << item.count << '}';
    }
    output << "],\"first_unsupported_addresses\":[";
    for (std::size_t index = 0U; index < report.first_unsupported_addresses.size(); ++index)
    {
        if (index != 0U)
        {
            output << ',';
        }
        output << '"' << hex_address(report.first_unsupported_addresses[index]) << '"';
    }
    output << "]}";
    return output.str();
}

} // namespace switchrecomp::analysis
