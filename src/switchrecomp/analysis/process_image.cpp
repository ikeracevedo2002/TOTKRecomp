#include "switchrecomp/analysis/process_image.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/common/sha256.hpp"
#include "switchrecomp/format/mod0.hpp"
#include "switchrecomp/loader/nso_guest_loader.hpp"
#include "switchrecomp/version.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <utility>

namespace switchrecomp::analysis
{

namespace
{

using GuestAddress = memory::GuestAddress;

struct StagedModule
{
    std::string name;
    std::string name_provenance;
    std::uint64_t input_size = 0U;
    format::NsoHeader header{};
    format::NsoImage image;
    Sha256Digest digest{};
    GuestAddress base = 0U;
    ModuleBaseProvenance base_provenance = ModuleBaseProvenance::ExplicitAnalysisBase;
};

[[nodiscard]] Result<GuestAddress> align_up(GuestAddress value, GuestAddress alignment)
{
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
    {
        return Result<GuestAddress>::failure(
            make_error(ErrorCode::InvalidArgument, "module alignment must be a non-zero power of two"));
    }
    const auto rounded = checked_add_u64(value, alignment - 1U);
    if (!rounded)
    {
        return Result<GuestAddress>::failure(rounded.error());
    }
    return Result<GuestAddress>::success(rounded.value() & ~(alignment - 1U));
}

[[nodiscard]] Result<GuestAddress> image_span(const format::NsoImage& image)
{
    GuestAddress end = 0U;
    const auto update = [&](std::uint64_t offset, std::size_t size) -> Result<void> {
        const auto candidate = checked_add_u64(offset, static_cast<std::uint64_t>(size));
        if (!candidate)
        {
            return Result<void>::failure(make_error(
                ErrorCode::ModuleLayoutOverflow, "NSO image segment range overflows"));
        }
        end = std::max(end, candidate.value());
        return Result<void>::success();
    };
    for (const auto& segment : {std::pair<std::uint64_t, std::size_t>{image.text.memory_offset,
                                                                       image.text.bytes.size()},
                                {image.rodata.memory_offset, image.rodata.bytes.size()},
                                {image.data.memory_offset, image.data.bytes.size()},
                                {image.bss_memory_offset, image.bss.size()}})
    {
        const auto valid = update(segment.first, segment.second);
        if (!valid) return Result<GuestAddress>::failure(valid.error());
    }
    return Result<GuestAddress>::success(end);
}

[[nodiscard]] bool ranges_overlap(GuestAddress left_base, GuestAddress left_end,
                                  GuestAddress right_base, GuestAddress right_end) noexcept
{
    return left_base < right_end && right_base < left_end;
}

[[nodiscard]] bool stable_report_label(std::string_view value, std::size_t max_size = 256U) noexcept
{
    if (value.empty() || value.size() > max_size) return false;
    for (const auto character : value)
    {
        if (character == '/' || character == '\\' ||
            static_cast<unsigned char>(character) < 0x20U)
            return false;
    }
    return true;
}

[[nodiscard]] Result<GuestAddress> symbol_address(const format::DynamicSymbol& symbol,
                                                  GuestAddress base)
{
    constexpr std::uint16_t shn_abs = 0xfff1U;
    if (symbol.section_index == shn_abs)
    {
        return Result<GuestAddress>::success(symbol.value);
    }
    const auto address = checked_add_u64(base, symbol.value);
    if (!address)
    {
        return Result<GuestAddress>::failure(make_error(
            ErrorCode::InvalidProviderDefinition,
            "dynamic symbol value plus provider module base overflows"));
    }
    return address;
}

[[nodiscard]] ProviderEligibility provider_eligibility(
    const format::DynamicSymbol& symbol, bool mapped, bool executable) noexcept
{
    if (symbol.name.empty()) return ProviderEligibility::NoName;
    if (!symbol.is_defined()) return ProviderEligibility::Undefined;
    if (symbol.binding == format::SymbolBinding::Local) return ProviderEligibility::LocalBinding;
    if (symbol.binding != format::SymbolBinding::Global &&
        symbol.binding != format::SymbolBinding::Weak)
        return ProviderEligibility::IneligibleBinding;
    if (symbol.visibility == format::SymbolVisibility::Hidden ||
        symbol.visibility == format::SymbolVisibility::Internal)
        return ProviderEligibility::HiddenVisibility;
    if (!mapped) return ProviderEligibility::OutsideProcessMemory;
    if (symbol.type == format::SymbolType::Function && !executable)
        return ProviderEligibility::NonExecutable;
    if (symbol.type == format::SymbolType::Unknown || symbol.type == format::SymbolType::None)
        return ProviderEligibility::UnsupportedType;
    return ProviderEligibility::Eligible;
}

[[nodiscard]] bool is_strong(const ProviderCandidate& candidate) noexcept
{
    return candidate.binding == format::SymbolBinding::Global;
}

[[nodiscard]] Result<void> validate_relocation_target(const memory::GuestMemory& memory,
                                                       const format::Relocation& relocation,
                                                       std::size_t index)
{
    const auto width = loader::relocation_width(relocation.type);
    if ((relocation.target_address % width) != 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::MisalignedRelocationTarget,
            "process relocation[" + std::to_string(index) + "] target is misaligned"));
    }
    const auto end = checked_add_u64(relocation.target_address, width);
    if (!end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "process relocation target range overflows"));
    }
    const auto valid = memory.validate_loader_write(relocation.target_address, width);
    if (!valid)
    {
        return Result<void>::failure(make_error(
            valid.error().code,
            "process relocation[" + std::to_string(index) + "] target is invalid: " +
                valid.error().message));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> add_seeds(ProcessModule& module,
                                      const PreparedModuleOptions& options,
                                      const memory::GuestMemory& memory)
{
    const auto text_entry = checked_add_u64(module.identity.guest_base,
                                             module.image.text.memory_offset);
    if (!text_entry)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "module base plus text offset overflows"));
    }
    if (options.seed_text_entry)
    {
        module.identity.entry_points.push_back(
            EntryPointEvidence{text_entry.value(), EntryPointKind::TextStartCandidate,
                               "prepared NSO .text segment start", FunctionConfidence::Low, false,
                               "text start is an analysis candidate, not a verified process entry"});
        module.seeds.push_back(FunctionSeed{text_entry.value(),
                                            FunctionDiscoverySource::TextStartCandidate,
                                            FunctionConfidence::Low, std::nullopt, std::nullopt,
                                            "prepared NSO text start candidate; runtime entry not verified"});
    }
    if (module.metadata.dynamic)
    {
        const auto add_pointer = [&](const auto& pointer, FunctionDiscoverySource source,
                                     FunctionConfidence confidence, EntryPointKind kind,
                                     std::string provenance, std::string note) {
            if (pointer && pointer->address != 0U)
            {
                module.identity.entry_points.push_back(
                    EntryPointEvidence{pointer->address, kind, std::move(provenance), confidence,
                                       false, note});
                module.seeds.push_back(FunctionSeed{pointer->address, source, confidence,
                                                    std::nullopt, std::nullopt, std::move(note)});
            }
        };
        add_pointer(module.metadata.dynamic->init, FunctionDiscoverySource::AnalystSeed,
                    FunctionConfidence::High, EntryPointKind::DynamicInit, "DT_INIT",
                    "DT_INIT is a module initialization candidate, not a verified process entry");
        add_pointer(module.metadata.dynamic->fini, FunctionDiscoverySource::AnalystSeed,
                    FunctionConfidence::High, EntryPointKind::DynamicFini, "DT_FINI",
                    "DT_FINI is a module finalization candidate, not a verified process entry");
    }
    if (module.symbols)
    {
        for (const auto& symbol : module.symbols->symbols)
        {
            if (!symbol.is_defined() || symbol.type != format::SymbolType::Function)
            {
                continue;
            }
            const auto address = symbol_address(symbol, module.identity.guest_base);
            if (!address) return Result<void>::failure(address.error());
            const auto executable = memory.is_executable(address.value(), 4U);
            if (!executable || !executable.value()) continue;
            module.seeds.push_back(FunctionSeed{
                address.value(), FunctionDiscoverySource::DynamicSymbol,
                FunctionConfidence::Confirmed, std::nullopt,
                symbol.name.empty() ? std::nullopt : std::optional<std::string>(symbol.name),
                "defined dynamic function symbol"});
        }
    }
    module.seeds.insert(module.seeds.end(), options.seeds.begin(), options.seeds.end());
    std::sort(module.identity.entry_points.begin(), module.identity.entry_points.end(),
              [](const auto& left, const auto& right) {
                  if (left.address != right.address) return left.address < right.address;
                  if (left.kind != right.kind) return left.kind < right.kind;
                  if (left.provenance != right.provenance) return left.provenance < right.provenance;
                  return left.note < right.note;
              });
    return Result<void>::success();
}

} // namespace

