#ifndef DERIVE_ENUMASK_HPP
#define DERIVE_ENUMASK_HPP

#include <meta>
#include <type_traits>
#include <utility>

namespace derive {
inline constexpr struct {
} enumask;

template <typename T>
concept Enumaskable = requires {
    std::is_enum_v<T>;
    !std::meta::annotations_of_with_type(^^T, ^^decltype(enumask)).empty();
};
} // namespace derive

template <derive::Enumaskable T>
constexpr auto operator&(T x, T y) -> T {
    return static_cast<T>(std::to_underlying(x) & std::to_underlying(y));
}

template <derive::Enumaskable T>
constexpr auto operator|(T x, T y) -> T {
    return static_cast<T>(std::to_underlying(x) | std::to_underlying(y));
}

template <derive::Enumaskable T>
constexpr auto operator^(T x, T y) -> T {
    return static_cast<T>(std::to_underlying(x) ^ std::to_underlying(y));
}

template <derive::Enumaskable T>
constexpr auto operator&=(T &x, T y) -> T & {
    x = static_cast<T>(std::to_underlying(x) & std::to_underlying(y));
    return x;
}

template <derive::Enumaskable T>
constexpr auto operator|=(T &x, T y) -> T & {
    x = static_cast<T>(std::to_underlying(x) | std::to_underlying(y));
    return x;
}

template <derive::Enumaskable T>
constexpr auto operator^(T &x, T y) -> T & {
    x = static_cast<T>(std::to_underlying(x) ^ std::to_underlying(y));
    return x;
}

template <derive::Enumaskable T>
constexpr auto operator~(T x) -> T {
    return static_cast<T>(~std::to_underlying(x));
}

template <derive::Enumaskable T>
constexpr auto operator!(T x) -> bool {
    return !std::to_underlying(x);
}

#endif // #ifndef DERIVE_ENUMASK_HPP