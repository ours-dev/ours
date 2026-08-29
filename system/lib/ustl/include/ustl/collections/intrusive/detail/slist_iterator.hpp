/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_SLIST_ITERATOR_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_SLIST_ITERATOR_HPP 1

#include <ustl/collections/intrusive/detail/iiterator.hpp>

#include <iterator>

namespace ustl::collections::intrusive::detail {
template <typename TValueTraits, bool IsConst>
class SlistIter;

template <typename TValueTraits, bool LeftConst, bool RightConst>
[[nodiscard]]
constexpr auto operator==(SlistIter<TValueTraits, LeftConst> const &left,
                          SlistIter<TValueTraits, RightConst> const &right) noexcept -> bool;

template <typename TValueTraits, bool LeftConst, bool RightConst>
[[nodiscard]]
constexpr auto operator!=(SlistIter<TValueTraits, LeftConst> const &left,
                          SlistIter<TValueTraits, RightConst> const &right) noexcept -> bool;

template <typename TValueTraits, bool IsConst>
class SlistIter final {
    using Types = Iiter<TValueTraits, IsConst, std::forward_iterator_tag>;

  public:
    using ValueTraits = typename Types::ValueTraits;
    using NodeTraits = typename Types::NodeTraits;
    using NodePtr = typename Types::NodePtr;
    using ConstValueTraitsPtr = typename Types::ConstValueTraitsPtr;
    using DifferenceType = typename Types::DifferenceType;
    using Element = typename Types::Element;
    using PtrMut = typename Types::PtrMut;
    using Ptr = typename Types::Ptr;
    using RefMut = typename Types::RefMut;
    using Ref = typename Types::Ref;
    using ElementPtr = typename Types::ElementPtr;
    using ElementRef = typename Types::ElementRef;
    using IterCategory = typename Types::IterCategory;
    using difference_type = DifferenceType;
    /// std::iterator_traits only takes effect when all five names are
    /// present; missing one silently makes the whole trait empty, which is
    /// how std::advance ends up unable to find iterator_category.
    using value_type = Element;
    using pointer = PtrMut;
    using reference = RefMut;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = IterCategory;

    constexpr SlistIter() noexcept = default;

    constexpr SlistIter(NodePtr const node, ConstValueTraitsPtr const value_traits) noexcept
        : m_members(node, value_traits) {}

    constexpr explicit SlistIter(NodePtr const node) noexcept
        requires(!Types::IS_STATEFUL_VALUE_TRAITS)
        : m_members(node, ConstValueTraitsPtr{}) {}

    template <bool OtherIsConst>
        requires(IsConst && !OtherIsConst)
    constexpr SlistIter(SlistIter<ValueTraits, OtherIsConst> const &other) noexcept
        : m_members(other.pointed_node(), other.get_value_traits()) {}

    constexpr SlistIter(SlistIter const &) noexcept = default;
    constexpr auto operator=(SlistIter const &) noexcept -> SlistIter & = default;

    constexpr auto operator=(NodePtr const node) noexcept -> SlistIter & {
        m_members.m_node_ptr = node;
        return *this;
    }

    [[nodiscard]]
    constexpr auto pointed_node() const noexcept -> NodePtr {
        return m_members.m_node_ptr;
    }

    [[nodiscard]]
    constexpr auto get_value_traits() const noexcept -> ConstValueTraitsPtr {
        return m_members.get_value_traits();
    }

    [[nodiscard]]
    constexpr auto operator!() const noexcept -> bool {
        return pointed_node() == NodePtr{};
    }

    constexpr auto operator++() noexcept -> SlistIter & {
        m_members.m_node_ptr = NodeTraits::get_next(m_members.m_node_ptr);
        return *this;
    }

    constexpr auto operator++(int) noexcept -> SlistIter {
        const auto result = *this;
        ++(*this);
        return result;
    }

    [[nodiscard]]
    constexpr auto operator*() const noexcept -> ElementRef {
        return *operator->();
    }

    [[nodiscard]]
    constexpr auto operator->() const noexcept -> ElementPtr {
        if constexpr (Types::IS_STATEFUL_VALUE_TRAITS) {
            return get_value_traits()->to_value_ptr(pointed_node());
        } else {
            return ValueTraits::to_value_ptr(pointed_node());
        }
    }

    [[nodiscard]]
    constexpr auto unconst() const noexcept -> SlistIter<ValueTraits, false> {
        return SlistIter<ValueTraits, false>(pointed_node(), get_value_traits());
    }

  private:
    template <typename OtherValueTraits, bool LeftConst, bool RightConst>
    friend constexpr auto operator==(SlistIter<OtherValueTraits, LeftConst> const &left,
                                     SlistIter<OtherValueTraits, RightConst> const &right) noexcept
        -> bool;

    template <typename OtherValueTraits, bool LeftConst, bool RightConst>
    friend constexpr auto operator!=(SlistIter<OtherValueTraits, LeftConst> const &left,
                                     SlistIter<OtherValueTraits, RightConst> const &right) noexcept
        -> bool;

    IiterMembers<NodePtr, ConstValueTraitsPtr, Types::IS_STATEFUL_VALUE_TRAITS> m_members{};
};

template <typename TValueTraits, bool LeftConst, bool RightConst>
[[nodiscard]]
constexpr auto operator==(SlistIter<TValueTraits, LeftConst> const &left,
                          SlistIter<TValueTraits, RightConst> const &right) noexcept -> bool {
    return left.pointed_node() == right.pointed_node();
}

template <typename TValueTraits, bool LeftConst, bool RightConst>
[[nodiscard]]
constexpr auto operator!=(SlistIter<TValueTraits, LeftConst> const &left,
                          SlistIter<TValueTraits, RightConst> const &right) noexcept -> bool {
    return !(left == right);
}
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_SLIST_ITERATOR_HPP