std::string_view provider_resolution_status_name(ProviderResolutionStatus status) noexcept
{
    switch (status)
    {
    case ProviderResolutionStatus::ResolvedGuestModule: return "resolved_guest_module";
    case ProviderResolutionStatus::AmbiguousGuestProvider: return "ambiguous_guest_provider";
    case ProviderResolutionStatus::NotFoundInSuppliedModules:
        return "provider_not_found_complete";
    case ProviderResolutionStatus::ProviderSearchIncomplete: return "provider_search_incomplete";
    case ProviderResolutionStatus::ProviderIneligible: return "provider_ineligible";
    case ProviderResolutionStatus::InvalidProviderDefinition: return "invalid_provider_definition";
    }
    return "unknown";
}

std::string_view module_set_completeness_name(ModuleSetCompleteness completeness) noexcept
{
    switch (completeness)
    {
    case ModuleSetCompleteness::Incomplete: return "incomplete";
    case ModuleSetCompleteness::DeclaredComplete: return "declared_complete";
    case ModuleSetCompleteness::ManifestVerifiedComplete: return "manifest_verified_complete";
    }
    return "unknown";
}

std::string_view module_set_completeness_basis_name(ModuleSetCompletenessBasis basis) noexcept
{
    switch (basis)
    {
    case ModuleSetCompletenessBasis::LegacyConfigFalse: return "legacy_config_false";
    case ModuleSetCompletenessBasis::ExplicitLocalAssertion: return "explicit_local_assertion";
    case ModuleSetCompletenessBasis::ExplicitInventory: return "explicit_inventory";
    case ModuleSetCompletenessBasis::LocalManifestMatch: return "local_manifest_match";
    case ModuleSetCompletenessBasis::TargetManifestMatch: return "target_manifest_match";
    case ModuleSetCompletenessBasis::DirectoryScanOnly: return "directory_scan_only";
    case ModuleSetCompletenessBasis::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view module_set_coherence_name(ModuleSetCoherence coherence) noexcept
{
    switch (coherence)
    {
    case ModuleSetCoherence::Verified: return "verified";
    case ModuleSetCoherence::PartiallyVerified: return "partially_verified";
    case ModuleSetCoherence::Unverified: return "unverified";
    case ModuleSetCoherence::Conflicting: return "conflicting";
    }
    return "unknown";
}

std::string_view provider_eligibility_name(ProviderEligibility eligibility) noexcept
{
    switch (eligibility)
    {
    case ProviderEligibility::Eligible: return "eligible";
    case ProviderEligibility::NoName: return "no_name";
    case ProviderEligibility::Undefined: return "undefined";
    case ProviderEligibility::LocalBinding: return "local_binding";
    case ProviderEligibility::IneligibleBinding: return "ineligible_binding";
    case ProviderEligibility::HiddenVisibility: return "hidden_visibility";
    case ProviderEligibility::InvalidAddress: return "invalid_address";
    case ProviderEligibility::OutsideProcessMemory: return "outside_process_memory";
    case ProviderEligibility::NonExecutable: return "non_executable";
    case ProviderEligibility::UnsupportedType: return "unsupported_type";
    }
    return "unknown";
}

Result<ProcessSymbolNamespace> ProcessSymbolNamespace::build_impl(
    const memory::GuestMemory& memory, std::span<const ProcessSymbolSource> sources,
    bool strict_invalid_providers)
{
    ProcessSymbolNamespace result;
    try
    {
        for (const auto& source : sources)
        {
            if (source.module.empty() || source.symbols == nullptr)
            {
                return Result<ProcessSymbolNamespace>::failure(make_error(
                    ErrorCode::InvalidArgument, "symbol namespace source is incomplete"));
            }
            for (const auto& symbol : source.symbols->symbols)
            {
                if (symbol.name.empty()) continue;
                std::optional<GuestAddress> address;
                bool mapped = false;
                bool executable = false;
                bool invalid_address = false;
                if (symbol.is_defined())
                {
                    const auto calculated = symbol_address(symbol, source.base);
                    if (!calculated)
                    {
                        invalid_address = true;
                        if (strict_invalid_providers)
                            return Result<ProcessSymbolNamespace>::failure(calculated.error());
                    }
                    else
                    {
                        address = calculated.value();
                        mapped = memory.region_at(calculated.value()).has_value();
                        if (mapped)
                        {
                            const auto executable_result = memory.is_executable(
                                calculated.value(), symbol.type == format::SymbolType::Function ? 4U : 1U);
                            executable = executable_result && executable_result.value();
                        }
                    }
                }
                const auto eligibility = invalid_address
                                             ? ProviderEligibility::InvalidAddress
                                             : provider_eligibility(symbol, mapped, executable);
                const bool eligible = eligibility == ProviderEligibility::Eligible;
                result.occurrences_.push_back(ProviderOccurrence{
                    source.module, symbol.index, symbol.name, symbol.is_defined(), symbol.binding,
                    symbol.type, symbol.visibility, symbol.section_index, symbol.value, address,
                    executable, eligible, eligibility});
                if (!eligible)
                {
                    if (strict_invalid_providers &&
                        (eligibility == ProviderEligibility::NonExecutable ||
                         eligibility == ProviderEligibility::OutsideProcessMemory))
                    {
                        return Result<ProcessSymbolNamespace>::failure(make_error(
                            ErrorCode::InvalidProviderDefinition,
                            "defined function provider is not mapped executable guest code"));
                    }
                    continue;
                }
                result.candidates_.push_back(ProviderCandidate{
                    source.module, symbol.index, symbol.name, symbol.binding, symbol.type,
                    symbol.visibility, symbol.section_index, symbol.value, address.value(), executable});
            }
        }
        std::sort(result.candidates_.begin(), result.candidates_.end(),
                  [](const auto& left, const auto& right) {
                      if (left.symbol != right.symbol) return left.symbol < right.symbol;
                      if (left.module != right.module) return left.module < right.module;
                      return left.symbol_index < right.symbol_index;
                  });
        std::sort(result.occurrences_.begin(), result.occurrences_.end(),
                  [](const auto& left, const auto& right) {
                      if (left.symbol != right.symbol) return left.symbol < right.symbol;
                      if (left.module != right.module) return left.module < right.module;
                      return left.symbol_index < right.symbol_index;
                  });
        return Result<ProcessSymbolNamespace>::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Result<ProcessSymbolNamespace>::failure(
            make_error(ErrorCode::ResourceLimit, "symbol namespace allocation failed"));
    }
}

Result<ProcessSymbolNamespace> ProcessSymbolNamespace::build(
    const memory::GuestMemory& memory, std::span<const ProcessSymbolSource> sources)
{
    return build_impl(memory, sources, true);
}

Result<ProcessSymbolNamespace> ProcessSymbolNamespace::build_audited(
    const memory::GuestMemory& memory, std::span<const ProcessSymbolSource> sources)
{
    return build_impl(memory, sources, false);
}

ProviderLookup ProcessSymbolNamespace::lookup(std::string_view name, bool search_complete) const
{
    return lookup(name, search_complete ? ModuleSetCompleteness::DeclaredComplete
                                         : ModuleSetCompleteness::Incomplete,
                  search_complete ? ModuleSetCompletenessBasis::ExplicitLocalAssertion
                                   : ModuleSetCompletenessBasis::LegacyConfigFalse);
}

ProviderLookup ProcessSymbolNamespace::lookup(
    std::string_view name, ModuleSetCompleteness completeness,
    ModuleSetCompletenessBasis basis) const
{
    ProviderLookup result;
    result.completeness = completeness;
    result.completeness_basis = basis;
    for (const auto& occurrence : occurrences_)
    {
        if (occurrence.symbol == name) result.occurrences.push_back(occurrence);
    }
    for (const auto& candidate : candidates_)
    {
        if (candidate.symbol == name) result.candidates.push_back(candidate);
    }
    std::vector<std::size_t> strong;
    for (std::size_t index = 0U; index < result.candidates.size(); ++index)
    {
        if (is_strong(result.candidates[index])) strong.push_back(index);
    }
    if (strong.size() == 1U)
    {
        result.status = ProviderResolutionStatus::ResolvedGuestModule;
        result.selected_candidate = strong.front();
    }
    else if (strong.size() > 1U || result.candidates.size() > 1U)
    {
        result.status = ProviderResolutionStatus::AmbiguousGuestProvider;
    }
    else if (result.candidates.size() == 1U)
    {
        result.status = ProviderResolutionStatus::ResolvedGuestModule;
        result.selected_candidate = 0U;
    }
    else if (std::any_of(result.occurrences.begin(), result.occurrences.end(), [](const auto& occurrence) {
                 return occurrence.eligibility == ProviderEligibility::NonExecutable ||
                        occurrence.eligibility == ProviderEligibility::OutsideProcessMemory ||
                        occurrence.eligibility == ProviderEligibility::InvalidAddress;
             }))
    {
        result.status = ProviderResolutionStatus::ProviderIneligible;
    }
    else
    {
        result.status = completeness == ModuleSetCompleteness::Incomplete
                            ? ProviderResolutionStatus::ProviderSearchIncomplete
                            : ProviderResolutionStatus::NotFoundInSuppliedModules;
    }
    return result;
}

const ProcessModule* ProcessImage::module(std::string_view name) const noexcept
{
    const auto found = std::lower_bound(
        modules_.begin(), modules_.end(), name,
        [](const ProcessModule& module, std::string_view value) { return module.identity.module < value; });
    return found != modules_.end() && found->identity.module == name ? &*found : nullptr;
}

const ProcessModule* ProcessImage::module_for_address(GuestAddress address,
                                                       memory::GuestSize size) const noexcept
{
    const auto end = checked_add_u64(address, size);
    if (!end) return nullptr;
    const ProcessModule* owner = nullptr;
    for (const auto& module : modules_)
    {
        for (const auto& range : module.mappings)
        {
            const auto range_end = checked_add_u64(range.base, range.size);
            if (range_end && address >= range.base && end.value() <= range_end.value())
            {
                if (owner != nullptr) return nullptr;
                owner = &module;
                break;
            }
        }
    }
    return owner;
}

std::vector<loader::UnresolvedRelocation> ProcessImage::unresolved_relocations() const
{
    std::vector<loader::UnresolvedRelocation> result;
    for (const auto& module : modules_)
    {
        result.insert(result.end(), module.unresolved_relocations.begin(),
                      module.unresolved_relocations.end());
    }
    return result;
}

ProviderLookup ProcessImage::lookup_provider(std::string_view name) const
{
    return symbol_namespace_.lookup(name, completeness_, completeness_basis_);
}

ProcessImageSummary ProcessImage::summary() const
{
    ProcessImageSummary result;
    result.primary_module = primary_module_;
    result.provider_search_complete = provider_search_complete_;
    result.completeness = completeness_;
    result.completeness_basis = completeness_basis_;
    result.coherence = coherence_;
    result.coherence_basis = coherence_basis_;
    result.source = source_;
    result.module_count = modules_.size();
    result.relocations_planned = relocations_planned_;
    result.transactional_relocation_success = executable_state_valid_;
    result.ignored_module_entries = ignored_module_entries_;
    result.focus_provider = lookup_provider("__nnmusl_init_dso");
    for (const auto& module : modules_)
    {
        ProcessModuleSummary summary;
        summary.logical_name = module.identity.module;
        summary.name_provenance = module.name_provenance;
        summary.input_size = module.input_size;
        summary.sha256 = module.identity.input_sha256;
        summary.build_id = module.identity.build_id;
        summary.nso_version = module.image.header.version;
        summary.nso_flags = module.image.header.flags;
        summary.base = module.identity.guest_base;
        summary.base_provenance = module.base_provenance;
        summary.runtime_base_verified = module.identity.guest_base_verified;
        for (const auto& segment : {module.image.header.text, module.image.header.rodata,
                                    module.image.header.data})
        {
            summary.segments.push_back(ProcessModuleSummary::Segment{
                std::string(format::nso_segment_kind_name(segment.kind)), segment.file_offset,
                segment.memory_offset, segment.memory_size, segment.stored_size,
                segment.compressed, segment.hash_required});
        }
        summary.bss_size = module.image.bss.size();
        for (const auto& mapping : module.mappings)
        {
            summary.mapped_ranges.push_back(GuestAddressRange{mapping.base, mapping.size});
            summary.mappings.push_back(ProcessModuleSummary::Mapping{
                mapping.base, mapping.size, mapping.permissions, mapping.kind});
        }
        if (module.symbols)
        {
            summary.dynamic_symbol_count = module.symbols->symbols.size();
            for (const auto& symbol : module.symbols->symbols)
            {
                if (symbol.is_defined()) ++summary.defined_symbol_count;
                else ++summary.undefined_symbol_count;
            }
        }
        summary.relocation_count = module.relocations.size();
        summary.applied_relocations = module.applied_relocations;
        summary.unresolved_relocations = module.unresolved_relocations.size();
        summary.mod0_available = module.metadata.mod0.has_value();
        summary.dynamic_available = module.metadata.dynamic.has_value();
        summary.mod0_status = summary.mod0_available ? "available" : "absent";
        summary.dynamic_status = summary.dynamic_available ? "available" : "absent";
        summary.provider_index_eligible = module.symbols.has_value();
        if (module.metadata.dynamic)
        {
            for (const auto& entry : module.metadata.dynamic->entries)
            {
                summary.dynamic_tags.emplace_back(format::dynamic_tag_name(
                    static_cast<format::DynamicTag>(entry.tag)));
            }
            std::sort(summary.dynamic_tags.begin(), summary.dynamic_tags.end());
            summary.dynamic_tags.erase(
                std::unique(summary.dynamic_tags.begin(), summary.dynamic_tags.end()),
                summary.dynamic_tags.end());
        }
        if (!module.identity.executable_ranges.empty()) ++result.executable_module_count;
        result.modules.push_back(std::move(summary));
    }
    result.bindings = bindings_;
    return result;
}

Result<ProcessImage> load_process_image(std::span<const ProcessModuleInput> inputs,
                                        const ProcessImageOptions& options)
{
    if (inputs.empty())
    {
        return Result<ProcessImage>::failure(
            make_error(ErrorCode::ModuleSetEmpty, "process image requires at least one module"));
    }
    if (options.primary_module.empty())
    {
        return Result<ProcessImage>::failure(
            make_error(ErrorCode::InvalidArgument, "process image primary module is empty"));
    }
    if (options.module_alignment == 0U ||
        (options.module_alignment & (options.module_alignment - 1U)) != 0U)
    {
        return Result<ProcessImage>::failure(make_error(
            ErrorCode::InvalidArgument, "module alignment must be a non-zero power of two"));
    }
    if (options.module_set_coherence == ModuleSetCoherence::Conflicting)
    {
        return Result<ProcessImage>::failure(make_error(
            ErrorCode::ModuleSetIdentityConflict,
            "process image cannot be constructed from a conflicting executable module set"));
    }
    if (!stable_report_label(options.module_set_source) ||
        !stable_report_label(options.module_set_coherence_basis) ||
        !std::all_of(options.ignored_module_entries.begin(), options.ignored_module_entries.end(),
                     [](const auto& entry) { return stable_report_label(entry); }))
    {
        return Result<ProcessImage>::failure(make_error(
            ErrorCode::InvalidArgument,
            "process module-set provenance contains an unsafe report label"));
    }

    try
    {
        std::vector<ProcessModuleInput> ordered(inputs.begin(), inputs.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return left.logical_name < right.logical_name;
        });
        std::set<std::string> names;
        for (const auto& input : ordered)
        {
            if (!stable_report_label(input.logical_name) ||
                !stable_report_label(input.name_provenance) ||
                !names.insert(input.logical_name).second)
            {
                return Result<ProcessImage>::failure(make_error(
                    ErrorCode::DuplicateModuleIdentity,
                    "process image contains an unsafe, empty, or duplicate logical module identity"));
            }
            if (input.file_bytes.empty())
            {
                return Result<ProcessImage>::failure(make_error(
                    ErrorCode::InvalidArgument, "process image module has no file bytes"));
            }
        }
        if (!names.contains(options.primary_module))
        {
            return Result<ProcessImage>::failure(make_error(
                ErrorCode::InvalidArgument, "primary module is not present in the process image"));
        }

        std::vector<StagedModule> staged;
        staged.reserve(ordered.size());
        for (const auto& input : ordered)
        {
            const auto digest = sha256_bytes(input.file_bytes);
            if (!digest) return Result<ProcessImage>::failure(digest.error());
            const auto header = format::parse_nso_header(input.file_bytes);
            if (!header) return Result<ProcessImage>::failure(header.error());
            const auto image = format::materialize_nso(input.file_bytes, header.value(),
                                                        options.module_options.materialization_limits);
            if (!image) return Result<ProcessImage>::failure(image.error());
            staged.push_back(StagedModule{input.logical_name, input.name_provenance,
                                          static_cast<std::uint64_t>(input.file_bytes.size()),
                                          header.value(), std::move(image).value(),
                                          digest.value(), input.explicit_base.value_or(0U),
                                          input.explicit_base ? ModuleBaseProvenance::ExplicitAnalysisBase
                                                              : ModuleBaseProvenance::DeterministicAnalysisLayout});
        }

        struct AssignedRange
        {
            GuestAddress base;
            GuestAddress end;
        };
        std::vector<AssignedRange> assigned;
        std::vector<AssignedRange> explicit_ranges;
        for (const auto& module : staged)
        {
            if (module.base_provenance != ModuleBaseProvenance::ExplicitAnalysisBase) continue;
            if ((module.base % options.module_alignment) != 0U)
            {
                return Result<ProcessImage>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    "explicit module base is not aligned to the process layout alignment"));
            }
            const auto span = image_span(module.image);
            if (!span) return Result<ProcessImage>::failure(span.error());
            const auto end = checked_add_u64(module.base, span.value());
            if (!end) return Result<ProcessImage>::failure(make_error(
                ErrorCode::ModuleLayoutOverflow, "explicit module base range overflows"));
            explicit_ranges.push_back(AssignedRange{module.base, end.value()});
        }

        GuestAddress cursor = options.automatic_base;
        for (auto& module : staged)
        {
            const auto span = image_span(module.image);
            if (!span) return Result<ProcessImage>::failure(span.error());
            if (module.base_provenance == ModuleBaseProvenance::DeterministicAnalysisLayout)
            {
                const auto aligned = align_up(cursor, options.module_alignment);
                if (!aligned) return Result<ProcessImage>::failure(aligned.error());
                module.base = aligned.value();
                for (;;)
                {
                    const auto end = checked_add_u64(module.base, span.value());
                    if (!end) return Result<ProcessImage>::failure(make_error(
                        ErrorCode::ModuleLayoutOverflow, "automatic module layout overflows"));
                    bool collision = false;
                    for (const auto& range : explicit_ranges)
                    {
                        if (ranges_overlap(module.base, end.value(), range.base, range.end))
                        {
                            collision = true;
                            const auto next = checked_add_u64(range.end, options.module_gap);
                            if (!next) return Result<ProcessImage>::failure(make_error(
                                ErrorCode::ModuleLayoutOverflow, "module gap overflows"));
                            module.base = next.value();
                            break;
                        }
                    }
                    if (!collision) break;
                }
            }
            const auto end = checked_add_u64(module.base, span.value());
            if (!end) return Result<ProcessImage>::failure(make_error(
                ErrorCode::ModuleLayoutOverflow, "module layout range overflows"));
            for (const auto& range : assigned)
            {
                if (ranges_overlap(module.base, end.value(), range.base, range.end))
                {
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::ModuleAddressOverlap,
                        "module address ranges overlap in the process analysis layout"));
                }
            }
            assigned.push_back(AssignedRange{module.base, end.value()});
            const auto next = checked_add_u64(end.value(), options.module_gap);
            if (!next) return Result<ProcessImage>::failure(make_error(
                ErrorCode::ModuleLayoutOverflow, "automatic module layout cursor overflows"));
            cursor = next.value();
            if (options.reserved_stack_base && options.reserved_stack_size != 0U)
            {
                const auto stack_end = checked_add_u64(options.reserved_stack_base.value(),
                                                       options.reserved_stack_size);
                if (!stack_end) return Result<ProcessImage>::failure(make_error(
                    ErrorCode::ModuleLayoutOverflow, "reserved stack range overflows"));
                if (ranges_overlap(module.base, end.value(), options.reserved_stack_base.value(),
                                   stack_end.value()))
                {
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::ModuleAddressOverlap,
                        "module address range collides with the reserved stack range"));
                }
            }
        }

        memory::GuestMemory staged_memory(options.module_options.memory_limits);
        for (const auto& module : staged)
        {
            const auto loaded = loader::load_nso(module.image, staged_memory,
                                                 loader::NsoGuestLoadOptions{module.base});
            if (!loaded) return Result<ProcessImage>::failure(loaded.error());
        }

        ProcessImage result;
        result.memory_ = std::move(staged_memory);
        result.primary_module_ = options.primary_module;
        result.completeness_ = options.module_set_completeness;
        result.completeness_basis_ = options.module_set_completeness_basis;
        if (options.provider_search_complete &&
            result.completeness_ == ModuleSetCompleteness::Incomplete)
        {
            // Preserve the M13 boolean while making its weaker provenance
            // explicit in every M14 report.
            result.completeness_ = ModuleSetCompleteness::DeclaredComplete;
            result.completeness_basis_ = ModuleSetCompletenessBasis::ExplicitLocalAssertion;
        }
        result.provider_search_complete_ = result.completeness_ != ModuleSetCompleteness::Incomplete;
        result.coherence_ = options.module_set_coherence;
        result.coherence_basis_ = options.module_set_coherence_basis;
        result.source_ = options.module_set_source;
        result.ignored_module_entries_ = options.ignored_module_entries;
        result.relocations_planned_ = options.plan_relocations;
        result.executable_state_valid_ = options.plan_relocations && options.apply_relocations;
        result.modules_.reserve(staged.size());

        for (const auto& source : staged)
        {
            PreparedModuleOptions module_options = options.module_options;
            module_options.module_name = source.name;
            module_options.module_base = source.base;
            module_options.apply_relocations = false;
            const auto metadata = format::parse_module_metadata(
                result.memory_, source.base, module_options.metadata_options);
            if (!metadata) return Result<ProcessImage>::failure(metadata.error());

            std::optional<format::DynamicSymbolTable> symbols;
            std::vector<format::Relocation> relocations;
            if (metadata.value().dynamic)
            {
                if (metadata.value().dynamic->symtab)
                {
                    const auto parsed = format::DynamicSymbolTable::parse(
                        result.memory_, metadata.value().dynamic.value(),
                        module_options.metadata_options.dynamic);
                    if (!parsed) return Result<ProcessImage>::failure(parsed.error());
                    symbols = std::move(parsed).value();
                }
                const auto rela_entries = format::parse_rela_table(
                    result.memory_, metadata.value().dynamic.value(), module_options.metadata_options.dynamic);
                if (!rela_entries) return Result<ProcessImage>::failure(rela_entries.error());
                const auto rela = format::make_relocations(rela_entries.value(),
                                                           format::RelocationSource::Rela);
                if (!rela) return Result<ProcessImage>::failure(rela.error());
                relocations = std::move(rela).value();
                const auto jmprel_entries = format::parse_jmprel_table(
                    result.memory_, metadata.value().dynamic.value(), module_options.metadata_options.dynamic);
                if (!jmprel_entries) return Result<ProcessImage>::failure(jmprel_entries.error());
                const auto jmprel = format::make_relocations(jmprel_entries.value(),
                                                             format::RelocationSource::JmpRel);
                if (!jmprel) return Result<ProcessImage>::failure(jmprel.error());
                relocations.insert(relocations.end(), jmprel.value().begin(), jmprel.value().end());
                if (!relocations.empty() && !symbols)
                {
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::MissingImportBinding,
                        "dynamic relocations are present without a dynamic symbol table"));
                }
            }

            ModuleIdentity identity;
            identity.module = source.name;
            identity.build_id = format::module_id_hex(source.header);
            identity.input_sha256 = sha256_to_hex(source.digest);
            identity.guest_base = source.base;
            identity.guest_base_verified = false;
            identity.guest_base_provenance = source.base_provenance;
            identity.executable_ranges.clear();
            identity.translator_version = switchrecomp::version;
            identity.metadata_schema_version = 3U;
            identity.llvm_version = "disabled";
            identity.feature_flags = {"process_image", "guest_symbol_namespace",
                                      "transactional_cross_module_relocations"};
            ProcessModule module{std::move(identity), source.name_provenance, source.input_size,
                                 source.base_provenance, source.image,
                                 metadata.value(), std::move(symbols), std::move(relocations),
                                 0U, {}, {}, {}, {}};
            for (const auto& region : result.memory_.regions())
            {
                const auto module_end = checked_add_u64(module.identity.guest_base,
                                                        image_span(module.image).value());
                if (module_end && region.base >= module.identity.guest_base &&
                    region.end() <= module_end.value())
                {
                    module.mappings.push_back(region);
                    if (memory::has_permission(region.permissions,
                                               memory::GuestMemoryPermissions::Execute))
                    {
                        module.identity.executable_ranges.push_back(
                            GuestAddressRange{region.base, region.size});
                    }
                }
            }
            const auto seeded = add_seeds(module, module_options, result.memory_);
            if (!seeded) return Result<ProcessImage>::failure(seeded.error());
            if (module.symbols) module.unresolved_imports = module.symbols->imports();
            result.modules_.push_back(std::move(module));
        }

        std::vector<ProcessSymbolSource> symbol_sources;
        symbol_sources.reserve(result.modules_.size());
        for (const auto& module : result.modules_)
        {
            if (module.symbols)
            {
                symbol_sources.push_back(ProcessSymbolSource{
                    module.identity.module, module.identity.guest_base, &module.symbols.value()});
            }
        }
        const auto symbol_namespace = ProcessSymbolNamespace::build_audited(
            result.memory_, std::span<const ProcessSymbolSource>(symbol_sources));
        if (!symbol_namespace) return Result<ProcessImage>::failure(symbol_namespace.error());
        result.symbol_namespace_ = symbol_namespace.value();

        std::vector<loader::AppliedRelocation> pending;
        std::vector<std::pair<std::size_t, std::size_t>> pending_owner;
        if (options.plan_relocations)
        for (std::size_t module_index = 0U; module_index < result.modules_.size(); ++module_index)
        {
            auto& module = result.modules_[module_index];
            for (std::size_t relocation_index = 0U; relocation_index < module.relocations.size();
                 ++relocation_index)
            {
                const auto& relocation = module.relocations[relocation_index];
                if (relocation.type == format::AArch64RelocationType::None) continue;
                if (relocation.type == format::AArch64RelocationType::Unknown)
                {
                    std::string symbol_name;
                    if (module.symbols)
                    {
                        if (const auto* symbol = module.symbols->at(relocation.symbol_index))
                            symbol_name = symbol->name;
                    }
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::UnsupportedRelocationType,
                        "unsupported relocation in module '" + module.identity.module + "' at index " +
                            std::to_string(relocation_index) + ": type " +
                            std::string(format::aarch64_relocation_type_name(relocation.type)) +
                            " (raw " + std::to_string(relocation.raw_type) + "), offset 0x" +
                            [&]() {
                                constexpr char digits[] = "0123456789abcdef";
                                std::string hex;
                                auto value = relocation.offset;
                                do
                                {
                                    hex.push_back(digits[value & 0xfU]);
                                    value >>= 4U;
                                } while (value != 0U);
                                std::reverse(hex.begin(), hex.end());
                                return hex;
                            }() +
                            (symbol_name.empty() ? std::string{} : ", symbol '" + symbol_name + "'")));
                }
                const auto target = validate_relocation_target(result.memory_, relocation,
                                                               relocation_index);
                if (!target) return Result<ProcessImage>::failure(target.error());
                std::uint64_t value = 0U;
                if (relocation.type == format::AArch64RelocationType::Relative)
                {
                    const auto calculated = checked_add_signed_u64(module.identity.guest_base,
                                                                     relocation.addend);
                    if (!calculated) return Result<ProcessImage>::failure(calculated.error());
                    value = calculated.value();
                }
                else
                {
                    const auto* symbol = module.symbols ? module.symbols->at(relocation.symbol_index)
                                                        : nullptr;
                    if (symbol == nullptr)
                    {
                        return Result<ProcessImage>::failure(make_error(
                            ErrorCode::InvalidSymbolIndex, "process relocation symbol index is invalid"));
                    }
                    ProviderLookup lookup;
                    if (symbol->is_defined())
                    {
                        const auto address = symbol_address(*symbol, module.identity.guest_base);
                        if (!address) return Result<ProcessImage>::failure(address.error());
                        const auto calculated = checked_add_signed_u64(address.value(), relocation.addend);
                        if (!calculated) return Result<ProcessImage>::failure(calculated.error());
                        value = calculated.value();
                    }
                    else
                    {
                        lookup = result.symbol_namespace_.lookup(
                            symbol->name, result.completeness_, result.completeness_basis_);
                        ProcessBinding binding;
                        binding.consumer_module = module.identity.module;
                        binding.consumer_symbol_index = relocation.symbol_index;
                        binding.symbol = symbol->name;
                        binding.relocation_index = relocation_index;
                        binding.relocation = relocation;
                        binding.provider = lookup;
                        binding.resolution_basis = "process_guest_symbol_namespace";
                        binding.confidence = lookup.status == ProviderResolutionStatus::ResolvedGuestModule
                                                 ? "unambiguous" : "not_selected";
                        if (lookup.selected_candidate)
                        {
                            const auto& provider = lookup.candidates[lookup.selected_candidate.value()];
                            if (relocation.type == format::AArch64RelocationType::JumpSlot &&
                                (provider.type != format::SymbolType::Function || !provider.executable))
                            {
                                return Result<ProcessImage>::failure(make_error(
                                    ErrorCode::InvalidProviderDefinition,
                                    "JUMP_SLOT provider is not an executable function"));
                            }
                            binding.provider_module = provider.module;
                            binding.provider_symbol_index = provider.symbol_index;
                            binding.provider_address = provider.address;
                            const auto calculated = checked_add_signed_u64(provider.address,
                                                                             relocation.addend);
                            if (!calculated) return Result<ProcessImage>::failure(calculated.error());
                            value = calculated.value();
                            binding.applied = true;
                        }
                        else
                        {
                            module.unresolved_relocations.push_back(
                                loader::UnresolvedRelocation{relocation_index, relocation,
                                                             format::ImportSymbol{symbol->index,
                                                                                  symbol->name,
                                                                                  symbol->binding,
                                                                                  symbol->type,
                                                                                  symbol->visibility,
                                                                                  symbol->section_index}});
                        }
                        result.bindings_.push_back(std::move(binding));
                        if (!lookup.selected_candidate) continue;
                    }
                }
                if (relocation.type == format::AArch64RelocationType::Abs32 &&
                    value > std::numeric_limits<std::uint32_t>::max())
                {
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::ArithmeticOverflow, "process ABS32 relocation value exceeds 32 bits"));
                }
                pending.push_back(loader::AppliedRelocation{relocation_index, relocation, value,
                                                            loader::relocation_width(relocation.type)});
                pending_owner.push_back({module_index, relocation_index});
            }
        }

        if (options.apply_relocations)
        {
            loader::RelocationPlan plan;
            plan.relocation_count = pending.size();
            plan.applied = pending;
            const auto applied = loader::apply_relocation_plan(result.memory_, plan,
                                                               options.module_options.relocation_options);
            if (!applied) return Result<ProcessImage>::failure(applied.error());
            for (const auto& owner : pending_owner) ++result.modules_[owner.first].applied_relocations;
        }
        for (auto& binding : result.bindings_)
        {
            if (binding.applied && !options.apply_relocations) binding.applied = false;
        }
        return Result<ProcessImage>::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Result<ProcessImage>::failure(
            make_error(ErrorCode::ResourceLimit, "process image allocation failed"));
    }
}

