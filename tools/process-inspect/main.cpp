#include "switchrecomp/analysis/module_set.hpp"
#include "switchrecomp/common/sha256.hpp"
#include "switchrecomp/target/manifest.hpp"
#include "switchrecomp/version.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace switchrecomp;
using json = nlohmann::json;

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
    const json& root, analysis::ModuleSetIngestionOptions& options)
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
    output << "Usage: process-inspect --local-config PATH [--json] [--report PATH]\n"
              "       process-inspect --directory PATH [--json] [--report PATH]\n\n"
              "Inspect a prepared executable module set without guest execution.\n"
              "Directory scans are non-recursive and do not attest completeness.\n";
}

void print_error(const Error& error)
{
    std::cerr << error_code_name(error.code) << ": " << error.message << '\n';
}

[[nodiscard]] analysis::ModuleSetCompleteness parse_completeness(
    const json& root, analysis::ModuleSetCompleteness fallback)
{
    if (!root.contains("completeness") || !root.at("completeness").is_string()) return fallback;
    const auto value = root.at("completeness").get<std::string>();
    if (value == "declared_complete") return analysis::ModuleSetCompleteness::DeclaredComplete;
    if (value == "manifest_verified_complete")
        return analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
    return analysis::ModuleSetCompleteness::Incomplete;
}

[[nodiscard]] analysis::ModuleSetCompletenessBasis parse_basis(
    const json& root, analysis::ModuleSetCompletenessBasis fallback)
{
    if (!root.contains("basis") || !root.at("basis").is_string()) return fallback;
    const auto value = root.at("basis").get<std::string>();
    if (value == "explicit_inventory") return analysis::ModuleSetCompletenessBasis::ExplicitInventory;
    if (value == "explicit_local_assertion")
        return analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion;
    if (value == "local_manifest_match")
        return analysis::ModuleSetCompletenessBasis::LocalManifestMatch;
    if (value == "target_manifest_match")
        return analysis::ModuleSetCompletenessBasis::TargetManifestMatch;
    if (value == "directory_scan_only") return analysis::ModuleSetCompletenessBasis::DirectoryScanOnly;
    return analysis::ModuleSetCompletenessBasis::Unknown;
}

[[nodiscard]] std::vector<std::string> expected_names(const json& root)
{
    std::vector<std::string> result;
    if (!root.contains("expected_modules") || !root.at("expected_modules").is_array()) return result;
    for (const auto& item : root.at("expected_modules"))
        if (item.is_string()) result.push_back(item.get<std::string>());
    return result;
}

[[nodiscard]] json range_json(const analysis::ModuleInventoryRange& range)
{
    return json{{"memory_offset", range.memory_offset}, {"memory_size", range.memory_size}};
}

[[nodiscard]] json inventory_json(const analysis::ModuleSetInventory& inventory)
{
    json modules = json::array();
    for (const auto& module : inventory.modules)
    {
        modules.push_back(json{
            {"logical_name", module.logical_name},
            {"name_provenance", module.name_provenance},
            {"sha256", module.sha256},
            {"build_id", module.build_id},
            {"input_size", module.input_size},
            {"nso_flags", module.nso_flags},
            {"materialization_status", module.materialization_status},
            {"ranges", json{{"text", range_json(module.text)},
                              {"rodata", range_json(module.rodata)},
                              {"data", range_json(module.data)},
                              {"bss", range_json(module.bss)}}},
            {"explicit_base", module.explicit_base
                                   ? json(module.explicit_base.value()) : json(nullptr)},
            {"selected_analysis_base", module.selected_analysis_base
                                           ? json(module.selected_analysis_base.value()) : json(nullptr)},
            {"base_provenance", module.base_provenance},
            {"runtime_base_verified", module.runtime_base_verified},
            {"mod0_status", module.mod0_status},
            {"dynamic_status", module.dynamic_status},
            {"dynamic_symbol_count", module.dynamic_symbol_count},
            {"defined_symbol_count", module.defined_symbol_count},
            {"undefined_symbol_count", module.undefined_symbol_count},
            {"relocation_count", module.relocation_count},
            {"executable_mapping", module.executable_mapping},
            {"module_load_valid", module.module_load_valid},
            {"provider_index_eligible", module.provider_index_eligible},
            {"executable_guest_mappings", [&]() {
                json mappings = json::array();
                for (const auto& range : module.executable_guest_mappings)
                    mappings.push_back(json{{"base", range.base}, {"size", range.size}});
                return mappings;
            }()}});
    }
    return json{{"source", inventory.source},
                {"completeness", analysis::module_set_completeness_name(inventory.completeness)},
                {"completeness_basis", analysis::module_set_completeness_basis_name(
                                           inventory.completeness_basis)},
                {"coherence", analysis::module_set_coherence_name(inventory.coherence)},
                {"coherence_basis", inventory.coherence_basis},
                {"module_count", inventory.modules.size()},
                {"executable_module_count", std::count_if(
                                                 inventory.modules.begin(), inventory.modules.end(),
                                                 [](const auto& module) { return module.executable_mapping; })},
                {"modules", std::move(modules)},
                {"ignored_entries", inventory.ignored_entries}};
}

