#include "switchrecomp/analysis/module_set.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/common/sha256.hpp"
#include "switchrecomp/format/nso.hpp"
#include "switchrecomp/format/nso_magic.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace switchrecomp::analysis
{

namespace
{

[[nodiscard]] bool safe_label(std::string_view value, std::size_t max_size = 128U) noexcept
{
    if (value.empty() || value.size() > max_size || value == "." || value == "..") return false;
    for (const auto character : value)
    {
        const bool alpha = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!alpha && !digit && character != '_' && character != '-' && character != '.') return false;
    }
    return true;
}

void normalize_completeness_provenance(ModuleSetIngestionOptions& options) noexcept
{
    if (options.completeness != ModuleSetCompleteness::Incomplete &&
        options.completeness_basis == ModuleSetCompletenessBasis::LegacyConfigFalse)
    {
        options.completeness_basis = ModuleSetCompletenessBasis::ExplicitInventory;
    }
}

} // namespace

[[nodiscard]] Result<void> validate_expected_names(
    const std::vector<ModuleInventoryEntry>& modules,
    const std::vector<std::string>& expected)
{
    if (expected.empty()) return Result<void>::success();
    if (!std::all_of(expected.begin(), expected.end(), [](const auto& item) {
            return safe_label(item);
        }))
    {
        return Result<void>::failure(make_error(
            ErrorCode::ModuleManifestMismatch,
            "expected module inventory contains an unsafe logical module name"));
    }
    std::vector<std::string> actual;
    actual.reserve(modules.size());
    for (const auto& module : modules) actual.push_back(module.logical_name);
    auto normalized_expected = expected;
    std::sort(actual.begin(), actual.end());
    std::sort(normalized_expected.begin(), normalized_expected.end());
    if (actual != normalized_expected)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ModuleManifestMismatch,
            "module inventory does not match the expected logical module inventory"));
    }
    return Result<void>::success();
}