Result<ProcessFunctionMap> ProcessFunctionMap::build(std::vector<FinalizedFunctionMap> maps)
{
    ProcessFunctionMap result;
    std::sort(maps.begin(), maps.end(), [](const auto& left, const auto& right) {
        return left.identity().module < right.identity().module;
    });
    for (std::size_t index = 1U; index < maps.size(); ++index)
    {
        if (maps[index - 1U].identity().module == maps[index].identity().module)
        {
            return Result<ProcessFunctionMap>::failure(make_error(
                ErrorCode::DuplicateModuleIdentity,
                "process function maps contain a duplicate logical module identity"));
        }
    }
    result.maps_ = std::move(maps);
    for (std::size_t map_index = 0U; map_index < result.maps_.size(); ++map_index)
    {
        if (!result.maps_[map_index].frozen())
        {
            return Result<ProcessFunctionMap>::failure(make_error(
                ErrorCode::InvalidArgument, "process function map requires frozen module maps"));
        }
        for (const auto& function : result.maps_[map_index].functions())
        {
            const auto inserted = result.entries_.emplace(function.canonical_entry, map_index);
            if (!inserted.second)
            {
                return Result<ProcessFunctionMap>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "two process modules claim the same canonical function entry"));
            }
        }
    }
    return Result<ProcessFunctionMap>::success(std::move(result));
}

const FunctionRecord* ProcessFunctionMap::find(GuestAddress entry) const noexcept
{
    const auto found = entries_.find(entry);
    if (found == entries_.end()) return nullptr;
    const auto& functions = maps_[found->second].functions();
    const auto item = std::lower_bound(functions.begin(), functions.end(), entry,
                                       [](const auto& function, GuestAddress value) {
                                           return function.canonical_entry < value;
                                       });
    return item != functions.end() && item->canonical_entry == entry ? &*item : nullptr;
}

const FinalizedFunctionMap* ProcessFunctionMap::map_for(GuestAddress entry) const noexcept
{
    const auto found = entries_.find(entry);
    return found == entries_.end() ? nullptr : &maps_[found->second];
}

} // namespace switchrecomp::analysis