[[nodiscard]] json process_json(const analysis::ProcessImage& process)
{
    const auto summary = process.summary();
    json modules = json::array();
    for (const auto& module : summary.modules)
    {
        modules.push_back(json{{"logical_name", module.logical_name},
                               {"name_provenance", module.name_provenance},
                               {"sha256", module.sha256},
                               {"build_id", module.build_id},
                               {"input_size", module.input_size},
                               {"analysis_base", module.base},
                               {"base_provenance", analysis::module_base_provenance_name(module.base_provenance)},
                               {"runtime_base_verified", module.runtime_base_verified},
                               {"nso_flags", module.nso_flags},
                               {"dynamic_metadata", json{{"mod0", module.mod0_status},
                                                            {"dynamic", module.dynamic_status}}},
                               {"dynamic_symbols", json{{"total", module.dynamic_symbol_count},
                                                         {"defined", module.defined_symbol_count},
                                                         {"undefined", module.undefined_symbol_count}}},
                               {"relocations", json{{"total", module.relocation_count},
                                                      {"applied", module.applied_relocations},
                                                      {"unresolved", module.unresolved_relocations}}}});
    }
    const auto lookup = process.lookup_provider("__nnmusl_init_dso");
    json occurrences = json::array();
    for (const auto& occurrence : lookup.occurrences)
    {
        occurrences.push_back(json{{"module", occurrence.module},
                                   {"symbol_index", occurrence.symbol_index},
                                   {"symbol", occurrence.symbol},
                                   {"defined", occurrence.defined},
                                   {"binding", format::symbol_binding_name(occurrence.binding)},
                                   {"type", format::symbol_type_name(occurrence.type)},
                                   {"visibility", format::symbol_visibility_name(occurrence.visibility)},
                                   {"section_index", occurrence.section_index},
                                   {"value", occurrence.value},
                                   {"guest_address", occurrence.address
                                                           ? json(occurrence.address.value()) : json(nullptr)},
                                   {"executable", occurrence.executable},
                                   {"eligible", occurrence.eligible},
                                   {"eligibility", provider_eligibility_name(occurrence.eligibility)}});
    }
    json candidates = json::array();
    for (const auto& candidate : lookup.candidates)
    {
        candidates.push_back(json{{"module", candidate.module},
                                  {"symbol_index", candidate.symbol_index},
                                  {"symbol", candidate.symbol},
                                  {"binding", format::symbol_binding_name(candidate.binding)},
                                  {"type", format::symbol_type_name(candidate.type)},
                                  {"visibility", format::symbol_visibility_name(candidate.visibility)},
                                  {"value", candidate.value},
                                  {"guest_address", candidate.address},
                                  {"executable", candidate.executable}});
    }
    json provider = nullptr;
    if (lookup.selected_candidate && lookup.selected_candidate.value() < lookup.candidates.size())
    {
        const auto& selected = lookup.candidates[lookup.selected_candidate.value()];
        provider = json{{"module", selected.module},
                        {"symbol_index", selected.symbol_index},
                        {"guest_address", selected.address}};
    }
    return json{{"primary_module", summary.primary_module},
                {"module_set", json{{"source", summary.source},
                                     {"completeness", module_set_completeness_name(summary.completeness)},
                                     {"completeness_basis", module_set_completeness_basis_name(
                                                                summary.completeness_basis)},
                                     {"coherence", module_set_coherence_name(summary.coherence)},
                                     {"coherence_basis", summary.coherence_basis},
                                     {"module_count", summary.module_count},
                                     {"executable_module_count", summary.executable_module_count}}},
                {"modules", std::move(modules)},
                {"provider_searches", json::array({json{
                    {"symbol", "__nnmusl_init_dso"},
                    {"completeness", module_set_completeness_name(lookup.completeness)},
                    {"completeness_basis", module_set_completeness_basis_name(lookup.completeness_basis)},
                    {"result", provider_resolution_status_name(lookup.status)},
                    {"candidate_count", lookup.candidates.size()},
                    {"provider", std::move(provider)},
                    {"selected_candidate", lookup.selected_candidate
                                                 ? json(lookup.selected_candidate.value()) : json(nullptr)},
                    {"candidates", std::move(candidates)},
                    {"occurrences", std::move(occurrences)}}})},
                {"relocation_plan", json{{"planned", summary.relocations_planned},
                                          {"transactional_success", summary.transactional_relocation_success}}}};
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path config_path;
    std::filesystem::path directory_path;
    std::string report_path;
    std::string primary_module = "main";
    bool json_output = false;
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
        if (argument == "--json")
        {
            json_output = true;
            continue;
        }
        if ((argument == "--local-config" || argument == "--directory" || argument == "--report") &&
            index + 1 < argc)
        {
            const auto value = std::filesystem::path(argv[++index]);
            if (argument == "--local-config") config_path = value;
            else if (argument == "--directory") directory_path = value;
            else report_path = value.string();
            continue;
        }
        std::cerr << "unknown or incomplete argument: " << argument << '\n';
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    if (!config_path.empty() && !directory_path.empty())
    {
        std::cerr << "choose either --local-config or --directory\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    analysis::ModuleSetInventory inventory;
    if (!directory_path.empty())
    {
        analysis::DirectoryInventoryOptions options;
        options.source = "directory";
        options.completeness_basis = analysis::ModuleSetCompletenessBasis::DirectoryScanOnly;
        const auto scanned = analysis::scan_prepared_module_directory(directory_path, options);
        if (!scanned) { print_error(scanned.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
        inventory = std::move(scanned).value();
    }
    else if (!config_path.empty())
    {
        std::vector<std::byte> config_bytes;
        if (!read_file(config_path, 4U * 1024U * 1024U, config_bytes))
        {
            std::cerr << "unable to read local config\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        try
        {
            const auto root = json::parse(std::string(reinterpret_cast<const char*>(config_bytes.data()),
                                                      config_bytes.size()));
            if (root.contains("primary_module") && root.at("primary_module").is_string())
                primary_module = root.at("primary_module").get<std::string>();
            const auto set = root.contains("module_set") && root.at("module_set").is_object()
                                 ? root.at("module_set") : json::object();
            if (set.contains("primary_module") && set.at("primary_module").is_string())
                primary_module = set.at("primary_module").get<std::string>();
            const bool directory_mode =
                (set.contains("source") && set.at("source").is_string() &&
                 set.at("source").get<std::string>() == "directory") ||
                set.contains("directory");
            analysis::DirectoryInventoryOptions directory_options;
            analysis::ModuleSetIngestionOptions options;
            options.source = directory_mode ? "directory" : "explicit";
            options.completeness = root.contains("provider_search_complete") &&
                                           root.at("provider_search_complete").is_boolean() &&
                                           root.at("provider_search_complete").get<bool>()
                                       ? analysis::ModuleSetCompleteness::DeclaredComplete
                                       : analysis::ModuleSetCompleteness::Incomplete;
            options.completeness_basis = options.completeness == analysis::ModuleSetCompleteness::DeclaredComplete
                                             ? analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion
                                             : analysis::ModuleSetCompletenessBasis::LegacyConfigFalse;
            const auto& base_object = set.contains("module_bases") && set.at("module_bases").is_object()
                                          ? set.at("module_bases")
                                          : root.value("module_bases", json::object());
            if (base_object.is_object())
            {
                for (const auto& [name, value] : base_object.items())
                {
                    std::uint64_t base = 0U;
                    if ((value.is_string() && !parse_u64(value.get<std::string>(), base)) ||
                        (!value.is_string() && !value.is_number_unsigned()))
                    {
                        std::cerr << "invalid module base\n";
                        return static_cast<int>(ExitCode::InvalidArguments);
                    }
                    if (value.is_number_unsigned()) base = value.get<std::uint64_t>();
                    directory_options.explicit_bases[name] = base;
                }
            }
            if (!set.is_null())
            {
                options.completeness = parse_completeness(set, options.completeness);
                options.completeness_basis = parse_basis(set, options.completeness_basis);
                options.expected_logical_names = expected_names(set);
                if (set.contains("expected_modules") && set.at("expected_modules").is_array())
                {
                    for (const auto& expected : set.at("expected_modules"))
                    {
                        if (!expected.is_object() || !expected.contains("name") ||
                            !expected.contains("sha256") || !expected.contains("build_id") ||
                            !expected.at("name").is_string() || !expected.at("sha256").is_string() ||
                            !expected.at("build_id").is_string())
                        {
                            std::cerr << "invalid expected module identity\n";
                            return static_cast<int>(ExitCode::InvalidArguments);
                        }
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
                        options.expected_modules.push_back(
                            analysis::ModuleSetIngestionOptions::ExpectedModule{
                                expected.at("name").get<std::string>(),
                                expected.at("sha256").get<std::string>(),
                                expected.at("build_id").get<std::string>(), expected_size});
                    }
                }
                if (options.completeness != analysis::ModuleSetCompleteness::Incomplete &&
                    options.completeness_basis == analysis::ModuleSetCompletenessBasis::LegacyConfigFalse)
                    options.completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitInventory;
                if (set.contains("coherence") && set.at("coherence").is_string())
                {
                    const auto coherence = set.at("coherence").get<std::string>();
                    if (coherence == "verified") options.coherence = analysis::ModuleSetCoherence::Verified;
                    else if (coherence == "partially_verified")
                        options.coherence = analysis::ModuleSetCoherence::PartiallyVerified;
                    else if (coherence == "conflicting") options.coherence = analysis::ModuleSetCoherence::Conflicting;
                }
                if (set.contains("coherence_basis") && set.at("coherence_basis").is_string())
                    options.coherence_basis = set.at("coherence_basis").get<std::string>();
            }
            const auto manifest = apply_target_manifest(root, options);
            if (!manifest)
            {
                print_error(manifest.error());
                return static_cast<int>(ExitCode::InfrastructureFailure);
            }
            if (directory_mode)
            {
                if (!set.contains("directory") || !set.at("directory").is_string())
                {
                    std::cerr << "module_set directory source requires a directory\n";
                    return static_cast<int>(ExitCode::InvalidArguments);
                }
                directory_options.source = "directory";
                directory_options.completeness = options.completeness;
                directory_options.completeness_basis = options.completeness_basis;
                directory_options.coherence = options.coherence;
                directory_options.coherence_basis = options.coherence_basis;
                directory_options.expected_logical_names = options.expected_logical_names;
                directory_options.expected_modules = options.expected_modules;
                const auto scanned = analysis::scan_prepared_module_directory(
                    set.at("directory").get<std::string>(), directory_options);
                if (!scanned) { print_error(scanned.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
                inventory = std::move(scanned).value();
            }
            else
            {
                const auto& module_object = set.contains("modules") && set.at("modules").is_object()
                                                ? set.at("modules") : root.value("modules", json::object());
                if (!module_object.is_object())
                {
                    std::cerr << "local config modules must be an object\n";
                    return static_cast<int>(ExitCode::InvalidArguments);
                }
                std::vector<analysis::ModuleSetFileInput> inputs;
                for (const auto& [name, value] : module_object.items())
                {
                    if (!value.is_string()) { std::cerr << "module paths must be strings\n"; return 2; }
                    const auto found = directory_options.explicit_bases.find(name);
                    inputs.push_back(analysis::ModuleSetFileInput{
                        name, value.get<std::string>(),
                        found == directory_options.explicit_bases.end()
                            ? std::nullopt
                            : std::optional<memory::GuestAddress>(found->second)});
                }
                const auto loaded = analysis::ingest_module_files(inputs, options);
                if (!loaded) { print_error(loaded.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
                inventory = std::move(loaded).value();
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "invalid local config: " << error.what() << '\n';
            return static_cast<int>(ExitCode::InvalidArguments);
        }
    }
    else
    {
        std::cerr << "one of --local-config or --directory is required\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    analysis::ProcessImageOptions process_options;
    process_options.primary_module = "main";
    process_options.module_set_completeness = inventory.completeness;
    process_options.module_set_completeness_basis = inventory.completeness_basis;
    process_options.module_set_coherence = inventory.coherence;
    process_options.module_set_coherence_basis = inventory.coherence_basis;
    process_options.module_set_source = inventory.source;
    process_options.ignored_module_entries = inventory.ignored_entries;
    process_options.plan_relocations = false;
    process_options.apply_relocations = false;
    if (!inventory.modules.empty())
    {
        bool has_main = false;
        for (const auto& module : inventory.modules) has_main |= module.logical_name == primary_module;
        if (has_main) process_options.primary_module = primary_module;
        else if (!inventory.modules.empty()) process_options.primary_module = inventory.modules.front().logical_name;
    }
    auto inputs = inventory.process_inputs();
    const auto process = analysis::load_process_image(inputs, process_options);
    if (!process) { print_error(process.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
    inventory.enrich_from_process_summary(process.value().summary());
    const auto report = json{{"schema_version", 4U},
                             {"inventory", inventory_json(inventory)},
                             {"process", process_json(process.value())}}.dump(2) + "\n";
    if (!report_path.empty())
    {
        std::ofstream output(report_path, std::ios::binary);
        if (!output) { std::cerr << "unable to write report\n"; return 3; }
        output << report;
    }
    if (json_output || report_path.empty()) std::cout << report;
    return static_cast<int>(ExitCode::Success);
}
