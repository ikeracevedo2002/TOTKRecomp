#include "switchrecomp/analysis/process_image.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/common/sha256.hpp"
#include "switchrecomp/format/mod0.hpp"
#include "switchrecomp/loader/nso_guest_loader.hpp"
#include "switchrecomp/version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace switchrecomp::analysis
{

namespace
{

using GuestAddress = memory::GuestAddress;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
}

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

[[nodiscard]] Result<void> validate_relocation_target(
    const memory::GuestMemory& memory, std::span<const memory::GuestMemoryRegionInfo> regions,
    const format::Relocation& relocation, std::size_t index)
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

    // The process image is immutable while relocation planning runs. Avoid a
    // lower_bound over GuestMemory's mutable region object and its Result/
    // diagnostic construction for every relocation; retain the original
    // validator for the uncommon invalid-target diagnostic path.
    auto next = std::lower_bound(
        regions.begin(), regions.end(), relocation.target_address,
        [](const memory::GuestMemoryRegionInfo& region, GuestAddress value) {
            return region.base < value;
        });
    if (next != regions.begin() && (next == regions.end() || next->base > relocation.target_address))
        --next;
    const bool contained = next != regions.end() && next->base <= relocation.target_address &&
                           end.value() <= next->end();
    if (contained) return Result<void>::success();

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
        for (const auto& relocation : module.relocations)
        {
            const auto* symbol = module.symbols->at(relocation.symbol_index);
            if (symbol == nullptr || !symbol->is_defined() ||
                symbol->type != format::SymbolType::Function)
            {
                continue;
            }
            const auto address = symbol_address(*symbol, module.identity.guest_base);
            if (!address) return Result<void>::failure(address.error());
            const auto executable = memory.is_executable(address.value(), 4U);
            if (!executable || !executable.value()) continue;
            module.seeds.push_back(FunctionSeed{
                address.value(), FunctionDiscoverySource::RelocationReference,
                FunctionConfidence::High, std::nullopt,
                symbol->name.empty() ? std::nullopt
                                     : std::optional<std::string>(symbol->name),
                "relocation-backed executable function pointer at " +
                    hex_address(relocation.target_address) + ", " +
                    std::string(format::aarch64_relocation_type_name(relocation.type))});
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
        // Symbol eligibility queries are read-only while the process image is
        // being built. Snapshot the tiny mapping metadata table once instead
        // of constructing several Result objects and searching GuestMemory
        // for every defined dynamic symbol.
        const auto memory_regions = memory.regions();
        const auto mapped_region = [&](GuestAddress address) {
            auto next = std::lower_bound(
                memory_regions.begin(), memory_regions.end(), address,
                [](const memory::GuestMemoryRegionInfo& region, GuestAddress value) {
                    return region.base < value;
                });
            if (next != memory_regions.begin() &&
                (next == memory_regions.end() || next->base > address))
                --next;
            return next != memory_regions.end() && next->base <= address &&
                   address < next->end() ? &*next : nullptr;
        };
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
                        const auto* region = mapped_region(calculated.value());
                        mapped = region != nullptr;
                        if (mapped && symbol.type == format::SymbolType::Function)
                        {
                            const auto function_end = checked_add_u64(calculated.value(), 4U);
                            executable = function_end && function_end.value() <= region->end() &&
                                         memory::has_permission(
                                             region->permissions, memory::GuestMemoryPermissions::Execute);
                        }
                        else if (mapped)
                        {
                            executable = memory::has_permission(
                                region->permissions, memory::GuestMemoryPermissions::Execute);
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
        const auto build_index = [](const auto& entries, auto& index) {
            std::size_t begin = 0U;
            while (begin < entries.size())
            {
                std::size_t end = begin + 1U;
                while (end < entries.size() && entries[end].symbol == entries[begin].symbol)
                    ++end;
                index.emplace(entries[begin].symbol, ProcessSymbolNamespace::SymbolRange{begin, end});
                begin = end;
            }
        };
        build_index(result.candidates_, result.candidate_index_);
        build_index(result.occurrences_, result.occurrence_index_);
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
    if (const auto found = occurrence_index_.find(name); found != occurrence_index_.end())
    {
        const auto [begin, end] = found->second;
        result.occurrences.insert(
            result.occurrences.end(),
            occurrences_.begin() + static_cast<std::vector<ProviderOccurrence>::difference_type>(begin),
            occurrences_.begin() + static_cast<std::vector<ProviderOccurrence>::difference_type>(end));
    }
    if (const auto found = candidate_index_.find(name); found != candidate_index_.end())
    {
        const auto [begin, end] = found->second;
        result.candidates.insert(
            result.candidates.end(),
            candidates_.begin() + static_cast<std::vector<ProviderCandidate>::difference_type>(begin),
            candidates_.begin() + static_cast<std::vector<ProviderCandidate>::difference_type>(end));
    }
    // A candidate in a directory scan is evidence, not a resolved provider.
    // Do not let a partial namespace accidentally become executable state.
    if (completeness == ModuleSetCompleteness::Incomplete)
    {
        result.status = ProviderResolutionStatus::ProviderSearchIncomplete;
        return result;
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

const std::vector<ProcessFunctionTargetReference>& ProcessImage::function_target_references(
    GuestAddress target) const noexcept
{
    static const std::vector<ProcessFunctionTargetReference> empty;
    const auto found = function_target_references_.find(target);
    return found == function_target_references_.end() ? empty : found->second;
}

const std::vector<ProcessRelocationReference>& ProcessImage::relocation_references(
    GuestAddress source_slot) const noexcept
{
    static const std::vector<ProcessRelocationReference> empty;
    const auto found = relocation_references_.find(source_slot);
    return found == relocation_references_.end() ? empty : found->second;
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
    result.module_load_order = module_order_.module_names;
    result.module_load_order_basis = module_order_.basis;
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

        // NSO digesting and materialization are independent per module and
        // dominate cold run-entry startup. Compute them in stable input slots;
        // all later layout, mapping, binding, and relocation publication stays
        // ordered, so worker count cannot affect semantic output.
        std::vector<std::optional<Result<StagedModule>>> staged_results(ordered.size());
        const auto stage_module = [&](std::size_t index) {
            try
            {
                const auto& input = ordered[index];
                const auto digest = sha256_bytes(input.file_bytes);
                if (!digest)
                {
                    staged_results[index] = Result<StagedModule>::failure(digest.error());
                    return;
                }
                const auto header = format::parse_nso_header(input.file_bytes);
                if (!header)
                {
                    staged_results[index] = Result<StagedModule>::failure(header.error());
                    return;
                }
                const auto image = format::materialize_nso(
                    input.file_bytes, header.value(), options.module_options.materialization_limits);
                if (!image)
                {
                    staged_results[index] = Result<StagedModule>::failure(image.error());
                    return;
                }
                staged_results[index] = Result<StagedModule>::success(StagedModule{
                    input.logical_name, input.name_provenance,
                    static_cast<std::uint64_t>(input.file_bytes.size()), header.value(),
                    std::move(image).value(), digest.value(), input.explicit_base.value_or(0U),
                    input.explicit_base ? ModuleBaseProvenance::ExplicitAnalysisBase
                                        : ModuleBaseProvenance::DeterministicAnalysisLayout});
            }
            catch (const std::bad_alloc&)
            {
                staged_results[index] = Result<StagedModule>::failure(
                    make_error(ErrorCode::ResourceLimit, "parallel NSO staging allocation failed"));
            }
            catch (...)
            {
                staged_results[index] = Result<StagedModule>::failure(
                    make_error(ErrorCode::ThreadCreationFailed, "parallel NSO staging failed unexpectedly"));
            }
        };
        const auto worker_count = std::min<std::size_t>(
            10U, std::min(std::max<std::size_t>(options.module_workers, 1U), ordered.size()));
        if (worker_count <= 1U || ordered.size() <= 1U)
        {
            for (std::size_t index = 0U; index < ordered.size(); ++index) stage_module(index);
        }
        else
        {
            std::atomic<std::size_t> next{0U};
            std::vector<std::thread> workers;
            workers.reserve(worker_count);
            try
            {
                for (std::size_t worker = 0U; worker < worker_count; ++worker)
                {
                    workers.emplace_back([&]() {
                        for (;;)
                        {
                            const auto index = next.fetch_add(1U, std::memory_order_relaxed);
                            if (index >= ordered.size()) return;
                            stage_module(index);
                        }
                    });
                }
            }
            catch (...)
            {
                for (auto& worker : workers)
                    if (worker.joinable()) worker.join();
                return Result<ProcessImage>::failure(make_error(
                    ErrorCode::ThreadCreationFailed, "unable to create the bounded NSO staging worker pool"));
            }
            for (auto& worker : workers)
                if (worker.joinable()) worker.join();
        }

        std::vector<StagedModule> staged;
        staged.reserve(ordered.size());
        for (auto& staged_result : staged_results)
        {
            if (!staged_result)
            {
                return Result<ProcessImage>::failure(make_error(
                    ErrorCode::ThreadCreationFailed, "NSO staging worker did not publish a result"));
            }
            if (!staged_result->has_value())
                return Result<ProcessImage>::failure(staged_result->error());
            staged.push_back(std::move(staged_result->value()));
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
        result.module_order_ = options.module_order;
        result.ignored_module_entries_ = options.ignored_module_entries;
        result.relocations_planned_ = options.plan_relocations;
        result.executable_state_valid_ = options.plan_relocations && options.apply_relocations;
        result.modules_.reserve(staged.size());

        struct ParsedModuleData
        {
            std::optional<format::ModuleMetadata> metadata;
            std::optional<format::DynamicSymbolTable> symbols;
            std::vector<format::Relocation> relocations;
            std::optional<Error> error;
        };
        std::vector<std::optional<ParsedModuleData>> parsed_modules(staged.size());
        const auto parse_module = [&](std::size_t index) {
            ParsedModuleData parsed;
            const auto& source = staged[index];
            PreparedModuleOptions module_options = options.module_options;
            module_options.module_name = source.name;
            module_options.module_base = source.base;
            module_options.apply_relocations = false;
            const auto metadata = format::parse_module_metadata(
                result.memory_, source.base, module_options.metadata_options);
            if (!metadata)
            {
                parsed.error = metadata.error();
                parsed_modules[index] = std::move(parsed);
                return;
            }
            parsed.metadata = std::move(metadata).value();
            if (parsed.metadata->dynamic)
            {
                if (parsed.metadata->dynamic->symtab)
                {
                    const auto symbols = format::DynamicSymbolTable::parse(
                        result.memory_, parsed.metadata->dynamic.value(),
                        module_options.metadata_options.dynamic);
                    if (!symbols)
                    {
                        parsed.error = symbols.error();
                        parsed_modules[index] = std::move(parsed);
                        return;
                    }
                    parsed.symbols = std::move(symbols).value();
                }
                const auto rela_entries = format::parse_rela_table(
                    result.memory_, parsed.metadata->dynamic.value(),
                    module_options.metadata_options.dynamic);
                if (!rela_entries)
                {
                    parsed.error = rela_entries.error();
                    parsed_modules[index] = std::move(parsed);
                    return;
                }
                const auto rela = format::make_relocations(
                    rela_entries.value(), format::RelocationSource::Rela);
                if (!rela)
                {
                    parsed.error = rela.error();
                    parsed_modules[index] = std::move(parsed);
                    return;
                }
                parsed.relocations = std::move(rela).value();
                const auto jmprel_entries = format::parse_jmprel_table(
                    result.memory_, parsed.metadata->dynamic.value(),
                    module_options.metadata_options.dynamic);
                if (!jmprel_entries)
                {
                    parsed.error = jmprel_entries.error();
                    parsed_modules[index] = std::move(parsed);
                    return;
                }
                const auto jmprel = format::make_relocations(
                    jmprel_entries.value(), format::RelocationSource::JmpRel);
                if (!jmprel)
                {
                    parsed.error = jmprel.error();
                    parsed_modules[index] = std::move(parsed);
                    return;
                }
                parsed.relocations.insert(parsed.relocations.end(), jmprel.value().begin(),
                                          jmprel.value().end());
                if (!parsed.relocations.empty() && !parsed.symbols)
                {
                    parsed.error = make_error(
                        ErrorCode::MissingImportBinding,
                        "dynamic relocations are present without a dynamic symbol table");
                    parsed_modules[index] = std::move(parsed);
                    return;
                }
            }
            parsed_modules[index] = std::move(parsed);
        };
        const auto parse_workers = std::min<std::size_t>(
            10U, std::min(std::max<std::size_t>(options.module_workers, 1U),
                          std::max<std::size_t>(staged.size(), 1U)));
        if (parse_workers <= 1U)
        {
            for (std::size_t index = 0U; index < staged.size(); ++index) parse_module(index);
        }
        else
        {
            std::atomic<std::size_t> next_module{0U};
            std::vector<std::thread> workers;
            workers.reserve(parse_workers);
            try
            {
                for (std::size_t worker = 0U; worker < parse_workers; ++worker)
                    workers.emplace_back([&]() {
                        for (;;)
                        {
                            const auto index = next_module.fetch_add(1U, std::memory_order_relaxed);
                            if (index >= staged.size()) return;
                            try { parse_module(index); }
                            catch (const std::bad_alloc&)
                            {
                                ParsedModuleData parsed;
                                parsed.error = make_error(
                                    ErrorCode::ResourceLimit,
                                    "parallel module metadata allocation failed");
                                parsed_modules[index] = std::move(parsed);
                            }
                            catch (...)
                            {
                                ParsedModuleData parsed;
                                parsed.error = make_error(
                                    ErrorCode::ThreadCreationFailed,
                                    "parallel module metadata parsing failed unexpectedly");
                                parsed_modules[index] = std::move(parsed);
                            }
                        }
                    });
            }
            catch (...)
            {
                for (auto& worker : workers)
                    if (worker.joinable()) worker.join();
                return Result<ProcessImage>::failure(make_error(
                    ErrorCode::ThreadCreationFailed,
                    "unable to create the bounded module metadata worker pool"));
            }
            for (auto& worker : workers)
                if (worker.joinable()) worker.join();
        }
        for (const auto& parsed : parsed_modules)
        {
            if (!parsed)
                return Result<ProcessImage>::failure(make_error(
                    ErrorCode::ThreadCreationFailed,
                    "module metadata worker did not publish a result"));
            if (parsed->error) return Result<ProcessImage>::failure(*parsed->error);
        }

        for (std::size_t source_index = 0U; source_index < staged.size(); ++source_index)
        {
            const auto& source = staged[source_index];
            auto parsed = std::move(parsed_modules[source_index].value());
            PreparedModuleOptions module_options = options.module_options;
            module_options.module_name = source.name;
            module_options.module_base = source.base;
            module_options.apply_relocations = false;
            auto metadata = std::move(parsed.metadata).value();
            auto symbols = std::move(parsed.symbols);
            auto relocations = std::move(parsed.relocations);

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
                                 std::move(metadata), std::move(symbols), std::move(relocations),
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

        const auto relocation_regions = result.memory_.regions();
        std::size_t relocation_capacity = 0U;
        for (const auto& module : result.modules_)
            relocation_capacity += module.relocations.size();
        std::vector<loader::AppliedRelocation> pending;
        pending.reserve(relocation_capacity);
        std::vector<std::pair<std::size_t, std::size_t>> pending_owner;
        pending_owner.reserve(relocation_capacity);
        if (options.plan_relocations)
        {
            struct RelocationWork
            {
                std::vector<loader::AppliedRelocation> applied;
                std::vector<loader::UnresolvedRelocation> unresolved;
                std::vector<ProcessBinding> bindings;
                std::optional<Error> error;
            };
            struct RelocationChunk
            {
                std::size_t module_index;
                std::size_t begin;
                std::size_t end;
            };

            const auto plan_workers = std::min<std::size_t>(
                10U, std::min(std::max<std::size_t>(options.module_workers, 1U),
                              std::max<std::size_t>(relocation_capacity, 1U)));
            std::vector<RelocationChunk> chunks;
            for (std::size_t module_index = 0U; module_index < result.modules_.size(); ++module_index)
            {
                const auto count = result.modules_[module_index].relocations.size();
                if (count == 0U) continue;
                const auto parts = std::min(plan_workers, count);
                const auto chunk_size = (count + parts - 1U) / parts;
                for (std::size_t begin = 0U; begin < count; begin += chunk_size)
                    chunks.push_back(RelocationChunk{module_index, begin,
                                                     std::min(begin + chunk_size, count)});
            }
            std::vector<std::optional<RelocationWork>> work_results(chunks.size());
            const auto plan_chunk = [&](const RelocationChunk& chunk) {
                RelocationWork work;
                const auto& module = result.modules_[chunk.module_index];
                work.applied.reserve(chunk.end - chunk.begin);
                for (std::size_t relocation_index = chunk.begin; relocation_index < chunk.end;
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
                        work.error = make_error(
                            ErrorCode::UnsupportedRelocationType,
                            "unsupported relocation in module '" + module.identity.module +
                                "' at index " + std::to_string(relocation_index) + ": type " +
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
                                (symbol_name.empty() ? std::string{} : ", symbol '" + symbol_name + "'"));
                        return work;
                    }
                    const auto target = validate_relocation_target(
                        result.memory_, std::span<const memory::GuestMemoryRegionInfo>(relocation_regions),
                        relocation, relocation_index);
                    if (!target)
                    {
                        work.error = target.error();
                        return work;
                    }
                    std::uint64_t value = 0U;
                    if (relocation.type == format::AArch64RelocationType::Relative)
                    {
                        const auto calculated = checked_add_signed_u64(module.identity.guest_base,
                                                                         relocation.addend);
                        if (!calculated)
                        {
                            work.error = calculated.error();
                            return work;
                        }
                        value = calculated.value();
                    }
                    else
                    {
                        const auto* symbol = module.symbols ? module.symbols->at(relocation.symbol_index)
                                                            : nullptr;
                        if (symbol == nullptr)
                        {
                            work.error = make_error(
                                ErrorCode::InvalidSymbolIndex,
                                "process relocation symbol index is invalid");
                            return work;
                        }
                        ProviderLookup lookup;
                        if (symbol->is_defined())
                        {
                            const auto address = symbol_address(*symbol, module.identity.guest_base);
                            if (!address)
                            {
                                work.error = address.error();
                                return work;
                            }
                            const auto calculated = checked_add_signed_u64(address.value(),
                                                                             relocation.addend);
                            if (!calculated)
                            {
                                work.error = calculated.error();
                                return work;
                            }
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
                                    work.error = make_error(
                                        ErrorCode::InvalidProviderDefinition,
                                        "JUMP_SLOT provider is not an executable function");
                                    return work;
                                }
                                binding.provider_module = provider.module;
                                binding.provider_symbol_index = provider.symbol_index;
                                const auto provider_module_it = std::find_if(
                                    result.modules_.begin(), result.modules_.end(),
                                    [&](const auto& item) {
                                        return item.identity.module == provider.module;
                                    });
                                if (provider_module_it == result.modules_.end())
                                {
                                    work.error = make_error(
                                        ErrorCode::InvalidProviderDefinition,
                                        "selected provider module is absent from the process image");
                                    return work;
                                }
                                binding.provider_base = provider_module_it->identity.guest_base;
                                binding.provider_symbol_value = provider.value;
                                binding.provider_address = provider.address;
                                const auto calculated = checked_add_signed_u64(provider.address,
                                                                                 relocation.addend);
                                if (!calculated)
                                {
                                    work.error = calculated.error();
                                    return work;
                                }
                                value = calculated.value();
                                binding.resolved_value = value;
                                binding.applied = true;
                            }
                            else
                            {
                                work.unresolved.push_back(
                                    loader::UnresolvedRelocation{relocation_index, relocation,
                                                                 format::ImportSymbol{symbol->index,
                                                                                      symbol->name,
                                                                                      symbol->binding,
                                                                                      symbol->type,
                                                                                      symbol->visibility,
                                                                                      symbol->section_index}});
                            }
                            work.bindings.push_back(std::move(binding));
                            if (!lookup.selected_candidate) continue;
                        }
                    }
                    if (relocation.type == format::AArch64RelocationType::Abs32 &&
                        value > std::numeric_limits<std::uint32_t>::max())
                    {
                        work.error = make_error(
                            ErrorCode::ArithmeticOverflow,
                            "process ABS32 relocation value exceeds 32 bits");
                        return work;
                    }
                    work.applied.push_back(loader::AppliedRelocation{
                        relocation_index, relocation, value, loader::relocation_width(relocation.type)});
                }
                return work;
            };

            const auto run_chunk = [&](std::size_t index) {
                try { work_results[index] = plan_chunk(chunks[index]); }
                catch (const std::bad_alloc&)
                {
                    RelocationWork work;
                    work.error = make_error(ErrorCode::ResourceLimit,
                                            "parallel relocation planning allocation failed");
                    work_results[index] = std::move(work);
                }
                catch (...)
                {
                    RelocationWork work;
                    work.error = make_error(ErrorCode::ThreadCreationFailed,
                                            "parallel relocation planning failed unexpectedly");
                    work_results[index] = std::move(work);
                }
            };
            const auto actual_workers = std::min(plan_workers, std::max<std::size_t>(chunks.size(), 1U));
            if (actual_workers <= 1U)
            {
                for (std::size_t index = 0U; index < chunks.size(); ++index) run_chunk(index);
            }
            else
            {
                std::atomic<std::size_t> next_chunk{0U};
                std::vector<std::thread> workers;
                workers.reserve(actual_workers);
                try
                {
                    for (std::size_t worker = 0U; worker < actual_workers; ++worker)
                        workers.emplace_back([&]() {
                            for (;;)
                            {
                                const auto index = next_chunk.fetch_add(1U, std::memory_order_relaxed);
                                if (index >= chunks.size()) return;
                                run_chunk(index);
                            }
                        });
                }
                catch (...)
                {
                    for (auto& worker : workers)
                        if (worker.joinable()) worker.join();
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::ThreadCreationFailed,
                        "unable to create the bounded relocation planning worker pool"));
                }
                for (auto& worker : workers)
                    if (worker.joinable()) worker.join();
            }

            // Chunks were created in module/offset order. Merge in that same
            // order so worker scheduling cannot affect reports or bytes.
            for (std::size_t index = 0U; index < work_results.size(); ++index)
            {
                if (!work_results[index])
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::ThreadCreationFailed,
                        "relocation planning worker did not publish a result"));
                auto& work = work_results[index].value();
                if (work.error) return Result<ProcessImage>::failure(*work.error);
                auto& module = result.modules_[chunks[index].module_index];
                module.unresolved_relocations.insert(
                    module.unresolved_relocations.end(),
                    std::make_move_iterator(work.unresolved.begin()),
                    std::make_move_iterator(work.unresolved.end()));
                result.bindings_.insert(
                    result.bindings_.end(), std::make_move_iterator(work.bindings.begin()),
                    std::make_move_iterator(work.bindings.end()));
                for (auto& relocation : work.applied)
                {
                    pending_owner.push_back({chunks[index].module_index, relocation.relocation_index});
                    pending.push_back(std::move(relocation));
                }
            }
        }

        if (options.apply_relocations)
        {
            loader::RelocationPlan plan;
            plan.relocation_count = pending.size();
            plan.applied = std::move(pending);
            const auto applied = loader::apply_relocation_plan(result.memory_, plan,
                                                               options.module_options.relocation_options);
            if (!applied) return Result<ProcessImage>::failure(applied.error());
            for (const auto& owner : pending_owner) ++result.modules_[owner.first].applied_relocations;
        }
        for (auto& binding : result.bindings_)
        {
            if (binding.applied && !options.apply_relocations) binding.applied = false;
            if (binding.applied)
            {
                if (!binding.resolved_value)
                {
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::RelocationPlanFailed,
                        "applied process binding has no resolved relocation value"));
                }
                const auto width = loader::relocation_width(binding.relocation.type);
                std::array<std::byte, sizeof(std::uint64_t)> bytes{};
                const auto read = result.memory_.read(
                    binding.relocation.target_address,
                    std::span<std::byte>(bytes.data(), width));
                if (!read)
                {
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::RelocationPlanFailed,
                        "applied process binding slot could not be read back: " + read.error().message));
                }
                std::uint64_t observed = 0U;
                for (std::size_t index = 0U; index < width; ++index)
                {
                    observed |= static_cast<std::uint64_t>(
                                    std::to_integer<unsigned int>(bytes[index]))
                                << (index * 8U);
                }
                const auto expected = width == 4U
                                          ? (*binding.resolved_value & 0xffffffffULL)
                                          : *binding.resolved_value;
                if (observed != expected)
                {
                    return Result<ProcessImage>::failure(make_error(
                        ErrorCode::RelocationPlanFailed,
                        "applied process binding slot does not contain its resolved guest value"));
                }
                binding.slot_value_verified = true;
            }
        }

        // Build deterministic lookup indexes once. Indirect-target
        // certification still performs the same evidence validation and
        // relocation readback, but equivalent runtime observations no longer
        // rescan every process symbol and relocation table.
        for (const auto& module : result.modules_)
        {
            for (std::size_t relocation_index = 0U;
                 relocation_index < module.relocations.size(); ++relocation_index)
            {
                const auto& relocation = module.relocations[relocation_index];
                result.relocation_references_[relocation.target_address].push_back(
                    ProcessRelocationReference{module.identity.module, relocation_index});
            }
            if (!module.symbols) continue;
            for (const auto& symbol : module.symbols->symbols)
            {
                if (!symbol.is_defined() || symbol.type != format::SymbolType::Function) continue;
                const auto address = symbol_address(symbol, module.identity.guest_base);
                if (address)
                    result.function_target_references_[address.value()].push_back(
                        ProcessFunctionTargetReference{module.identity.module, symbol.index,
                                                       std::nullopt});
            }
            for (std::size_t relocation_index = 0U;
                 relocation_index < module.relocations.size(); ++relocation_index)
            {
                const auto& relocation = module.relocations[relocation_index];
                if (relocation.type == format::AArch64RelocationType::None ||
                    relocation.type == format::AArch64RelocationType::Unknown)
                    continue;
                const auto* symbol = module.symbols->at(relocation.symbol_index);
                if (symbol == nullptr || !symbol->is_defined() ||
                    symbol->type != format::SymbolType::Function)
                    continue;
                const auto address = symbol_address(*symbol, module.identity.guest_base);
                if (!address) continue;
                const auto resolved = checked_add_signed_u64(address.value(), relocation.addend);
                if (resolved)
                    result.function_target_references_[resolved.value()].push_back(
                        ProcessFunctionTargetReference{module.identity.module, symbol->index,
                                                       relocation_index});
            }
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
    MapStorage storage;
    storage.reserve(maps.size());
    for (auto& map : maps)
    {
        if (!map.frozen())
        {
            return Result<ProcessFunctionMap>::failure(make_error(
                ErrorCode::InvalidArgument, "process function map requires frozen module maps"));
        }
        storage.push_back(std::make_shared<const FinalizedFunctionMap>(std::move(map)));
    }
    return from_storage(std::move(storage));
}

