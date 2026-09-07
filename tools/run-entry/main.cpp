#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/analysis/module_set.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/target/manifest.hpp"
#include "switchrecomp/version.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace switchrecomp;

enum class ExitCode : int
{
    Success = 0,
    InvalidArguments = 2,
    InfrastructureFailure = 3,
};

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& value)
{
    int base = 10;
    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text.remove_prefix(2U);
        base = 16;
    }
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool read_file(const std::filesystem::path& path, std::size_t max_size,
                             std::vector<std::byte>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_size) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(end));
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return input.good() || input.eof();
    }
    return true;
}

[[nodiscard]] Result<void> apply_target_manifest(
    const nlohmann::json& root, analysis::ModuleSetIngestionOptions& options)
{
    if (!root.contains("target_manifest") || !root.at("target_manifest").is_string())
        return Result<void>::success();
    std::vector<std::byte> bytes;
    if (!read_file(root.at("target_manifest").get<std::string>(), 1024U * 1024U, bytes))
    {
        return Result<void>::failure(make_error(
            ErrorCode::ModuleManifestMismatch, "target manifest could not be read"));
    }
    const auto manifest = target::parse_manifest(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!manifest) return Result<void>::failure(manifest.error());
    if (manifest.value().support_status == target::SupportStatus::Template)
    {
        if (options.completeness == analysis::ModuleSetCompleteness::ManifestVerifiedComplete)
            return Result<void>::failure(make_error(
                ErrorCode::ModuleManifestMismatch,
                "a template target manifest cannot establish manifest-verified completeness"));
        return Result<void>::success();
    }
    if (manifest.value().support_status == target::SupportStatus::Unsupported)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ModuleManifestMismatch, "target manifest marks this target unsupported"));
    }
    options.expected_logical_names.clear();
    options.expected_modules.clear();
    for (const auto& module : manifest.value().modules)
    {
        options.expected_logical_names.push_back(module.name);
        options.expected_modules.push_back(analysis::ModuleSetIngestionOptions::ExpectedModule{
            module.name, module.sha256, module.build_id, module.expected_size});
    }
    options.completeness = analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
    options.completeness_basis = analysis::ModuleSetCompletenessBasis::TargetManifestMatch;
    options.coherence = analysis::ModuleSetCoherence::Verified;
    options.coherence_basis = "target_manifest_identity_match";
    return Result<void>::success();
}

void help(std::ostream& output)
{
    output << "Usage: run-entry [options] prepared-main.nso\n"
              "       run-entry --local-config <file> [options]\n"
              "       run-entry --module NAME=PATH [--module NAME=PATH ...] [options]\n\n"
              "Run one bounded prepared guest entry with the Semantic IR interpreter.\n\n"
              "Options:\n"
              "  --help                         Show this help text.\n"
              "  --version                      Show the project version.\n"
              "  --module-name NAME             Logical module name (default: main).\n"
              "  --module-base ADDR             Analysis-selected guest base.\n"
              "  --module NAME=PATH             Add a process module (repeatable).\n"
              "  --local-config PATH             Read a local-only multi-module config.\n"
              "  --module-directory PATH        Scan a prepared non-recursive module directory.\n"
              "  --module-base-for NAME=ADDR    Set one process module analysis base.\n"
              "  --entry KIND                   dt-init, dt-fini, text-start, process.\n"
              "  --entry-address ADDR           Unverified analyst address.\n"
              "  --backend interpreter           M11 reference backend.\n"
              "  --stack-size N                 Synthetic guest stack size.\n"
              "  --report PATH                  Write deterministic JSON report.\n"
              "  --json                         Print deterministic JSON report.\n"
              "  --analysis-max-functions N     Analysis function budget.\n"
              "  --analysis-max-instructions N  Analysis instruction budget.\n"
              "  --analysis-max-blocks N        Analysis block budget.\n"
              "  --analysis-max-edges N         Analysis edge budget.\n"
              "  --analysis-max-seeds N         Analysis seed budget.\n"
              "  --analysis-max-bytes N         Analysis byte budget.\n"
              "  --max-ir-operations N          Global execution IR budget.\n"
              "  --max-function-transitions N   Global guest transition budget.\n"
              "  --max-call-depth N             Guest call-depth budget.\n"
              "  --max-events N                 Trace event budget.\n"
              "  --max-guest-blocks N           Guest block budget.\n";
}

