/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SET_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SET_HPP 1

#include <ustl/collections/intrusive/rbtree.hpp>
#include <ustl/option.hpp>

#include <iterator>
#include <type_traits>
#include <utility>

namespace ustl::collections::intrusive {
template <typename T, typename... Options>
class Set final {
    using Tree = RBTree<T, Options...>;

  public:
    using Element = typename Tree::Element;
    using Size = typename Tree::Size;
    using KeyCompare = typename Tree::KeyCompare;
    using PtrMut = T *;
    using Ptr = T const *;
    using RefMut = T &;
    using Ref = T const &;
    using IterMut = typename Tree::IterMut;
    using Iter = typename Tree::Iter;
    using ReverseIterMut = typename Tree::ReverseIterMut;
    using ReverseIter = typename Tree::ReverseIter;

    constexpr Set() noexcept = default;

    constexpr explicit Set(KeyCompare compare) noexcept : m_tree(std::move(compare)) {}

    constexpr Set(Set const &) = delete;
    constexpr auto operator=(Set const &) -> Set & = delete;

    [[nodiscard]]
    constexpr auto begin() noexcept -> IterMut {
        return m_tree.begin();
    }
    [[nodiscard]]
    constexpr auto begin() const noexcept -> Iter {
        return m_tree.begin();
    }
    [[nodiscard]]
    constexpr auto cbegin() const noexcept -> Iter {
        return m_tree.cbegin();
    }
    [[nodiscard]]
    constexpr auto end() noexcept -> IterMut {
        return m_tree.end();
    }
    [[nodiscard]]
    constexpr auto end() const noexcept -> Iter {
        return m_tree.end();
    }
    [[nodiscard]]
    constexpr auto cend() const noexcept -> Iter {
        return m_tree.cend();
    }
    [[nodiscard]]
    constexpr auto rbegin() noexcept -> ReverseIterMut {
        return m_tree.rbegin();
    }
    [[nodiscard]]
    constexpr auto rbegin() const noexcept -> ReverseIter {
        return m_tree.rbegin();
    }
    [[nodiscard]]
    constexpr auto crbegin() const noexcept -> ReverseIter {
        return m_tree.crbegin();
    }
    [[nodiscard]]
    constexpr auto rend() noexcept -> ReverseIterMut {
        return m_tree.rend();
    }
    [[nodiscard]]
    constexpr auto rend() const noexcept -> ReverseIter {
        return m_tree.rend();
    }
    [[nodiscard]]
    constexpr auto crend() const noexcept -> ReverseIter {
        return m_tree.crend();
    }
    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_tree.empty();
    }
    [[nodiscard]]
    constexpr auto size() const noexcept -> Size {
        return m_tree.size();
    }
    [[nodiscard]]
    constexpr auto insert(T &value) noexcept -> std::pair<IterMut, bool> {
        return m_tree.insert_unique(value);
    }

    template <std::input_iterator InputIter>
    constexpr auto insert(InputIter first, InputIter last) noexcept -> void {
        m_tree.insert_unique(first, last);
    }

