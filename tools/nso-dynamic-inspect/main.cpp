#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/format/dynamic_symbols.hpp"
#include "switchrecomp/format/elf_rela.hpp"
#include "switchrecomp/format/module_metadata.hpp"
#include "switchrecomp/format/nso.hpp"
#include "switchrecomp/loader/nso_guest_loader.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/version.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{

using switchrecomp::format::AArch64RelocationType;
using switchrecomp::format::DynamicSymbolTable;
using switchrecomp::format::Relocation;

[[nodiscard]] std::string hex_value(std::uint64_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

void print_help(std::ostream& output)
{
    output << "Usage: nso-dynamic-inspect [options] module.nso\n\n"
              "Inspect validated NSO dynamic symbols, imports, and AArch64 RELA relocations.\n\n"
              "Options:\n"
              "  --symbols       Print dynamic symbols.\n"
              "  --imports       Print undefined imports.\n"
              "  --relocations   Print semantic relocations.\n"
              "  --json          Emit stable JSON instead of text.\n"
              "  --help          Show this help text.\n"
              "  --version       Show the project version.\n";
}

[[nodiscard]] nlohmann::json symbol_json(const switchrecomp::format::DynamicSymbol& symbol)
{
    return { {"index", symbol.index},
             {"name", symbol.name},
             {"binding", switchrecomp::format::symbol_binding_name(symbol.binding)},
             {"type", switchrecomp::format::symbol_type_name(symbol.type)},
             {"visibility", switchrecomp::format::symbol_visibility_name(symbol.visibility)},
             {"defined", symbol.is_defined()},
             {"value", hex_value(symbol.value)},
             {"size", symbol.size} };
}

[[nodiscard]] std::string relocation_name(const Relocation& relocation)
{
    if (relocation.type != AArch64RelocationType::Unknown)
    {
        return std::string(switchrecomp::format::aarch64_relocation_type_name(relocation.type));
    }
    return "UNKNOWN(" + std::to_string(relocation.raw_type) + ")";
}

[[nodiscard]] nlohmann::json relocation_json(const Relocation& relocation)
{
    return { {"offset", hex_value(relocation.offset)},
             {"target", hex_value(relocation.target_address)},
             {"type", relocation_name(relocation)},
             {"raw_type", relocation.raw_type},
             {"symbol_index", relocation.symbol_index},
             {"addend", relocation.addend} };
}

template <typename T> int report_error(const switchrecomp::Result<T>& result)
{
    std::cerr << switchrecomp::error_code_name(result.error().code) << ": "
              << result.error().message << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    bool show_symbols = false;
    bool show_imports = false;
    bool show_relocations = false;
    bool json_output = false;
    std::string input_path;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h")
        {
            print_help(std::cout);
            return 0;
        }
        if (argument == "--version")
        {
            std::cout << switchrecomp::version << '\n';
            return 0;
        }
        if (argument == "--symbols")
        {
            show_symbols = true;
        }
        else if (argument == "--imports")
        {
            show_imports = true;
        }
        else if (argument == "--relocations")
        {
            show_relocations = true;
        }
        else if (argument == "--json")
        {
            json_output = true;
        }
        else if (argument.starts_with('-') || !input_path.empty())
        {
            std::cerr << "invalid argument: " << argument << '\n';
            return 2;
        }
        else
        {
            input_path = std::string(argument);
        }
    }
    if (input_path.empty())
    {
        print_help(std::cerr);
        return 2;
    }
    if (!show_symbols && !show_imports && !show_relocations)
    {
        show_symbols = true;
        show_imports = true;
        show_relocations = true;
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input)
    {
        std::cerr << "could not open input: " << input_path << '\n';
        return 1;
    }
    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(input_path, file_error);
    if (file_error || file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        std::cerr << "could not determine a safe input size\n";
        return 1;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        {
            std::cerr << "failed to read input\n";
            return 1;
        }
    }

    const auto header = switchrecomp::format::parse_nso_header(bytes);
    if (!header)
    {
        return report_error(header);
    }
    const auto image = switchrecomp::format::materialize_nso(bytes, header.value());
    if (!image)
    {
        return report_error(image);
    }
    switchrecomp::memory::GuestMemory guest_memory;
    const auto loaded = switchrecomp::loader::load_nso(image.value(), guest_memory);
    if (!loaded)
    {
        return report_error(loaded);
    }
    const auto module_base = static_cast<switchrecomp::memory::GuestAddress>(
        image.value().text.memory_offset);
    const auto metadata = switchrecomp::format::parse_module_metadata(guest_memory, module_base);
    if (!metadata)
    {
        return report_error(metadata);
    }
    if (!metadata.value().dynamic)
    {
        std::cerr << "invalid_format: module has no MOD0 dynamic metadata\n";
        return 1;
    }
    const auto symbols = DynamicSymbolTable::parse(guest_memory, metadata.value().dynamic.value());
    if (!symbols)
    {
        return report_error(symbols);
    }
    const auto rela = switchrecomp::format::parse_rela_table(
        guest_memory, metadata.value().dynamic.value());
    if (!rela)
    {
        return report_error(rela);
    }
    const auto jmprel = switchrecomp::format::parse_jmprel_table(
        guest_memory, metadata.value().dynamic.value());
    if (!jmprel)
    {
        return report_error(jmprel);
    }
    std::vector<switchrecomp::format::RelaEntry> binary_relocations = rela.value();
    binary_relocations.insert(binary_relocations.end(), jmprel.value().begin(), jmprel.value().end());
    const auto relocations = switchrecomp::format::make_relocations(binary_relocations);
    if (!relocations)
    {
        return report_error(relocations);
    }

    if (json_output)
    {
        nlohmann::json result{{"module_base", hex_value(module_base)}};
        if (show_symbols)
        {
            result["symbols"] = nlohmann::json::array();
            for (const auto& symbol : symbols.value().symbols)
            {
                result["symbols"].push_back(symbol_json(symbol));
            }
        }
        if (show_imports)
        {
            result["imports"] = nlohmann::json::array();
            for (const auto& import : symbols.value().imports())
            {
                result["imports"].push_back({{"index", import.symbol_index},
                                               {"name", import.name},
                                               {"binding", switchrecomp::format::symbol_binding_name(import.binding)},
                                               {"type", switchrecomp::format::symbol_type_name(import.type)},
                                               {"visibility", switchrecomp::format::symbol_visibility_name(import.visibility)}});
            }
        }
        if (show_relocations)
        {
            result["relocations"] = nlohmann::json::array();
            for (const auto& relocation : relocations.value())
            {
                result["relocations"].push_back(relocation_json(relocation));
            }
        }
        std::cout << result.dump(2) << '\n';
        return 0;
    }

    std::cout << "Module base: " << hex_value(module_base) << '\n';
    if (show_symbols)
    {
        std::cout << "Dynamic symbols:\n";
        for (const auto& symbol : symbols.value().symbols)
        {
            std::cout << "  [" << symbol.index << "] name=" << symbol.name
                      << " binding=" << switchrecomp::format::symbol_binding_name(symbol.binding)
                      << " type=" << switchrecomp::format::symbol_type_name(symbol.type)
                      << " visibility="
                      << switchrecomp::format::symbol_visibility_name(symbol.visibility)
                      << " defined=" << (symbol.is_defined() ? "yes" : "no")
                      << " value=" << hex_value(symbol.value) << '\n';
        }
    }
    if (show_imports)
    {
        std::cout << "Imports:\n";
        for (const auto& import : symbols.value().imports())
        {
            std::cout << "  [" << import.symbol_index << "] " << import.name
                      << " binding=" << switchrecomp::format::symbol_binding_name(import.binding)
                      << " type=" << switchrecomp::format::symbol_type_name(import.type) << '\n';
        }
    }
    if (show_relocations)
    {
        std::cout << "Relocations:\n";
        for (std::size_t index = 0U; index < relocations.value().size(); ++index)
        {
            const auto& relocation = relocations.value()[index];
            std::cout << "  [" << index << "] type=" << relocation_name(relocation)
                      << " target=" << hex_value(relocation.target_address)
                      << " symbol=" << relocation.symbol_index
                      << " addend=" << relocation.addend << '\n';
        }
    }
    return 0;
}
