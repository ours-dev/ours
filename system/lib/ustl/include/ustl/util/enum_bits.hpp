/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_UTIL_ENUM_BITS_HPP
#define USTL_UTIL_ENUM_BITS_HPP 1

#include <type_traits>
#include <utility>

// Opt-in stays in the enum's associated namespace so operators remain
// unavailable for enums that have not explicitly declared bitmask semantics.
#define USTL_ENABLE_ENUM_BITMASK(type)                                                             \
    [[maybe_unused]]                                                                               \
    auto ustl_enable_enum_bitmask(type) -> ::std::true_type

#define USTL_ENABLE_INNER_ENUM_BITMASK(type)                                                       \
    friend constexpr auto ustl_enable_enum_bitmask(type) -> ::std::true_type {                     \
        return {};                                                                                 \
    }

template <typename Enum>
auto ustl_enable_enum_bitmask(Enum) -> std::false_type;

template <typename Enum>
inline constexpr bool UstlEnumBitMaskEnabledV =
    decltype(ustl_enable_enum_bitmask(std::declval<Enum>()))::value;

template <typename Enum>
    requires(std::is_enum_v<Enum> && UstlEnumBitMaskEnabledV<Enum>)
constexpr auto operator|(Enum lhs, Enum rhs) noexcept -> Enum {
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Enum>(static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs));
}

template <typename Enum>
    requires(std::is_enum_v<Enum> && UstlEnumBitMaskEnabledV<Enum>)
constexpr auto operator&(Enum lhs, Enum rhs) noexcept -> Enum {
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Enum>(static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs));
}

template <typename Enum>
    requires(std::is_enum_v<Enum> && UstlEnumBitMaskEnabledV<Enum>)
constexpr auto operator~(Enum value) noexcept -> Enum {
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Enum>(~static_cast<Underlying>(value));
}

template <typename Enum>
    requires(std::is_enum_v<Enum> && UstlEnumBitMaskEnabledV<Enum>)
constexpr auto operator|=(Enum &lhs, Enum rhs) noexcept -> Enum & {
    return lhs = lhs | rhs;
}

template <typename Enum>
    requires(std::is_enum_v<Enum> && UstlEnumBitMaskEnabledV<Enum>)
constexpr auto operator&=(Enum &lhs, Enum rhs) noexcept -> Enum & {
    return lhs = lhs & rhs;
}

template <typename Enum>
    requires(std::is_enum_v<Enum> && UstlEnumBitMaskEnabledV<Enum>)
constexpr auto operator!(Enum value) noexcept -> bool {
    using Underlying = std::underlying_type_t<Enum>;
    return !static_cast<Underlying>(value);
}

#endif // USTL_UTIL_ENUM_BITS_HPP
