#include "switchrecomp/format/nso_magic.hpp"
#include "switchrecomp/version.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>

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
    output << "Usage: nso-inspect [options] [file]\n\n"
              "Inspect the currently supported subset of a Nintendo Switch NSO input.\n\n"
              "Options:\n"
              "  --help       Show this help text.\n"
              "  --version    Show the project version.\n\n"
              "The complete NSO parser is planned for Milestone 1.\n";
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

    std::array<std::byte, 4> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    const auto count = input.gcount();
    if (count < static_cast<std::streamsize>(magic.size()))
    {
        std::cerr << "malformed input: file is shorter than the four-byte NSO magic\n";
        return static_cast<int>(ExitCode::MalformedInput);
    }
    if (input.bad())
    {
        std::cerr << "general error: failed while reading input file\n";
        return static_cast<int>(ExitCode::GeneralError);
    }

    const auto magic_status = switchrecomp::format::inspect_nso_magic(magic);
    if (!magic_status)
    {
        std::cerr << "general error: " << magic_status.error().message << '\n';
        return static_cast<int>(ExitCode::GeneralError);
    }
    switch (magic_status.value())
    {
    case switchrecomp::format::NsoMagicStatus::Valid:
        std::cout << "magic: NSO0\n"
                     "note: full NSO parsing is not implemented yet\n";
        return static_cast<int>(ExitCode::Success);
    case switchrecomp::format::NsoMagicStatus::TooShort:
        std::cerr << "malformed input: file is shorter than the four-byte NSO magic\n";
        return static_cast<int>(ExitCode::MalformedInput);
    case switchrecomp::format::NsoMagicStatus::Unexpected:
        std::cerr << "unsupported input: expected NSO0 magic\n";
        return static_cast<int>(ExitCode::UnsupportedInput);
    }

    return static_cast<int>(ExitCode::GeneralError);
}
