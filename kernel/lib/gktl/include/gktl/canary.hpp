/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#pragma once

#include <ours/assert.hpp>

#include <gktl/type_info.hpp>

#ifdef GKTL_CONFIG_CANARY
#define ENABLE_CANARY GKTL_CONFIG_CANARY
#else
#define ENABLE_CANARY 1
#endif // GKTL_CONFIG_CANARY

namespace gktl {
consteval auto magic(std::string_view literal) -> u32 {
    constexpr auto BASE = 63;

    auto const encode = [](char c) -> unsigned {
        if (c >= 'A' && c <= 'Z')
            return c - 'A'; // 0-25
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26; // 26-51
        if (c >= '0' && c <= '9')
            return c - '0' + 52; // 52-61
        if (c == '_')
            return 62; // 62
        return 63;     // invalid
    };

    u32 h = 0;
    for (auto i = usize{0}; i < literal.size(); ++i) {
        h = h * BASE + encode(literal[i]);
    }

    return h;
}

template <typename T>
FORCE_INLINE
consteval auto magic() -> u32 {
    return magic(TypeName<T>::value());
}

class Canary {
    using Self = Canary;

  public:
    // Some static and aggregate kernel objects cannot name their enclosing
    // type during member initialization. A zero canary keeps construction
    // valid; typed constructors remain the checked path for guarded objects.
    //
    // A class that calls verify<T>() is not one of those, and must seed its
    // canary -- `gktl::Canary m_canary{this};` on the declaration, so that
    // defaulted constructors are covered too. Leaving it zero there is a
    // guaranteed assertion, and used to be invisible only because
    // gktl::magic<T>() was itself zero for every T and the check compared
    // zero against zero.
    FORCE_INLINE
    constexpr Canary()
        : m_magic(0) {}

    template <typename T>
    FORCE_INLINE
    constexpr Canary(T *owner)
        : m_magic(gktl::magic<T>()) {}

    FORCE_INLINE
    constexpr Canary(u32 magic)
        : m_magic(magic) {}

    template <typename T>
    FORCE_INLINE
    constexpr auto verify() const -> void {
        DEBUG_ASSERT(m_magic == gktl::magic<T>(), "Error type magic");
    }

    FORCE_INLINE
    constexpr auto verify(u32 magic) const -> void {
        DEBUG_ASSERT(m_magic == magic, "Error type magic");
    }

    FORCE_INLINE
    constexpr auto magic() const -> u32 {
        return m_magic;
    }

  private:
    u32 m_magic;
};

} // namespace gktl