Result<ProcessFunctionMap> ProcessFunctionMap::replace_module(
    const ProcessFunctionMap& existing, std::string_view module,
    FinalizedFunctionMap replacement)
{
    if (!replacement.frozen() || replacement.identity().module != module)
    {
        return Result<ProcessFunctionMap>::failure(make_error(
            ErrorCode::InvalidArgument,
            "incremental process map replacement must be a frozen map for the requested module"));
    }

    const auto found = std::find_if(
        existing.maps_.begin(), existing.maps_.end(), [module](const auto& item) {
            return item != nullptr && item->identity().module == module;
        });
    if (found != existing.maps_.end() && existing.address_ranges_ != nullptr)
    {
        const auto map_index = static_cast<std::size_t>(found - existing.maps_.begin());
        // Replacing an existing module preserves its sorted position. If its
        // executable layout is unchanged, the interval index remains valid
        // and can be shared by the new immutable process map.
        if ((*found)->identity().executable_ranges == replacement.identity().executable_ranges)
        {
            const auto replacement_ptr =
                std::make_shared<const FinalizedFunctionMap>(std::move(replacement));
            for (const auto& function : replacement_ptr->functions())
            {
                const auto* previous_owner = existing.find(function.canonical_entry);
                if (previous_owner == nullptr) continue;
                const auto* previous_map = existing.map_for(function.canonical_entry);
                if (previous_map == nullptr || previous_map->identity().module != module)
                {
                    return Result<ProcessFunctionMap>::failure(make_error(
                        ErrorCode::FunctionBoundaryConflict,
                        "two process modules claim the same canonical function entry"));
                }
            }

            ProcessFunctionMap result;
            result.maps_ = existing.maps_;
            result.maps_[map_index] = replacement_ptr;
            result.address_ranges_ = existing.address_ranges_;
            return Result<ProcessFunctionMap>::success(std::move(result));
        }
    }

    // A new module or a changed executable layout needs a complete index
    // construction. This is uncommon during refinement but remains the safe
    // general path for callers using replace_module directly.
    MapStorage storage = existing.maps_;
    if (found == existing.maps_.end())
    {
        storage.push_back(std::make_shared<const FinalizedFunctionMap>(std::move(replacement)));
    }
    else
    {
        const auto map_index = static_cast<std::size_t>(found - existing.maps_.begin());
        storage[map_index] =
            std::make_shared<const FinalizedFunctionMap>(std::move(replacement));
    }
    std::sort(storage.begin(), storage.end(), [](const auto& left, const auto& right) {
        return left->identity().module < right->identity().module;
    });
    return from_storage(std::move(storage));
}

