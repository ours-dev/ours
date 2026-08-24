/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_UTIL_TYPE_MAP_HPP
#define USTL_UTIL_TYPE_MAP_HPP 1

#include <type_traits>

namespace ustl {
template <typename Key, typename Value> struct TypeMapEntry {
    using key_type = Key;
    using value_type = Value;
};

template <typename... Entries> struct TypeMap {};

struct TypeAlgos {
    struct NotFound {};

  private:
    template <typename Map, typename Key> struct FindImpl;

    template <typename Key> struct FindImpl<TypeMap<>, Key> {
        using type = NotFound;
    };

    template <typename Key, typename Value, typename... Rest>
    struct FindImpl<TypeMap<TypeMapEntry<Key, Value>, Rest...>, Key> {
        using type = Value;
    };

    template <typename FirstKey, typename Value, typename... Rest, typename Key>
    struct FindImpl<TypeMap<TypeMapEntry<FirstKey, Value>, Rest...>, Key>
        : FindImpl<TypeMap<Rest...>, Key> {};

  public:
    template <typename Map, typename Key> using Find = typename FindImpl<Map, Key>::type;
};
} // namespace ustl

#endif // USTL_UTIL_TYPE_MAP_HPP
