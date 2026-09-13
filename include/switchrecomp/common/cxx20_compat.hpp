#pragma once

#include <bit>
#include <type_traits>

// Apple Clang 14 ships a C++20 language mode with a libc++ that predates
// std::bit_cast. Keep the source portable without depending on a private
// developer-only forced-include header. Newer standard libraries already
// provide the API, so this compatibility definition is inactive there.
#if defined(__has_builtin)
#if __has_builtin(__builtin_bit_cast) && !defined(__cpp_lib_bit_cast)
namespace std
{
template <class To, class From>
    requires(sizeof(To) == sizeof(From) && std::is_trivially_copyable_v<To> &&
             std::is_trivially_copyable_v<From>)
constexpr To bit_cast(const From& value) noexcept
{
    return __builtin_bit_cast(To, value);
}
}
#endif
#endif
