/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_UTIL_TYPE_MAP_HPP
#define USTL_UTIL_TYPE_MAP_HPP 1

#include <ustl/util/type_list.hpp>

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
    template <typename Accumulated, template <typename> class Predicate, typename... Rest>
    struct FilterImpl;

    template <typename... Kept, template <typename> class Predicate>
    struct FilterImpl<TypeList<Kept...>, Predicate> {
        using type = TypeList<Kept...>;
    };

    template <typename... Kept, template <typename> class Predicate, typename Head,
              typename... Tail>
    struct FilterImpl<TypeList<Kept...>, Predicate, Head, Tail...> {
        // Predicate selects what to *drop*, so a matching Head is left out of
        // the accumulator; see Filter below.
        using type = typename std::conditional_t<
            Predicate<Head>::value,
            FilterImpl<TypeList<Kept...>, Predicate, Tail...>,
            FilterImpl<TypeList<Kept..., Head>, Predicate, Tail...>>::type;
    };

    template <typename List, template <typename> class Predicate> struct FilterList;

    template <typename... Types, template <typename> class Predicate>
    struct FilterList<TypeList<Types...>, Predicate> {
        using type = typename FilterImpl<TypeList<>, Predicate, Types...>::type;
    };

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

    /// Removes from |List| every type for which `Predicate<T>::value` is true,
    /// keeping the rest in order.
    ///
    /// Note the polarity: the predicate names what to discard, not what to
    /// keep. That reads naturally at the call sites, which pass filters such
    /// as `DisabledZoneFilter` -- "drop the zones that are disabled".
    template <typename List, template <typename> class Predicate>
    using Filter = typename FilterList<List, Predicate>::type;
};
} // namespace ustl

#endif // USTL_UTIL_TYPE_MAP_HPP