[[nodiscard]] std::string lower_ascii(std::string value)
{
    for (auto& character : value)
    {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

struct load_order_key
{
    int rank = 4;
    std::uint64_t subsdk_number = 0U;
    bool numeric_subsdk = false;
};

[[nodiscard]] load_order_key exefs_load_order_key(std::string_view name) noexcept
{
    if (name == "rtld") return {0, 0U, false};
    if (name == "main") return {1, 0U, false};
    if (name == "sdk") return {3, 0U, false};
    constexpr std::string_view prefix = "subsdk";
    if (name.size() > prefix.size() && name.substr(0U, prefix.size()) == prefix)
    {
        std::uint64_t number = 0U;
        for (const auto digit : name.substr(prefix.size()))
        {
            if (digit < '0' || digit > '9') return {4, 0U, false};
            const auto next = checked_mul_u64(number, 10U);
            if (!next) return {4, 0U, false};
            const auto accumulated = checked_add_u64(
                next.value(), static_cast<std::uint64_t>(digit - '0'));
            if (!accumulated) return {4, 0U, false};
            number = accumulated.value();
        }
        return {2, number, true};
    }
    return {4, 0U, false};
}

[[nodiscard]] std::pair<std::vector<std::string>, std::string> exefs_load_order(
    const std::vector<ModuleInventoryEntry>& modules)
{
    std::vector<std::string> names;
    names.reserve(modules.size());
    bool all_known = true;
    for (const auto& module : modules)
    {
        names.push_back(module.logical_name);
        all_known &= exefs_load_order_key(module.logical_name).rank != 4;
    }
    std::sort(names.begin(), names.end(), [](const auto& left, const auto& right) {
        const auto left_key = exefs_load_order_key(left);
        const auto right_key = exefs_load_order_key(right);
        if (left_key.rank != right_key.rank) return left_key.rank < right_key.rank;
        if (left_key.numeric_subsdk && right_key.numeric_subsdk &&
            left_key.subsdk_number != right_key.subsdk_number)
            return left_key.subsdk_number < right_key.subsdk_number;
        return left < right;
    });
    return {std::move(names), all_known ? "public_exefs_load_order"
                                         : "public_exefs_load_order_with_unclassified_modules"};
}

[[nodiscard]] Result<void> validate_expected_modules(
    const std::vector<ModuleInventoryEntry>& modules,
    const std::vector<ModuleSetIngestionOptions::ExpectedModule>& expected)
{
    if (expected.empty()) return Result<void>::success();
    std::set<std::string> expected_names;
    for (const auto& item : expected)
    {
        if (!safe_label(item.logical_name) || !expected_names.insert(item.logical_name).second ||
            item.sha256.empty() || item.build_id.empty())
        {
            return Result<void>::failure(make_error(
                ErrorCode::ModuleManifestMismatch,
                "expected module identity contains an empty or duplicate field"));
        }
        const auto found = std::find_if(modules.begin(), modules.end(), [&](const auto& module) {
            return module.logical_name == item.logical_name;
        });
        if (found == modules.end() || lower_ascii(found->sha256) != lower_ascii(item.sha256) ||
            lower_ascii(found->build_id) != lower_ascii(item.build_id) ||
            (item.input_size && found->input_size != item.input_size.value()))
        {
            return Result<void>::failure(make_error(
                ErrorCode::ModuleManifestMismatch,
                "module inventory identity does not match the expected manifest"));
        }
    }
    if (expected_names.size() != modules.size())
    {
        return Result<void>::failure(make_error(
            ErrorCode::ModuleManifestMismatch,
            "module inventory does not contain exactly the expected manifest modules"));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<ModuleSetInventory> validate_impl(
    std::span<const ModuleSetByteInput> inputs, const ModuleSetIngestionOptions& options)
{
    if (inputs.empty())
    {
        return Result<ModuleSetInventory>::failure(
            make_error(ErrorCode::ModuleSetEmpty, "executable module set is empty"));
    }
    if (options.max_modules == 0U || inputs.size() > options.max_modules)
    {
        return Result<ModuleSetInventory>::failure(
            make_error(ErrorCode::ResourceLimit, "executable module count exceeds the ingestion budget"));
    }

    ModuleSetInventory result;
    result.completeness = options.completeness;
    result.completeness_basis = options.completeness_basis;
    result.coherence = options.coherence;
    result.coherence_basis = options.coherence_basis;
    result.source = options.source;
    if (!safe_label(result.source) || !safe_label(result.coherence_basis))
    {
        return Result<ModuleSetInventory>::failure(make_error(
            ErrorCode::InvalidArgument, "module-set provenance labels must be stable labels"));
    }
    result.modules.reserve(inputs.size());
    result.owned_modules_.reserve(inputs.size());
    std::set<std::string> names;
    std::set<std::string> digests;
    try
    {
        for (const auto& input : inputs)
        {
            if (!safe_label(input.logical_name) || !safe_label(input.name_provenance) ||
                !names.insert(input.logical_name).second)
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::DuplicateModuleIdentity,
                    "module inventory contains an empty or duplicate logical module name"));
            }
            if (input.file_bytes.empty())
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ModuleParseFailed,
                    "module inventory contains an empty executable input"));
            }
            if (input.file_bytes.size() > options.max_file_size)
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ResourceLimit,
                    "executable module exceeds the configured input-size budget"));
            }
            const auto digest = sha256_bytes(input.file_bytes);
            if (!digest) return Result<ModuleSetInventory>::failure(digest.error());
            const auto sha = sha256_to_hex(digest.value());
            if (!digests.insert(sha).second)
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ModuleSetIdentityConflict,
                    "the same exact executable module was supplied under multiple logical names"));
            }
            const auto header = format::parse_nso_header(input.file_bytes);
            if (!header)
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ModuleParseFailed,
                    "executable module failed NSO parsing: " + header.error().message));
            }
            const auto image = format::materialize_nso(input.file_bytes, header.value());
            if (!image)
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ModuleMaterializationFailed,
                    "executable module failed materialization: " + image.error().message));
            }
            const auto bss_offset = checked_add_u64(header.value().data.memory_offset,
                                                    header.value().data.memory_size);
            if (!bss_offset || bss_offset.value() > std::numeric_limits<std::uint32_t>::max())
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ModuleParseFailed, "NSO data/BSS memory range overflows"));
            }

            result.owned_modules_.push_back(ModuleSetInventory::OwnedModule{
                input.logical_name, input.name_provenance,
                std::vector<std::byte>(input.file_bytes.begin(), input.file_bytes.end()),
                input.explicit_base});
            result.modules.push_back(ModuleInventoryEntry{
                input.logical_name,
                input.name_provenance,
                sha,
                format::module_id_hex(header.value()),
                static_cast<std::uint64_t>(input.file_bytes.size()),
                header.value().flags,
                "materialized",
                {header.value().text.memory_offset, header.value().text.memory_size},
                {header.value().rodata.memory_offset, header.value().rodata.memory_size},
                {header.value().data.memory_offset, header.value().data.memory_size},
                {static_cast<std::uint32_t>(bss_offset.value()),
                 header.value().bss_size},
                input.explicit_base,
                input.explicit_base,
                input.explicit_base ? "explicit_analysis_base" : "pending_process_layout",
                false,
                "not_parsed",
                "not_parsed",
                0U,
                0U,
                0U,
                0U,
                header.value().text.memory_size != 0U,
                true,
                false,
                {}});
        }
        std::sort(result.modules.begin(), result.modules.end(), [](const auto& left, const auto& right) {
            return left.logical_name < right.logical_name;
        });
        std::sort(result.owned_modules_.begin(), result.owned_modules_.end(),
                  [](const auto& left, const auto& right) {
                      return left.logical_name < right.logical_name;
                  });
        auto load_order = exefs_load_order(result.modules);
        result.module_load_order = std::move(load_order.first);
        result.module_load_order_basis = std::move(load_order.second);
        const auto expected = validate_expected_names(result.modules, options.expected_logical_names);
        if (!expected)
        {
            result.coherence = ModuleSetCoherence::Conflicting;
            return Result<ModuleSetInventory>::failure(expected.error());
        }
        const auto expected_identities = validate_expected_modules(result.modules, options.expected_modules);
        if (!expected_identities)
        {
            result.coherence = ModuleSetCoherence::Conflicting;
            return Result<ModuleSetInventory>::failure(expected_identities.error());
        }
        if (options.completeness == ModuleSetCompleteness::ManifestVerifiedComplete &&
            options.expected_modules.empty())
        {
            return Result<ModuleSetInventory>::failure(make_error(
                ErrorCode::ModuleManifestMismatch,
                "manifest-verified completeness requires expected module hashes and Build IDs"));
        }
        return Result<ModuleSetInventory>::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Result<ModuleSetInventory>::failure(
            make_error(ErrorCode::ResourceLimit, "executable module inventory allocation failed"));
    }
}

