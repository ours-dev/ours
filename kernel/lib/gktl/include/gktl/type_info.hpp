#pragma once

#include <gktl/types.hpp>
#include <ustl/views/string_view.hpp>

namespace gktl {
namespace priv {
    template <typename Type>
    FORCE_INLINE [[nodiscard]]
    constexpr auto pretty_function() noexcept -> char const * {
#if defined OURS_PRETTY_FUNCTION
        return static_cast<const char *>(OURS_PRETTY_FUNCTION);
#else
        return "";
#endif
    }

    template <typename Type>
    FORCE_INLINE [[nodiscard]]
    constexpr auto strip_type_name() noexcept {
#if defined OURS_PRETTY_FUNCTION
        const std::string_view full_name{pretty_function<Type>()};
        auto first = full_name.find_first_not_of(
            ' ', full_name.find_first_of(OURS_PRETTY_FUNCTION_PREFIX) + 1);
        auto value =
            full_name.substr(first, full_name.find_last_of(OURS_PRETTY_FUNCTION_SUFFIX) - first);
        return value.data();
#else
        return std::string_view{};
#endif
    }

    template <typename Type, auto = stripped_type_name<Type>().find_first_of('.')>
    FORCE_INLINE [[nodiscard]]
    constexpr auto type_name(int) noexcept {
        constexpr auto value = strip_type_name<Type>();
        return value;
    }

    template <typename Type>
    FORCE_INLINE [[nodiscard]]
    constexpr auto type_name(char) noexcept {
        constexpr static auto value = strip_type_name<Type>();
        return value;
    }
} // namespace priv

template <typename Type, typename = void>
struct TypeName final {
    FORCE_INLINE [[nodiscard]]
    constexpr static auto value() noexcept {
        return priv::type_name<Type>(0);
    }

    FORCE_INLINE [[nodiscard]]
    constexpr operator ustl::views::StringView() const noexcept {
        return value();
    }
};
} // namespace gktl