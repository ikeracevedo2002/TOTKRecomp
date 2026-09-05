#include "switchrecomp/format/nso.hpp"
#include "switchrecomp/version.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

enum class ExitCode : int
{
    Success = 0,
    GeneralError = 1,
    InvalidArguments = 2,
    UnsupportedInput = 3,
    MalformedInput = 4,
};

void print_help(std::ostream& output)
{
    output << "Usage: nso-inspect [options] file\n\n"
              "Inspect the supported NSO0 header of a Nintendo Switch NSO input.\n\n"
              "Options:\n"
              "  --help       Show this help text.\n"
              "  --version    Show the project version.\n";
}

[[nodiscard]] std::string hex_value(std::uint64_t value, std::size_t digits)
{
    constexpr char digits_table[] = "0123456789abcdef";
    std::string result = "0x";
    result.reserve(2U + digits);
    for (std::size_t index = digits; index > 0U; --index)
    {
        const auto shift = static_cast<unsigned int>((index - 1U) * 4U);
        result.push_back(digits_table[(value >> shift) & 0x0fU]);
    }
    return result;
}

[[nodiscard]] const char* yes_no(bool value) noexcept
{
    return value ? "yes" : "no";
}

void print_segment(std::ostream& output, const switchrecomp::format::NsoSegment& segment)
{
    output << std::left << std::setw(10)
           << switchrecomp::format::nso_segment_kind_name(segment.kind) << std::right
           << std::setw(13) << hex_value(segment.file_offset, 8U) << std::setw(13)
           << hex_value(segment.stored_size, 8U) << std::setw(15)
           << hex_value(segment.memory_offset, 8U) << std::setw(13)
           << hex_value(segment.memory_size, 8U) << std::setw(12) << yes_no(segment.compressed)
           << std::setw(14) << yes_no(segment.hash_required) << '\n';
}

void print_header(std::ostream& output, const switchrecomp::format::NsoHeader& header)
{
    output << "Format: NSO0\n"
           << "Version: " << header.version << '\n'
           << "Flags: " << hex_value(header.flags, 8U) << '\n'
           << "Module ID: " << switchrecomp::format::module_id_hex(header) << "\n\n"
           << "Segment   FileOffset   StoredSize   MemoryOffset    MemorySize  Compressed  "
              "HashRequired\n";
    print_segment(output, header.text);
    print_segment(output, header.rodata);
    print_segment(output, header.data);
    output << '\n'
           << "BSS size: " << hex_value(header.bss_size, 8U) << '\n'
           << "Module name: offset=" << hex_value(header.module_name_offset, 8U)
           << " size=" << hex_value(header.module_name_size, 8U) << '\n'
           << "Embedded: offset=" << hex_value(header.embedded.offset, 8U)
           << " size=" << hex_value(header.embedded.size, 8U) << " (relative to rodata)\n"
           << "DynStr:   offset=" << hex_value(header.dynstr.offset, 8U)
           << " size=" << hex_value(header.dynstr.size, 8U) << " (relative to rodata)\n"
           << "DynSym:   offset=" << hex_value(header.dynsym.offset, 8U)
           << " size=" << hex_value(header.dynsym.size, 8U) << " (relative to rodata)\n"
           << "Execute-only memory: " << yes_no(header.execute_only_memory) << '\n'
           << "ZBIC compression: " << yes_no(header.use_zbic_compression) << '\n'
           << "Validation: OK\n";
}

[[nodiscard]] ExitCode error_exit_code(switchrecomp::ErrorCode code) noexcept
{
    switch (code)
    {
    case switchrecomp::ErrorCode::Unsupported:
        return ExitCode::UnsupportedInput;
    case switchrecomp::ErrorCode::InvalidFormat:
    case switchrecomp::ErrorCode::OutOfBounds:
    case switchrecomp::ErrorCode::ArithmeticOverflow:
    case switchrecomp::ErrorCode::ArithmeticUnderflow:
    case switchrecomp::ErrorCode::SizeMismatch:
        return ExitCode::MalformedInput;
    default:
        return ExitCode::GeneralError;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        print_help(std::cerr);
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    std::string_view input_path;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h")
        {
            print_help(std::cout);
            return static_cast<int>(ExitCode::Success);
        }
        if (argument == "--version")
        {
            std::cout << switchrecomp::version << '\n';
            return static_cast<int>(ExitCode::Success);
        }
        if (argument.starts_with('-'))
        {
            std::cerr << "invalid argument: " << argument << '\n';
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        if (!input_path.empty())
        {
            std::cerr << "invalid arguments: only one input file is accepted\n";
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        input_path = argument;
    }

    if (input_path.empty())
    {
        std::cerr << "invalid arguments: an input file is required\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    std::ifstream input(std::string(input_path), std::ios::binary);
    if (!input)
    {
        std::cerr << "general error: could not open input file: " << input_path << '\n';
        return static_cast<int>(ExitCode::GeneralError);
    }

    std::error_code filesystem_error;
    const auto file_size = std::filesystem::file_size(input_path, filesystem_error);
    if (filesystem_error)
    {
        std::cerr << "general error: could not determine input file size: " << input_path << '\n';
        return static_cast<int>(ExitCode::GeneralError);
    }
    if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
    {
        std::cerr << "general error: input file is too large to inspect safely\n";
        return static_cast<int>(ExitCode::GeneralError);
    }

    std::vector<std::byte> bytes;
    try
    {
        bytes.resize(static_cast<std::size_t>(file_size));
    }
    catch (const std::exception&)
    {
        std::cerr << "general error: could not allocate input file buffer\n";
        return static_cast<int>(ExitCode::GeneralError);
    }

    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || input.bad())
        {
            std::cerr << "general error: failed while reading input file\n";
            return static_cast<int>(ExitCode::GeneralError);
        }
    }

    const auto parsed = switchrecomp::format::parse_nso_header(bytes);
    if (!parsed)
    {
        const auto exit_code = error_exit_code(parsed.error().code);
        std::cerr << (exit_code == ExitCode::UnsupportedInput ? "unsupported input: "
                                                              : "malformed input: ")
                  << parsed.error().message << '\n';
        return static_cast<int>(exit_code);
    }

    print_header(std::cout, parsed.value());
    return static_cast<int>(ExitCode::Success);
}