    [[nodiscard]]
    constexpr auto erase(Iter position) noexcept -> IterMut {
        return m_tree.erase(position);
    }
    [[nodiscard]]
    constexpr auto erase(Iter first, Iter last) noexcept -> IterMut {
        return m_tree.erase(first, last);
    }
    constexpr auto erase(T &value) noexcept -> void {
        m_tree.erase(value);
    }
    constexpr auto clear() noexcept -> void {
        m_tree.clear();
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    constexpr auto clear_and_dispose(Disposer disposer) noexcept -> void {
        m_tree.clear_and_dispose(disposer);
    }

    constexpr auto remove_node(T &value) noexcept -> void {
        m_tree.remove_node(value);
    }
    constexpr auto replace_node(Iter position, T &replacement) noexcept -> void {
        m_tree.replace_node(position, replacement);
    }
    constexpr auto swap(Set &other) noexcept -> void {
        m_tree.swap(other.m_tree);
    }
    [[nodiscard]]
    constexpr auto find(T const &value) noexcept -> ustl::Option<T &> {
        return m_tree.find(value);
    }
    [[nodiscard]]
    constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
        return m_tree.find(value);
    }
    [[nodiscard]]
    constexpr auto lower_bound(T const &value) noexcept -> ustl::Option<T &> {
        return m_tree.lower_bound(value);
    }
    [[nodiscard]]
    constexpr auto lower_bound(T const &value) const noexcept -> ustl::Option<T const &> {
        return m_tree.lower_bound(value);
    }
    [[nodiscard]]
    constexpr auto upper_bound(T const &value) noexcept -> ustl::Option<T &> {
        return m_tree.upper_bound(value);
    }
    [[nodiscard]]
    constexpr auto upper_bound(T const &value) const noexcept -> ustl::Option<T const &> {
        return m_tree.upper_bound(value);
    }
    /// Lookups by a key that is not an element, as Multiset has them: a set
    /// whose elements are big or not copyable, keyed on a field of each, has
    /// no element to hand the lookups above. |compare| orders an element and
    /// the key the same way the set's own comparison orders two elements.
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto lower_bound(Key const &key, Compare const &compare) noexcept -> IterMut {
        return m_tree.lower_bound(key, compare);
    }
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto lower_bound(Key const &key, Compare const &compare) const noexcept -> Iter {
        return m_tree.lower_bound(key, compare);
    }
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto upper_bound(Key const &key, Compare const &compare) noexcept -> IterMut {
        return m_tree.upper_bound(key, compare);
    }
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto upper_bound(Key const &key, Compare const &compare) const noexcept -> Iter {
        return m_tree.upper_bound(key, compare);
    }
    [[nodiscard]]
    constexpr auto count(T const &value) const noexcept -> Size {
        return m_tree.count(value);
    }
    [[nodiscard]]
    constexpr auto iterator_to(T &value) noexcept -> IterMut {
        return m_tree.iterator_to(value);
    }
    [[nodiscard]]
    constexpr auto iterator_to(T const &value) const noexcept -> Iter {
        return m_tree.iterator_to(value);
    }

  private:
    Tree m_tree{};
};

template <typename T, typename... Options>
class Multiset {
    using Tree = RBTree<T, Options...>;

  public:
    using Element = typename Tree::Element;
    using Size = typename Tree::Size;
    using KeyCompare = typename Tree::KeyCompare;
    using PtrMut = T *;
    using Ptr = T const *;
    using RefMut = T &;
    using Ref = T const &;
    using IterMut = typename Tree::IterMut;
    using Iter = typename Tree::Iter;

    constexpr Multiset() noexcept = default;

    constexpr explicit Multiset(KeyCompare compare) noexcept : m_tree(std::move(compare)) {}

    constexpr Multiset(Multiset const &) = delete;
    constexpr auto operator=(Multiset const &) -> Multiset & = delete;