void print_error(const Error& error)
{
    std::cerr << error_code_name(error.code) << ": " << error.message << '\n';
}

[[nodiscard]] bool take_value(int& index, int argc, char** argv, std::string_view option,
                              std::string& value)
{
    if (std::string_view(argv[index]) != option || index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

[[nodiscard]] bool parse_assignment(std::string_view text, std::string& left,
                                    std::string& right)
{
    const auto separator = text.find('=');
    if (separator == std::string_view::npos || separator == 0U || separator + 1U >= text.size())
    {
        return false;
    }
    left = text.substr(0U, separator);
    right = text.substr(separator + 1U);
    return !left.empty() && !right.empty();
}

} // namespace

int main(int argc, char** argv)
{
    std::string module_name = "main";
    std::uint64_t module_base = 0U;
    std::string entry_name = "dt-init";
    std::optional<std::uint64_t> analyst_address;
    std::string report_path;
    bool emit_json = false;
    std::filesystem::path input_path;
    std::filesystem::path local_config_path;
    std::vector<std::pair<std::string, std::filesystem::path>> configured_modules;
    std::map<std::string, std::uint64_t> configured_bases;
    std::string configured_primary = "main";
    bool provider_search_complete = false;
    analysis::ModuleSetCompleteness module_set_completeness =
        analysis::ModuleSetCompleteness::Incomplete;
    analysis::ModuleSetCompletenessBasis module_set_completeness_basis =
        analysis::ModuleSetCompletenessBasis::LegacyConfigFalse;
    analysis::ModuleSetCoherence module_set_coherence = analysis::ModuleSetCoherence::Unverified;
    std::string module_set_coherence_basis = "unknown";
    std::string module_set_source = "explicit";
    std::vector<std::string> expected_module_names;
    std::vector<analysis::ModuleSetIngestionOptions::ExpectedModule> expected_modules;
    std::filesystem::path module_directory;
    analysis::PreparedModuleOptions load_options;
    execution::ExecutionSessionOptions execution_options;
    analysis::FunctionMapOptions function_options;
    function_options.budgets.max_functions = 5'000U;
    function_options.budgets.max_instructions = 200'000U;
    function_options.budgets.max_blocks = 50'000U;
    function_options.budgets.max_edges = 100'000U;
    function_options.budgets.max_seeds = 10'000U;
    function_options.budgets.max_bytes_analyzed = 16U * 1024U * 1024U;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--help")
        {
            help(std::cout);
            return static_cast<int>(ExitCode::Success);
        }
        if (argument == "--version")
        {
            std::cout << version << '\n';
            return static_cast<int>(ExitCode::Success);
        }
        std::string value;
        if (take_value(index, argc, argv, "--module-name", value))
        {
            module_name = value;
            continue;
        }
        if (take_value(index, argc, argv, "--local-config", value))
        {
            local_config_path = value;
            continue;
        }
        if (take_value(index, argc, argv, "--module-directory", value))
        {
            module_directory = value;
            continue;
        }
        if (take_value(index, argc, argv, "--module", value))
        {
            std::string name;
            std::string path;
            if (!parse_assignment(value, name, path))
            {
                std::cerr << "invalid --module; expected NAME=PATH\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            configured_modules.emplace_back(std::move(name), std::filesystem::path(path));
            continue;
        }
        if (take_value(index, argc, argv, "--module-base-for", value))
        {
            std::string name;
            std::string address;
            std::uint64_t parsed_base = 0U;
            if (!parse_assignment(value, name, address) || !parse_u64(address, parsed_base))
            {
                std::cerr << "invalid --module-base-for; expected NAME=ADDR\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            configured_bases[name] = parsed_base;
            continue;
        }
        if (take_value(index, argc, argv, "--module-base", value))
        {
            if (!parse_u64(value, module_base))
            {
                std::cerr << "invalid --module-base\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }
        if (take_value(index, argc, argv, "--entry", value))
        {
            entry_name = value;
            continue;
        }
        if (take_value(index, argc, argv, "--entry-address", value))
        {
            std::uint64_t address = 0U;
            if (!parse_u64(value, address))
            {
                std::cerr << "invalid --entry-address\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            analyst_address = address;
            continue;
        }
        if (take_value(index, argc, argv, "--backend", value))
        {
            if (value != "interpreter")
            {
                std::cerr << "only --backend interpreter is implemented for M11\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }
        if (take_value(index, argc, argv, "--stack-size", value))
        {
            if (!parse_u64(value, execution_options.synthetic_stack_size))
            {
                std::cerr << "invalid --stack-size\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }
        if (take_value(index, argc, argv, "--report", report_path)) continue;
        if (argument == "--json")
        {
            emit_json = true;
            continue;
        }

        bool invalid_number = false;
        const auto parse_number = [&]<typename T>(std::string_view name, T& destination) {
            if (argument != name) return false;
            if (index + 1 >= argc)
            {
                invalid_number = true;
                return true;
            }
            value = argv[++index];
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
            {
                invalid_number = true;
                return true;
            }
            destination = static_cast<T>(parsed);
            return true;
        };
        if (parse_number("--analysis-max-functions", function_options.budgets.max_functions) ||
            parse_number("--analysis-max-instructions", function_options.budgets.max_instructions) ||
            parse_number("--analysis-max-blocks", function_options.budgets.max_blocks) ||
            parse_number("--analysis-max-edges", function_options.budgets.max_edges) ||
            parse_number("--analysis-max-seeds", function_options.budgets.max_seeds) ||
            parse_number("--analysis-max-bytes", function_options.budgets.max_bytes_analyzed) ||
            parse_number("--max-ir-operations", execution_options.budgets.max_ir_operations) ||
            parse_number("--max-function-transitions", execution_options.budgets.max_function_transitions) ||
            parse_number("--max-call-depth", execution_options.budgets.max_call_depth) ||
            parse_number("--max-events", execution_options.budgets.max_events) ||
            parse_number("--max-guest-blocks", execution_options.budgets.max_guest_blocks))
        {
            if (invalid_number)
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }

        if (!argument.empty() && argument[0] != '-' && input_path.empty())
        {
            input_path = argument;
            continue;
        }
        std::cerr << "unknown or incomplete argument: " << argument << '\n';
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    if (!local_config_path.empty())
    {
        std::vector<std::byte> config_bytes;
        if (!read_file(local_config_path, 4U * 1024U * 1024U, config_bytes))
        {
            std::cerr << "unable to read local config\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        try
        {
            const auto root = nlohmann::json::parse(
                std::string(reinterpret_cast<const char*>(config_bytes.data()), config_bytes.size()));
            const auto module_set = root.contains("module_set") && root.at("module_set").is_object()
                                        ? root.at("module_set") : nlohmann::json::object();
            const bool directory_source =
                (module_set.contains("source") && module_set.at("source").is_string() &&
                 module_set.at("source").get<std::string>() == "directory") ||
                (module_set.contains("directory") && module_set.at("directory").is_string());
            if (module_set.contains("directory") && module_set.at("directory").is_string())
                module_directory = module_set.at("directory").get<std::string>();
            const auto& module_object = module_set.contains("modules") &&
                                                module_set.at("modules").is_object()
                                            ? module_set.at("modules")
                                            : root.value("modules", nlohmann::json::object());
            if (!directory_source && !module_object.is_object())
            {
                std::cerr << "local config modules must be an object\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            if (!directory_source)
            for (const auto& [name, path] : module_object.items())
            {
                if (!path.is_string())
                {
                    std::cerr << "local config module paths must be strings\n";
                    return static_cast<int>(ExitCode::InvalidArguments);
                }
                configured_modules.emplace_back(name, path.get<std::string>());
            }
            if (root.contains("primary_module") && root.at("primary_module").is_string())
            {
                configured_primary = root.at("primary_module").get<std::string>();
            }
            if (module_set.contains("primary_module") && module_set.at("primary_module").is_string())
                configured_primary = module_set.at("primary_module").get<std::string>();
            if (root.contains("provider_search_complete") &&
                root.at("provider_search_complete").is_boolean())
            {
                provider_search_complete = root.at("provider_search_complete").get<bool>();
                if (provider_search_complete)
                {
                    module_set_completeness = analysis::ModuleSetCompleteness::DeclaredComplete;
                    module_set_completeness_basis =
                        analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion;
                }
            }
            const auto& base_object = module_set.contains("module_bases") &&
                                              module_set.at("module_bases").is_object()
                                          ? module_set.at("module_bases")
                                          : root.value("module_bases", nlohmann::json::object());
            if (base_object.is_object())
            {
                for (const auto& [name, base] : base_object.items())
                {
                    std::uint64_t parsed = 0U;
                    if (base.is_number_unsigned()) parsed = base.get<std::uint64_t>();
                    else if (base.is_string() && !parse_u64(base.get<std::string>(), parsed))
                    {
                        std::cerr << "invalid local config module base\n";
                        return static_cast<int>(ExitCode::InvalidArguments);
                    }
                    else if (!base.is_number_unsigned())
                    {
                        std::cerr << "invalid local config module base\n";
                        return static_cast<int>(ExitCode::InvalidArguments);
                    }
                    configured_bases[name] = parsed;
                }
            }
            if (module_set.is_object())
            {
                if (module_set.contains("source") && module_set.at("source").is_string())
                {
                    module_set_source = module_set.at("source").get<std::string>();
                }
                // The directory path is captured before module entries are parsed.
                if (module_set.contains("completeness") && module_set.at("completeness").is_string())
                {
                    const auto completeness = module_set.at("completeness").get<std::string>();
                    if (completeness == "declared_complete")
                        module_set_completeness = analysis::ModuleSetCompleteness::DeclaredComplete;
                    else if (completeness == "manifest_verified_complete")
                        module_set_completeness = analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
                    else if (completeness == "incomplete")
                        module_set_completeness = analysis::ModuleSetCompleteness::Incomplete;
                }
                if (module_set.contains("basis") && module_set.at("basis").is_string())
                {
                    const auto basis = module_set.at("basis").get<std::string>();
                    if (basis == "explicit_inventory")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitInventory;
                    else if (basis == "explicit_local_assertion")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion;
                    else if (basis == "local_manifest_match")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::LocalManifestMatch;
                    else if (basis == "target_manifest_match")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::TargetManifestMatch;
                    else if (basis == "directory_scan_only")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::DirectoryScanOnly;
                }
                if (module_set_completeness != analysis::ModuleSetCompleteness::Incomplete &&
                    module_set_completeness_basis == analysis::ModuleSetCompletenessBasis::LegacyConfigFalse)
                    module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitInventory;
                if (module_set.contains("expected_modules") && module_set.at("expected_modules").is_array())
                {
                    for (const auto& expected : module_set.at("expected_modules"))
                    {
                        if (expected.is_string())
                            expected_module_names.push_back(expected.get<std::string>());
                        else if (expected.is_object() && expected.contains("name") &&
                                 expected.contains("sha256") && expected.contains("build_id") &&
                                 expected.at("name").is_string() && expected.at("sha256").is_string() &&
                                 expected.at("build_id").is_string())
                        {
                            std::optional<std::uint64_t> expected_size;
                            if (expected.contains("expected_size"))
                            {
                                if (!expected.at("expected_size").is_number_unsigned())
                                {
                                    std::cerr << "invalid expected module size\n";
                                    return static_cast<int>(ExitCode::InvalidArguments);
                                }
                                expected_size = expected.at("expected_size").get<std::uint64_t>();
                            }
                            expected_modules.push_back(
                                analysis::ModuleSetIngestionOptions::ExpectedModule{
                                    expected.at("name").get<std::string>(),
                                    expected.at("sha256").get<std::string>(),
                                    expected.at("build_id").get<std::string>(), expected_size});
                            expected_module_names.push_back(expected.at("name").get<std::string>());
                        }
                        else
                        {
                            std::cerr << "invalid expected module identity\n";
                            return static_cast<int>(ExitCode::InvalidArguments);
                        }
                    }
                }
                if (module_set.contains("coherence") && module_set.at("coherence").is_string())
                {
                    const auto coherence = module_set.at("coherence").get<std::string>();
                    if (coherence == "verified") module_set_coherence = analysis::ModuleSetCoherence::Verified;
                    else if (coherence == "partially_verified") module_set_coherence = analysis::ModuleSetCoherence::PartiallyVerified;
                    else if (coherence == "conflicting") module_set_coherence = analysis::ModuleSetCoherence::Conflicting;
                }
                if (module_set.contains("coherence_basis") && module_set.at("coherence_basis").is_string())
                    module_set_coherence_basis = module_set.at("coherence_basis").get<std::string>();
            }
            analysis::ModuleSetIngestionOptions manifest_options;
            manifest_options.completeness = module_set_completeness;
            const auto manifest = apply_target_manifest(root, manifest_options);
            if (!manifest)
            {
                print_error(manifest.error());
                return static_cast<int>(ExitCode::InfrastructureFailure);
            }
            if (root.contains("target_manifest") && root.at("target_manifest").is_string())
            {
                module_set_completeness = manifest_options.completeness;
                module_set_completeness_basis = manifest_options.completeness_basis;
                module_set_coherence = manifest_options.coherence;
                module_set_coherence_basis = manifest_options.coherence_basis;
                expected_module_names = manifest_options.expected_logical_names;
                expected_modules = manifest_options.expected_modules;
            }
            if (directory_source && module_directory.empty())
            {
                std::cerr << "module_set directory source requires a directory\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "invalid local config: " << error.what() << '\n';
            return static_cast<int>(ExitCode::InvalidArguments);
        }
    }

    const bool process_mode = !configured_modules.empty() || !module_directory.empty();
    if (process_mode)
    {
        if (local_config_path.empty() && configured_primary == "main" && module_name != "main" &&
            module_directory.empty())
        {
            configured_primary = module_name;
        }
        if (!input_path.empty() && module_directory.empty())
        {
            bool has_main = false;
            for (const auto& module : configured_modules) has_main |= module.first == configured_primary;
            if (!has_main) configured_modules.emplace_back(configured_primary, input_path);
        }
        std::sort(configured_modules.begin(), configured_modules.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });
        for (std::size_t index = 1U; index < configured_modules.size(); ++index)
        {
            if (configured_modules[index - 1U].first == configured_modules[index].first)
            {
                std::cerr << "duplicate process module name\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
        }
        if (configured_primary.empty()) configured_primary = "main";
        bool has_primary = false;
        for (const auto& module : configured_modules) has_primary |= module.first == configured_primary;
        if (!has_primary && module_directory.empty())
        {
            std::cerr << "primary process module is not configured\n";
            return static_cast<int>(ExitCode::InvalidArguments);
        }

        analysis::ModuleSetInventory inventory;
        if (!module_directory.empty())
        {
            analysis::DirectoryInventoryOptions inventory_options;
            inventory_options.source = "directory";
            inventory_options.completeness = module_set_completeness;
            inventory_options.completeness_basis = module_set_completeness_basis;
            inventory_options.coherence = module_set_coherence;
            inventory_options.coherence_basis = module_set_coherence_basis;
            inventory_options.expected_logical_names = expected_module_names;
            inventory_options.expected_modules = expected_modules;
            inventory_options.explicit_bases = configured_bases;
            if (module_set_completeness == analysis::ModuleSetCompleteness::Incomplete &&
                module_set_completeness_basis == analysis::ModuleSetCompletenessBasis::LegacyConfigFalse)
                inventory_options.completeness_basis = analysis::ModuleSetCompletenessBasis::DirectoryScanOnly;
            const auto scanned = analysis::scan_prepared_module_directory(module_directory, inventory_options);
            if (!scanned) { print_error(scanned.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
            inventory = std::move(scanned).value();
            configured_modules.clear();
            for (const auto& module : inventory.modules)
                configured_modules.emplace_back(module.logical_name, std::filesystem::path{});
        }
        else
        {
            std::vector<analysis::ModuleSetFileInput> file_inputs;
            file_inputs.reserve(configured_modules.size());
            for (const auto& [name, path] : configured_modules)
            {
                const auto found = configured_bases.find(name);
                file_inputs.push_back(analysis::ModuleSetFileInput{
                    name, path, found == configured_bases.end() ? std::nullopt
                                                                 : std::optional<memory::GuestAddress>(found->second)});
            }
            analysis::ModuleSetIngestionOptions inventory_options;
            inventory_options.source = module_set_source;
            inventory_options.completeness = module_set_completeness;
            inventory_options.completeness_basis = module_set_completeness_basis;
            inventory_options.coherence = module_set_coherence;
            inventory_options.coherence_basis = module_set_coherence_basis;
            inventory_options.expected_logical_names = expected_module_names;
            inventory_options.expected_modules = expected_modules;
            const auto loaded = analysis::ingest_module_files(file_inputs, inventory_options);
            if (!loaded) { print_error(loaded.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
            inventory = std::move(loaded).value();
        }
        std::vector<analysis::ProcessModuleInput> module_inputs = inventory.process_inputs();
        bool inventory_has_primary = false;
        for (const auto& module : inventory.modules) inventory_has_primary |= module.logical_name == configured_primary;
        if (!inventory_has_primary)
        {
            std::cerr << "primary process module is not present in the ingested module set\n";
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        analysis::ProcessImageOptions process_options;
        process_options.primary_module = configured_primary;
        process_options.provider_search_complete = provider_search_complete;
        process_options.module_set_completeness = inventory.completeness;
        process_options.module_set_completeness_basis = inventory.completeness_basis;
        process_options.module_set_coherence = inventory.coherence;
        process_options.module_set_coherence_basis = inventory.coherence_basis;
        process_options.module_set_source = inventory.source;
        process_options.ignored_module_entries = inventory.ignored_entries;
        process_options.module_options = load_options;
        process_options.module_options.module_base = 0U;
        process_options.module_options.module_name = "";
        auto process = analysis::load_process_image(module_inputs, process_options);
        if (!process)
        {
            print_error(process.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        std::vector<analysis::FinalizedFunctionMap> maps;
        maps.reserve(process.value().modules().size());
        for (const auto& module : process.value().modules())
        {
            const auto map = analysis::FunctionMapBuilder::build(
                analysis::ModuleAnalysisInput{module.identity, &process.value().memory(), module.seeds},
                function_options);
            if (!map)
            {
                print_error(map.error());
                return static_cast<int>(ExitCode::InfrastructureFailure);
            }
            maps.push_back(std::move(map).value());
        }
        const auto process_map = analysis::ProcessFunctionMap::build(std::move(maps));
        if (!process_map)
        {
            print_error(process_map.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        const auto* primary_map = [&]() -> const analysis::FinalizedFunctionMap* {
            for (const auto& map : process_map.value().maps())
                if (map.identity().module == configured_primary) return &map;
            return nullptr;
        }();
        if (primary_map == nullptr)
        {
            std::cerr << "primary process module has no finalized function map\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        execution::EntrySelectionKind selection_kind;
        if (entry_name == "dt-init") selection_kind = execution::EntrySelectionKind::DynamicInit;
        else if (entry_name == "dt-fini") selection_kind = execution::EntrySelectionKind::DynamicFini;
        else if (entry_name == "text-start") selection_kind = execution::EntrySelectionKind::TextStartCandidate;
        else if (entry_name == "process") selection_kind = execution::EntrySelectionKind::VerifiedProcessEntry;
        else if (entry_name == "analyst") selection_kind = execution::EntrySelectionKind::AnalystAddress;
        else
        {
            std::cerr << "unknown --entry kind\n";
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        const auto selected = execution::select_entry(primary_map->identity(), selection_kind, analyst_address);
        if (!selected)
        {
            print_error(selected.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        runtime::RuntimeImportRegistry runtime_imports;
        const auto registered = runtime::register_m12_evidence_imports(runtime_imports);
        if (!registered)
        {
            print_error(registered.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        execution::ExecutionLoadSummary summary;
        for (const auto& module : process.value().modules())
        {
            summary.relocations_parsed += module.relocations.size();
            summary.relocations_applied += module.applied_relocations;
            summary.unresolved_relocations += module.unresolved_relocations.size();
        }
        execution::ExecutionSession session(process.value().memory(), process_map.value(),
                                            process.value(), execution_options, summary,
                                            &runtime_imports);
        const auto run = session.run(selected.value());
        if (!run)
        {
            print_error(run.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        const auto report = execution::render_execution_report_json(run.value());
        if (!report_path.empty())
        {
            std::ofstream output(report_path, std::ios::binary);
            if (!output) { std::cerr << "unable to write report\n"; return static_cast<int>(ExitCode::InfrastructureFailure); }
            output << report;
            if (!output) { std::cerr << "unable to finish report\n"; return static_cast<int>(ExitCode::InfrastructureFailure); }
        }
        if (emit_json) std::cout << report;
        else std::cout << "STOPPED: " << execution::execution_stop_reason_name(run.value().stop_reason)
                        << " pc=" << std::hex << run.value().stop_pc << std::dec << '\n';
        return static_cast<int>(ExitCode::Success);
    }

    // Keep the CLI's analysis controls separate from the metadata parser and
    // from the global execution budgets.
    load_options.module_name = module_name;
    load_options.module_base = module_base;

    if (input_path.empty())
    {
        help(std::cerr);
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    if (entry_name == "process" && analyst_address)
    {
        std::cerr << "--entry process cannot be combined with --entry-address\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    if (analyst_address && entry_name == "dt-init") entry_name = "analyst";
    std::vector<std::byte> bytes;
    if (!read_file(input_path, 512U * 1024U * 1024U, bytes))
    {
        std::cerr << "unable to read input module\n";
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    auto loaded = analysis::load_prepared_nso(bytes, load_options);
    if (!loaded)
    {
        print_error(loaded.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{loaded.value().identity, &loaded.value().memory,
                                      loaded.value().seeds},
        function_options);
    if (!map)
    {
        print_error(map.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }

    execution::EntrySelectionKind selection_kind;
    if (entry_name == "dt-init") selection_kind = execution::EntrySelectionKind::DynamicInit;
    else if (entry_name == "dt-fini") selection_kind = execution::EntrySelectionKind::DynamicFini;
    else if (entry_name == "text-start") selection_kind = execution::EntrySelectionKind::TextStartCandidate;
    else if (entry_name == "process") selection_kind = execution::EntrySelectionKind::VerifiedProcessEntry;
    else if (entry_name == "analyst") selection_kind = execution::EntrySelectionKind::AnalystAddress;
    else
    {
        std::cerr << "unknown --entry kind\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    const auto selected = execution::select_entry(map.value().identity(), selection_kind, analyst_address);
    if (!selected)
    {
        print_error(selected.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    runtime::RuntimeImportRegistry runtime_imports;
    const auto registered = runtime::register_m12_evidence_imports(runtime_imports);
    if (!registered)
    {
        print_error(registered.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    execution::ExecutionSession session(loaded.value().memory, map.value(),
                                        loaded.value().unresolved_relocations, execution_options,
                                        execution::ExecutionLoadSummary{
                                            loaded.value().relocations.size(),
                                            loaded.value().applied_relocations,
                                            loaded.value().unresolved_relocations.size()},
                                        &runtime_imports, &loaded.value().metadata,
                                        loaded.value().symbols ? &loaded.value().symbols.value()
                                                               : nullptr);
    const auto run = session.run(selected.value());
    if (!run)
    {
        print_error(run.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    const auto report = execution::render_execution_report_json(run.value());
    if (!report_path.empty())
    {
        std::ofstream output(report_path, std::ios::binary);
        if (!output)
        {
            std::cerr << "unable to write report\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        output << report;
        if (!output)
        {
            std::cerr << "unable to finish report\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
    }
    if (emit_json)
    {
        std::cout << report;
    }
    else
    {
        std::cout << "STOPPED: " << execution::execution_stop_reason_name(run.value().stop_reason)
                  << " pc=" << std::hex << run.value().stop_pc << std::dec << '\n';
    }
    return static_cast<int>(ExitCode::Success);
}
