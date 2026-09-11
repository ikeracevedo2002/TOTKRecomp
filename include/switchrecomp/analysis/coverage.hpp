#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace switchrecomp::analysis
{

struct CoverageOptions
{
    std::size_t max_instructions = 4'000'000U;
};

struct CoverageOpcodeCount
{
    std::string opcode;
    std::size_t count = 0U;
};

struct CoverageReport
{
    std::uint32_t schema_version = 1U;
    std::string module;
    memory::GuestAddress base = 0U;
    memory::GuestSize size = 0U;
    std::size_t decoded = 0U;
    std::size_t liftable = 0U;
    std::size_t unsupported = 0U;
    std::size_t decode_failures = 0U;
    std::vector<CoverageOpcodeCount> instruction_frequency;
    std::vector<CoverageOpcodeCount> unsupported_frequency;
    std::vector<memory::GuestAddress> first_unsupported_addresses;
};

[[nodiscard]] Result<CoverageReport> scan_coverage(
    const memory::GuestMemory& memory, memory::GuestAddress base, memory::GuestSize size,
    std::string module = {}, const CoverageOptions& options = {});

[[nodiscard]] std::string render_coverage(const CoverageReport& report);
[[nodiscard]] std::string render_coverage_json(const CoverageReport& report);

} // namespace switchrecomp::analysis