    [[nodiscard]]
    constexpr auto begin() noexcept -> IterMut {
        return m_tree.begin();
    }
    [[nodiscard]]
    constexpr auto begin() const noexcept -> Iter {
        return m_tree.begin();
    }
    [[nodiscard]]
    constexpr auto end() noexcept -> IterMut {
        return m_tree.end();
    }
    [[nodiscard]]
    constexpr auto end() const noexcept -> Iter {
        return m_tree.end();
    }
    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_tree.empty();
    }
    [[nodiscard]]
    constexpr auto size() const noexcept -> Size {
        return m_tree.size();
    }
    [[nodiscard]]
    constexpr auto insert(T &value) noexcept -> IterMut {
        return m_tree.insert_equal(value);
    }
    [[nodiscard]]
    constexpr auto insert(Iter position, T &value) noexcept -> IterMut {
        return m_tree.insert_before(position, value);
    }
    [[nodiscard]]
    constexpr auto erase(Iter position) noexcept -> IterMut {
        return m_tree.erase(position);
    }
    [[nodiscard]]
    constexpr auto erase(Iter first, Iter last) noexcept -> IterMut {
        return m_tree.erase(first, last);
    }
    constexpr auto erase(T &value) noexcept -> void {
        m_tree.erase(value);
    }
    constexpr auto clear() noexcept -> void {
        m_tree.clear();
    }
    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    constexpr auto clear_and_dispose(Disposer disposer) noexcept -> void {
        m_tree.clear_and_dispose(disposer);
    }
    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto erase_and_dispose(Iter first, Iter last, Disposer disposer) noexcept -> IterMut {
        return m_tree.erase_and_dispose(first, last, disposer);
    }
    [[nodiscard]]
    constexpr auto find(T const &value) noexcept -> ustl::Option<T &> {
        return m_tree.find(value);
    }
    [[nodiscard]]
    constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
        return m_tree.find(value);
    }
    [[nodiscard]]
    constexpr auto lower_bound(T const &value) noexcept -> ustl::Option<T &> {
        return m_tree.lower_bound(value);
    }
    [[nodiscard]]
    constexpr auto upper_bound(T const &value) noexcept -> ustl::Option<T &> {
        return m_tree.upper_bound(value);
    }
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto lower_bound(Key const &key, Compare const &compare) noexcept -> IterMut {
        return m_tree.lower_bound(key, compare);
    }
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto lower_bound(Key const &key, Compare const &compare) const noexcept -> Iter {
        return m_tree.lower_bound(key, compare);
    }
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto upper_bound(Key const &key, Compare const &compare) noexcept -> IterMut {
        return m_tree.upper_bound(key, compare);
    }
    template <typename Key, typename Compare>
    [[nodiscard]]
    constexpr auto upper_bound(Key const &key, Compare const &compare) const noexcept -> Iter {
        return m_tree.upper_bound(key, compare);
    }
    [[nodiscard]]
    constexpr auto count(T const &value) const noexcept -> Size {
        return m_tree.count(value);
    }
    [[nodiscard]]
    constexpr auto iterator_to(T &value) noexcept -> IterMut {
        return m_tree.iterator_to(value);
    }
    [[nodiscard]]
    constexpr auto iterator_to(T const &value) const noexcept -> Iter {
        return m_tree.iterator_to(value);
    }

    /// Structural navigation, for augmented queries that descend on per-node
    /// aggregates rather than on key order. `end()` means "no such child".
    [[nodiscard]]
    constexpr auto root() noexcept -> IterMut {
        return m_tree.root();
    }
    [[nodiscard]]
    constexpr auto root() const noexcept -> Iter {
        return m_tree.root();
    }
    [[nodiscard]]
    constexpr auto left(Iter position) noexcept -> IterMut {
        return m_tree.left(position);
    }
    [[nodiscard]]
    constexpr auto left(Iter position) const noexcept -> Iter {
        return m_tree.left(position);
    }
    [[nodiscard]]
    constexpr auto right(Iter position) noexcept -> IterMut {
        return m_tree.right(position);
    }
    [[nodiscard]]
    constexpr auto right(Iter position) const noexcept -> Iter {
        return m_tree.right(position);
    }

    /// Re-derives the aggregates on `position`'s root path after the
    /// element's augmented inputs changed in place.
    constexpr auto refresh_augment(Iter position) noexcept -> void {
        m_tree.refresh_augment(position);
    }

  private:
    Tree m_tree{};
};

template <typename T, typename... Options>
using MultiSet = Multiset<T, Options...>;
} // namespace ustl::collections::intrusive

#ifndef USTL_DECLARE_MULTISET
#define USTL_DECLARE_MULTISET(OWNER, ALIAS, ...)                                                   \
    using ALIAS = ::ustl::collections::intrusive::Multiset<OWNER __VA_OPT__(, ) __VA_ARGS__>
#endif

#endif // USTL_COLLECTIONS_INTRUSIVE_SET_HPP