namespace
{

[[nodiscard]] bool read_bounded_file(const std::filesystem::path& path, std::size_t max_size,
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

[[nodiscard]] bool starts_with_nso(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::array<std::byte, 4> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    const auto count = input.gcount();
    if (count < 4) return false;
    const auto status = format::inspect_nso_magic(magic);
    return status && status.value() == format::NsoMagicStatus::Valid;
}

[[nodiscard]] std::string directory_logical_name(const std::filesystem::path& path)
{
    const auto filename = path.filename().string();
    if (path.extension() == ".nso") return path.stem().string();
    return filename;
}

} // namespace

std::vector<ProcessModuleInput> ModuleSetInventory::process_inputs() const
{
    std::vector<ProcessModuleInput> result;
    result.reserve(owned_modules_.size());
    for (const auto& module : owned_modules_)
    {
        result.push_back(ProcessModuleInput{module.logical_name, module.bytes,
                                            module.explicit_base, module.name_provenance});
    }
    return result;
}

void ModuleSetInventory::enrich_from_process_summary(const ProcessImageSummary& summary)
{
    for (auto& inventory_module : modules)
    {
        const auto found = std::find_if(summary.modules.begin(), summary.modules.end(),
                                        [&](const auto& module) {
                                            return module.logical_name == inventory_module.logical_name;
                                        });
        if (found == summary.modules.end()) continue;
        inventory_module.selected_analysis_base = found->base;
        inventory_module.base_provenance = std::string(module_base_provenance_name(found->base_provenance));
        inventory_module.runtime_base_verified = found->runtime_base_verified;
        inventory_module.mod0_status = found->mod0_status;
        inventory_module.dynamic_status = found->dynamic_status;
        inventory_module.dynamic_symbol_count = found->dynamic_symbol_count;
        inventory_module.defined_symbol_count = found->defined_symbol_count;
        inventory_module.undefined_symbol_count = found->undefined_symbol_count;
        inventory_module.relocation_count = found->relocation_count;
        inventory_module.executable_mapping = std::any_of(
            found->mappings.begin(), found->mappings.end(), [](const auto& mapping) {
                return memory::has_permission(mapping.permissions, memory::GuestMemoryPermissions::Execute);
            });
        inventory_module.module_load_valid = true;
        inventory_module.provider_index_eligible = found->provider_index_eligible;
        inventory_module.executable_guest_mappings.clear();
        for (const auto& mapping : found->mappings)
        {
            if (memory::has_permission(mapping.permissions, memory::GuestMemoryPermissions::Execute))
                inventory_module.executable_guest_mappings.push_back(
                    GuestAddressRange{mapping.base, mapping.size});
        }
    }
}

Result<ModuleSetInventory> validate_module_set(
    std::span<const ModuleSetByteInput> inputs, const ModuleSetIngestionOptions& options)
{
    auto effective_options = options;
    normalize_completeness_provenance(effective_options);
    return validate_impl(inputs, effective_options);
}

Result<ModuleSetInventory> ingest_module_files(
    std::span<const ModuleSetFileInput> inputs, const ModuleSetIngestionOptions& options)
{
    try
    {
        auto effective_options = options;
        normalize_completeness_provenance(effective_options);
        if (inputs.empty())
        {
            return Result<ModuleSetInventory>::failure(make_error(
                ErrorCode::ModuleSetEmpty, "executable module set is empty"));
        }
        std::vector<std::vector<std::byte>> storage;
        std::vector<ModuleSetByteInput> bytes;
        storage.reserve(inputs.size());
        bytes.reserve(inputs.size());
        for (const auto& input : inputs)
        {
            storage.emplace_back();
            if (!read_bounded_file(input.path, effective_options.max_file_size, storage.back()))
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ModuleMaterializationFailed,
                    "unable to read or bounded-read executable module " + input.logical_name));
            }
            bytes.push_back(ModuleSetByteInput{input.logical_name, storage.back(), input.explicit_base,
                                               input.name_provenance});
        }
        return validate_impl(bytes, effective_options);
    }
    catch (const std::bad_alloc&)
    {
        return Result<ModuleSetInventory>::failure(
            make_error(ErrorCode::ResourceLimit, "executable module file inventory allocation failed"));
    }
}

