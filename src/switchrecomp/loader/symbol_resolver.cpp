#include "switchrecomp/loader/symbol_resolver.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <new>
#include <utility>

namespace switchrecomp::loader
{

Result<void> SymbolResolver::add_external(std::string name, memory::GuestAddress address)
{
    try
    {
        const auto inserted = external_symbols_.emplace(std::move(name), address);
        if (!inserted.second)
        {
            return Result<void>::failure(
                make_error(ErrorCode::DuplicateExternalSymbol,
                           "external symbol is already registered: " + inserted.first->first));
        }
        return Result<void>::success();
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "external symbol registry allocation failed"));
    }
}

Result<ResolvedSymbol> SymbolResolver::resolve(std::uint32_t symbol_index) const
{
    const auto* symbol = symbols_.at(symbol_index);
    if (symbol == nullptr)
    {
        return Result<ResolvedSymbol>::failure(make_error(
            ErrorCode::InvalidSymbolIndex,
            "symbol index " + std::to_string(symbol_index) + " exceeds dynsym size " +
                std::to_string(symbols_.symbols.size())));
    }

    if (symbol->is_defined())
    {
        // SHN_ABS values are already absolute in the guest address domain;
        // ordinary section-backed NSO symbols are module-relative.
        constexpr std::uint16_t shn_abs = 0xfff1U;
        const auto address = symbol->section_index == shn_abs
                                 ? Result<memory::GuestAddress>::success(symbol->value)
                                 : checked_add_u64(module_base_, symbol->value);
        if (!address)
        {
            return Result<ResolvedSymbol>::failure(make_error(
                address.error().code,
                "symbol[" + std::to_string(symbol_index) + "] value plus module base overflows"));
        }
        return Result<ResolvedSymbol>::success(
            ResolvedSymbol{symbol_index, symbol->name, address.value(), true, false});
    }

    const auto external = external_symbols_.find(symbol->name);
    if (external != external_symbols_.end())
    {
        return Result<ResolvedSymbol>::success(
            ResolvedSymbol{symbol_index, symbol->name, external->second, true,
                           symbol->binding == format::SymbolBinding::Weak});
    }

    if (symbol->binding == format::SymbolBinding::Weak)
    {
        // ELF weak undefined references resolve to the null address when no
        // provider exists. The result remains marked unresolved so diagnostics
        // can expose the missing import rather than hiding it.
        return Result<ResolvedSymbol>::success(
            ResolvedSymbol{symbol_index, symbol->name, 0U, false, true});
    }
    return Result<ResolvedSymbol>::failure(make_error(
        ErrorCode::UndefinedStrongSymbol,
        "undefined strong symbol[" + std::to_string(symbol_index) + "] '" + symbol->name + "'"));
}

std::vector<format::ImportSymbol> SymbolResolver::unresolved_imports() const
{
    std::vector<format::ImportSymbol> result;
    for (const auto& import : symbols_.imports())
    {
        if (external_symbols_.find(import.name) == external_symbols_.end())
        {
            result.push_back(import);
        }
    }
    return result;
}

} // namespace switchrecomp::loader
