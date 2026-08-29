/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_IITERATOR_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_IITERATOR_HPP 1

#include <ustl/collections/intrusive/pointer_traits.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace ustl::collections::intrusive::detail {
template <typename ValueTraits>
consteval auto value_traits_are_stateful() -> bool {
    if constexpr (requires {
                      { ValueTraits::IS_STATEFUL_VALUE_TRAITS } -> std::convertible_to<bool>;
                  }) {
        return ValueTraits::IS_STATEFUL_VALUE_TRAITS;
    } else {
        return !std::is_empty_v<ValueTraits>;
    }
}

template <typename ValueTraits, typename = void>
struct ValueTraitsPointer final {
    using Type =
        typename PointerTraits<typename ValueTraits::NodePtr>::template Rebind<ValueTraits>;
};

template <typename ValueTraits>
struct ValueTraitsPointer<ValueTraits, std::void_t<typename ValueTraits::ValueTraitsPtr>> final {
    using Type = typename ValueTraits::ValueTraitsPtr;
};

template <typename ValueTraits>
struct ValueTraitsPointers final {
    using ValueTraitsPtr = typename ValueTraitsPointer<ValueTraits>::Type;
    using ConstValueTraitsPtr =
        typename PointerTraits<ValueTraitsPtr>::template Rebind<ValueTraits const>;
};

template <typename TValueTraits, bool IsConst, typename Category>
struct Iiter final {
    using ValueTraits = TValueTraits;
    using NodeTraits = typename ValueTraits::NodeTraits;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using VoidPointer = typename PointerTraits<NodePtr>::template Rebind<void>;
    using Element = typename ValueTraits::Element;
    using PtrMut = typename ValueTraits::PtrMut;
    using Ptr = typename ValueTraits::Ptr;
    using RefMut = decltype(*std::declval<PtrMut>());
    using Ref = decltype(*std::declval<Ptr>());
    using ElementPtr = std::conditional_t<IsConst, Ptr, PtrMut>;
    using ElementRef = std::conditional_t<IsConst, Ref, RefMut>;
    using DifferenceType = typename PointerTraits<NodePtr>::DifferenceType;
    using IterCategory = Category;
    using ValueTraitsPtr = typename ValueTraitsPointers<ValueTraits>::ValueTraitsPtr;
    using ConstValueTraitsPtr = typename ValueTraitsPointers<ValueTraits>::ConstValueTraitsPtr;

    static constexpr bool IS_STATEFUL_VALUE_TRAITS = value_traits_are_stateful<ValueTraits>();
};

template <typename NodePtr, typename StoredPointer, bool IsStatefulValueTraits = true>
struct IiterMembers final {
    constexpr IiterMembers() noexcept = default;

    constexpr IiterMembers(NodePtr const node, StoredPointer const value_traits) noexcept
        : m_node_ptr(node), m_value_traits_ptr(value_traits) {}

    [[nodiscard]]
    constexpr auto get_value_traits() const noexcept -> StoredPointer {
        return m_value_traits_ptr;
    }

    NodePtr m_node_ptr{};
    StoredPointer m_value_traits_ptr{};
};

template <typename NodePtr, typename StoredPointer>
struct IiterMembers<NodePtr, StoredPointer, false> final {
    constexpr IiterMembers() noexcept = default;

    constexpr IiterMembers(NodePtr const node, StoredPointer const) noexcept : m_node_ptr(node) {}

    [[nodiscard]]
    static constexpr auto get_value_traits() noexcept -> StoredPointer {
        return StoredPointer{};
    }

    NodePtr m_node_ptr{};
};
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_IITERATOR_HPP
