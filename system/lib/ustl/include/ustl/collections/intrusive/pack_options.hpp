/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_PACK_OPTIONS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_PACK_OPTIONS_HPP 1

#include <utility>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename... Types>
struct Typelist final {};

template <std::size_t Index, typename List>
struct TypelistElement;

template <std::size_t Index, typename Head, typename... Tail>
struct TypelistElement<Index, Typelist<Head, Tail...>>
    : TypelistElement<Index - 1, Typelist<Tail...>> {};

template <typename Head, typename... Tail>
struct TypelistElement<0, Typelist<Head, Tail...>> {
    using Type = Head;
};

template <typename List>
struct InvertTypelist;

template <>
struct InvertTypelist<Typelist<>> final {
    using Type = Typelist<>;
};

template <typename... Types>
struct InvertTypelist<Typelist<Types...>> final {
  private:
    template <std::size_t... Indices>
    static auto reverse(std::index_sequence<Indices...>) -> Typelist<
        typename TypelistElement<sizeof...(Types) - Indices - 1, Typelist<Types...>>::Type...>;

  public:
    using Type = decltype(reverse(std::index_sequence_for<Types...>{}));
};

template <typename List>
struct DoPack;

template <typename Option>
struct DoPack<Typelist<Option>> final {
    using Type = Option;
};

template <typename First, typename... Remaining>
struct DoPack<Typelist<First, Remaining...>> final {
    using Type = typename First::template Pack<typename DoPack<Typelist<Remaining...>>::Type>;
};

template <typename DefaultOptions, typename... Options>
struct PackOptions final {
  private:
    using ReversedOptions = typename InvertTypelist<Typelist<DefaultOptions, Options...>>::Type;

  public:
    using Type = typename DoPack<ReversedOptions>::Type;
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_PACK_OPTIONS_HPP
