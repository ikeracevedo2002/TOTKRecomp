#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/dynamic_symbols.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace switchrecomp::loader
{

struct ResolvedSymbol
{
    std::uint32_t symbol_index;
    std::string name;
    memory::GuestAddress address;
    bool resolved;
    bool weak;
    format::SymbolBinding binding;
    format::SymbolType type;
    format::SymbolVisibility visibility;
};

class SymbolResolver
{
  public:
    SymbolResolver(const format::DynamicSymbolTable& symbols,
                   memory::GuestAddress module_base) noexcept
        : symbols_(symbols), module_base_(module_base)
    {
    }

    [[nodiscard]] Result<void> add_external(std::string name, memory::GuestAddress address);
    [[nodiscard]] Result<ResolvedSymbol> resolve(std::uint32_t symbol_index) const;
    // Resolve a relocation symbol without turning a valid undefined external
    // binding into a loader error. Invalid symbol indices and malformed symbol
    // bindings remain errors.
    [[nodiscard]] Result<ResolvedSymbol> resolve_for_relocation(
        std::uint32_t symbol_index) const;
    [[nodiscard]] std::vector<format::ImportSymbol> unresolved_imports() const;
    [[nodiscard]] memory::GuestAddress module_base_for_relocation() const noexcept
    {
        return module_base_;
    }

  private:
    const format::DynamicSymbolTable& symbols_;
    memory::GuestAddress module_base_;
    std::map<std::string, memory::GuestAddress> external_symbols_;
};

} // namespace switchrecomp::loader
