#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/elf_dynamic.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace switchrecomp::format
{

class DynamicStringTable
{
  public:
    DynamicStringTable() = default;

    [[nodiscard]] static Result<DynamicStringTable> parse(
        const memory::GuestMemory& guest_memory, const DynamicInfo& dynamic,
        const DynamicParseLimits& limits = {});

    // The returned view is valid while this table remains alive and unchanged.
    [[nodiscard]] Result<std::string_view> get(std::uint64_t offset) const;
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  private:
    explicit DynamicStringTable(std::string bytes) : bytes_(std::move(bytes)) {}

    std::string bytes_;
};

enum class SymbolBinding
{
    Local,
    Global,
    Weak,
    Unknown,
};

enum class SymbolType
{
    None,
    Object,
    Function,
    Section,
    File,
    Tls,
    Unknown,
};

enum class SymbolVisibility
{
    Default,
    Internal,
    Hidden,
    Protected,
    Unknown,
};

[[nodiscard]] std::string_view symbol_binding_name(SymbolBinding binding) noexcept;
[[nodiscard]] std::string_view symbol_type_name(SymbolType type) noexcept;
[[nodiscard]] std::string_view symbol_visibility_name(SymbolVisibility visibility) noexcept;

struct DynamicSymbol
{
    std::uint32_t index;
    std::uint32_t name_offset;
    std::string name;
    SymbolBinding binding;
    SymbolType type;
    SymbolVisibility visibility;
    std::uint16_t section_index;
    // st_value remains a module-relative guest value for Switch NSO images.
    // It is not a file offset and never a host pointer.
    std::uint64_t value;
    std::uint64_t size;

    [[nodiscard]] bool is_defined() const noexcept { return section_index != 0U; }
};

struct ImportSymbol
{
    std::uint32_t symbol_index;
    std::string name;
    SymbolBinding binding;
    SymbolType type;
    SymbolVisibility visibility;
};

struct DynamicSymbolTable
{
    std::vector<DynamicSymbol> symbols;
    DynamicStringTable strings;

    [[nodiscard]] static Result<DynamicSymbolTable> parse(
        const memory::GuestMemory& guest_memory, const DynamicInfo& dynamic,
        const DynamicParseLimits& limits = {});

    [[nodiscard]] const DynamicSymbol* at(std::uint32_t index) const noexcept;
    [[nodiscard]] std::vector<ImportSymbol> imports() const;
};

} // namespace switchrecomp::format
