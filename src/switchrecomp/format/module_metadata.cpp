#include "switchrecomp/format/module_metadata.hpp"

#include "switchrecomp/common/logging.hpp"

#include <string>
#include <utility>

namespace switchrecomp::format
{

Result<ModuleMetadata> parse_module_metadata(
    const memory::GuestMemory& guest_memory, memory::GuestAddress module_base,
    const ModuleMetadataParseOptions& options)
{
    ModuleMetadata result{module_base, std::nullopt, std::nullopt};

    const auto discovered = discover_mod0(guest_memory, module_base);
    if (!discovered)
    {
        return Result<ModuleMetadata>::failure(discovered.error());
    }
    if (!discovered.value())
    {
        logging::log_debug(logging::LogCategory::Format,
                           "MOD0 was not present in the loaded module");
        return Result<ModuleMetadata>::success(std::move(result));
    }

    const auto mod0 = parse_mod0(guest_memory, discovered.value().value(), options.mod0);
    if (!mod0)
    {
        return Result<ModuleMetadata>::failure(mod0.error());
    }
    result.mod0 = mod0.value();

    logging::log_debug(logging::LogCategory::Format,
                       "MOD0 located at guest address " +
                           std::to_string(mod0.value().address));

    const auto dynamic =
        parse_dynamic(guest_memory, module_base, mod0.value().dynamic_address, options.dynamic);
    if (!dynamic)
    {
        return Result<ModuleMetadata>::failure(dynamic.error());
    }
    result.dynamic = dynamic.value();
    logging::log_debug(logging::LogCategory::Format,
                       "dynamic entries parsed: " +
                           std::to_string(dynamic.value().entry_count));
    return Result<ModuleMetadata>::success(std::move(result));
}

} // namespace switchrecomp::format
