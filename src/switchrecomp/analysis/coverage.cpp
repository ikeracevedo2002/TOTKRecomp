#include "switchrecomp/analysis/coverage.hpp"

#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/aarch64/instruction.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

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

[[nodiscard]] bool common_liftable(aarch64::InstructionId id) noexcept
{
    switch (id)
    {
    case aarch64::InstructionId::Nop:
    case aarch64::InstructionId::Add: case aarch64::InstructionId::Adds:
    case aarch64::InstructionId::Sub: case aarch64::InstructionId::Subs:
    case aarch64::InstructionId::And: case aarch64::InstructionId::Ands:
    case aarch64::InstructionId::Orr: case aarch64::InstructionId::Orn:
    case aarch64::InstructionId::Eor: case aarch64::InstructionId::Eon:
    case aarch64::InstructionId::Bic: case aarch64::InstructionId::Bics:
    case aarch64::InstructionId::Mov: case aarch64::InstructionId::Mvn:
    case aarch64::InstructionId::Cmp: case aarch64::InstructionId::Cmn:
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
    case aarch64::InstructionId::Mul: case aarch64::InstructionId::Madd:
    case aarch64::InstructionId::Msub: case aarch64::InstructionId::Mneg:
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
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool fp_simd_liftable(aarch64::SimdOperation operation) noexcept
{
    switch (operation)
    {
    case aarch64::SimdOperation::None:
    case aarch64::SimdOperation::Fmadd:
    case aarch64::SimdOperation::Fmsub:
    case aarch64::SimdOperation::Fnmadd:
    case aarch64::SimdOperation::Fnmsub:
        return false;
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
    const auto decoder = aarch64::AArch64Decoder::create();
    if (!decoder)
    {
        return Result<CoverageReport>::failure(decoder.error());
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
    for (std::size_t index = 0U; index < instruction_count; ++index)
    {
        const auto address = base + static_cast<memory::GuestAddress>(index * 4U);
        const auto decoded = aarch64::fetch_and_decode(memory, *decoder.value(), address);
        if (!decoded)
        {
            ++report.decode_failures;
            continue;
        }
        ++report.decoded;
        const auto name = std::string(aarch64::instruction_id_name(decoded.value().id));
        ++instruction_counts[name];
        const bool liftable = decoded.value().id == aarch64::InstructionId::FpSimd
                                  ? fp_simd_liftable(decoded.value().simd_operation)
                                  : decoded.value().normalized && common_liftable(decoded.value().id);
        if (liftable)
        {
            ++report.liftable;
        }
        else
        {
            ++report.unsupported;
            ++unsupported_counts[name];
            if (report.first_unsupported_addresses.size() < 16U)
            {
                report.first_unsupported_addresses.push_back(address);
            }
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