Result<ProcessFunctionMap> ProcessFunctionMap::from_storage(MapStorage maps)
{
    ProcessFunctionMap result;
    result.maps_ = std::move(maps);
    for (const auto& map : result.maps_)
    {
        if (map == nullptr || !map->frozen())
        {
            return Result<ProcessFunctionMap>::failure(make_error(
                ErrorCode::InvalidArgument, "process function map requires frozen module maps"));
        }
    }
    for (std::size_t index = 1U; index < result.maps_.size(); ++index)
    {
        if (result.maps_[index - 1U]->identity().module == result.maps_[index]->identity().module)
        {
            return Result<ProcessFunctionMap>::failure(make_error(
                ErrorCode::DuplicateModuleIdentity,
                "process function maps contain a duplicate logical module identity"));
        }
    }

    auto address_ranges = std::make_shared<AddressRangeIndex>();
    std::map<GuestAddress, std::size_t> canonical_entries;
    for (std::size_t map_index = 0U; map_index < result.maps_.size(); ++map_index)
    {
        const auto& map = result.maps_[map_index];
        for (const auto& function : map->functions())
        {
            const auto inserted = canonical_entries.emplace(function.canonical_entry, map_index);
            if (!inserted.second)
            {
                return Result<ProcessFunctionMap>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "two process modules claim the same canonical function entry"));
            }
        }
        for (const auto& range : map->identity().executable_ranges)
        {
            if (range.size == 0U) continue;
            const auto end = checked_add_u64(range.base, range.size);
            if (!end)
            {
                return Result<ProcessFunctionMap>::failure(make_error(
                    ErrorCode::ArithmeticOverflow,
                    "process function-map executable range overflows"));
            }
            address_ranges->push_back(
                ProcessFunctionMap::AddressRangeIndexEntry{range.base, end.value(), 0U, map_index});
        }
    }
    std::sort(address_ranges->begin(), address_ranges->end(),
              [](const auto& left, const auto& right) {
                  if (left.base != right.base) return left.base < right.base;
                  if (left.end != right.end) return left.end < right.end;
                  return left.map_index < right.map_index;
              });
    GuestAddress maximum_end = 0U;
    for (auto& range : *address_ranges)
    {
        maximum_end = std::max(maximum_end, range.end);
        range.maximum_end = maximum_end;
    }
    result.address_ranges_ = std::move(address_ranges);
    return Result<ProcessFunctionMap>::success(std::move(result));
}