Result<ModuleSetInventory> scan_prepared_module_directory(
    const std::filesystem::path& directory, const DirectoryInventoryOptions& options)
{
    try
    {
        auto effective_options = options;
        normalize_completeness_provenance(effective_options);
        if (effective_options.source == "explicit") effective_options.source = "directory";
        if (effective_options.completeness == ModuleSetCompleteness::Incomplete &&
            effective_options.completeness_basis == ModuleSetCompletenessBasis::LegacyConfigFalse)
            effective_options.completeness_basis = ModuleSetCompletenessBasis::DirectoryScanOnly;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(directory, error);
        if (error || !std::filesystem::is_directory(status))
        {
            return Result<ModuleSetInventory>::failure(make_error(
                ErrorCode::DirectoryScanFailed, "prepared executable module directory is unavailable"));
        }
        std::vector<std::filesystem::directory_entry> entries;
        std::vector<std::string> ignored;
        std::size_t seen_entries = 0U;
        for (std::filesystem::directory_iterator iterator(directory, error), end; iterator != end;
             iterator.increment(error))
        {
            if (error)
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::DirectoryScanFailed, "prepared module directory enumeration failed"));
            }
            if (++seen_entries > effective_options.max_directory_entries)
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ResourceLimit, "prepared module directory exceeds the entry budget"));
            }
            const auto entry_status = iterator->symlink_status(error);
            if (error) continue;
            if (std::filesystem::is_regular_file(entry_status)) entries.push_back(*iterator);
            else ignored.push_back(iterator->path().filename().string() + ":not_regular_file");
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.path().filename().string() < right.path().filename().string();
        });

        std::vector<ModuleSetByteInput> inputs;
        std::vector<std::vector<std::byte>> storage;
        inputs.reserve(entries.size());
        storage.reserve(entries.size());
        for (const auto& entry : entries)
        {
            const auto name = entry.path().filename().string();
            if (!starts_with_nso(entry.path()))
            {
                ignored.push_back(name + ":not_nso");
                continue;
            }
            storage.emplace_back();
            if (!read_bounded_file(entry.path(), effective_options.max_file_size, storage.back()))
            {
                return Result<ModuleSetInventory>::failure(make_error(
                    ErrorCode::ModuleMaterializationFailed,
                    "NSO directory entry could not be safely read: " + name));
            }
            const auto logical_name = directory_logical_name(entry.path());
            const auto found_base = effective_options.explicit_bases.find(logical_name);
            inputs.push_back(ModuleSetByteInput{
                logical_name, storage.back(),
                found_base == effective_options.explicit_bases.end()
                    ? std::nullopt
                    : std::optional<memory::GuestAddress>(found_base->second),
                "directory_filename_hint"});
        }
        auto result = validate_impl(inputs, effective_options);
        if (!result) return result;
        result.value().ignored_entries.insert(result.value().ignored_entries.end(),
                                              ignored.begin(), ignored.end());
        std::sort(result.value().ignored_entries.begin(), result.value().ignored_entries.end());
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return Result<ModuleSetInventory>::failure(
            make_error(ErrorCode::ResourceLimit, "prepared module directory allocation failed"));
    }
}

} // namespace switchrecomp::analysis