std::optional<std::size_t> ProcessFunctionMap::find_map_index(GuestAddress entry) const noexcept
{
    if (address_ranges_ == nullptr || address_ranges_->empty()) return std::nullopt;
    const auto& ranges = *address_ranges_;
    auto current = std::upper_bound(
        ranges.begin(), ranges.end(), entry,
        [](GuestAddress value, const AddressRangeIndexEntry& range) {
            return value < range.base;
        });
    while (current != ranges.begin())
    {
        --current;
        if (entry < current->end && current->map_index < maps_.size())
        {
            const auto& map = maps_[current->map_index];
            if (map != nullptr && map->find_canonical_entry(entry) != nullptr)
                return current->map_index;
        }
        const auto current_index = static_cast<std::size_t>(current - ranges.begin());
        if (current_index == 0U || ranges[current_index - 1U].maximum_end <= entry) break;
    }
    return std::nullopt;
}

const FunctionRecord* ProcessFunctionMap::find(GuestAddress entry) const noexcept
{
    const auto map_index = find_map_index(entry);
    if (!map_index) return nullptr;
    return maps_[map_index.value()]->find_canonical_entry(entry);
}

const FinalizedFunctionMap* ProcessFunctionMap::map_for(GuestAddress entry) const noexcept
{
    const auto map_index = find_map_index(entry);
    return map_index ? maps_[map_index.value()].get() : nullptr;
}

} // namespace switchrecomp::analysis
