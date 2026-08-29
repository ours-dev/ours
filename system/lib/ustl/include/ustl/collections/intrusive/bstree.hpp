/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_BSTREE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_BSTREE_HPP 1

#include <ustl/collections/intrusive/bs_set_hook.hpp>
#include <ustl/collections/intrusive/bstree_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/key_node_ptr_compare.hpp>
#include <ustl/collections/intrusive/detail/reverse_iter.hpp>
#include <ustl/collections/intrusive/detail/tree_iterator.hpp>
#include <ustl/collections/intrusive/detail/value_traits_access.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/option.hpp>

#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename T, typename... Options>
class Bstree final {
    struct Defaults: HookDefaults {
        using ProtoValueTraits = BsSetBaseHook<>;
        using Compare = std::less<T>;
    };

    using PackedOptions = typename PackOptions<Defaults, Options...>::Type;
    using RawValueTraits = typename PackedOptions::ProtoValueTraits;
    using ValueTraits = detail::ContainerValueTraits<T, RawValueTraits>;
    using ValueAccess = detail::ValueTraitsAccess<T, RawValueTraits>;
    using NodeTraits = typename ValueTraits::NodeTraits;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using Algorithms = BstreeAlgorithms<NodeTraits>;
    using NodeCompare = detail::KeyNodePtrCompare<typename PackedOptions::Compare, ValueAccess>;

  public:
    using Element = T;
    using Size = std::size_t;
    using DifferenceType = std::ptrdiff_t;
    using KeyCompare = typename PackedOptions::Compare;
    using InsertCommitData = typename Algorithms::InsertCommitData;
    using IterMut = detail::TreeIter<ValueTraits, false>;
    using Iter = detail::TreeIter<ValueTraits, true>;
    using ReverseIterMut = detail::ReverseIter<IterMut>;
    using ReverseIter = detail::ReverseIter<Iter>;

    static_assert(std::is_nothrow_invocable_r_v<bool, KeyCompare const &, T const &, T const &>);

    constexpr Bstree() noexcept {
        Algorithms::init_header(header());
    }

    constexpr explicit Bstree(KeyCompare compare) noexcept : m_compare(std::move(compare)) {
        Algorithms::init_header(header());
    }

    constexpr Bstree(Bstree const &) = delete;
    constexpr auto operator=(Bstree const &) -> Bstree & = delete;

    constexpr Bstree(Bstree &&other) noexcept : Bstree() {
        swap(other);
    }

    constexpr auto operator=(Bstree &&other) noexcept -> Bstree & {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }

    constexpr ~Bstree() noexcept {
        clear();
    }

    [[nodiscard]]
    constexpr auto begin() noexcept -> IterMut {
        return IterMut(Algorithms::begin_node(header()));
    }

    [[nodiscard]]
    constexpr auto begin() const noexcept -> Iter {
        return Iter(Algorithms::begin_node(header()));
    }

    [[nodiscard]]
    constexpr auto cbegin() const noexcept -> Iter {
        return begin();
    }

    [[nodiscard]]
    constexpr auto end() noexcept -> IterMut {
        return IterMut(Algorithms::end_node(header()));
    }

    [[nodiscard]]
    constexpr auto end() const noexcept -> Iter {
        return Iter(Algorithms::end_node(header()));
    }

    [[nodiscard]]
    constexpr auto cend() const noexcept -> Iter {
        return end();
    }

    [[nodiscard]]
    constexpr auto rbegin() noexcept -> ReverseIterMut {
        return ReverseIterMut(end());
    }

    [[nodiscard]]
    constexpr auto rbegin() const noexcept -> ReverseIter {
        return ReverseIter(end());
    }

    [[nodiscard]]
    constexpr auto crbegin() const noexcept -> ReverseIter {
        return rbegin();
    }

    [[nodiscard]]
    constexpr auto rend() noexcept -> ReverseIterMut {
        return ReverseIterMut(begin());
    }

    [[nodiscard]]
    constexpr auto rend() const noexcept -> ReverseIter {
        return ReverseIter(begin());
    }

    [[nodiscard]]
    constexpr auto crend() const noexcept -> ReverseIter {
        return rend();
    }

    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_size == 0U;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> Size {
        return m_size;
    }

    [[nodiscard]]
    constexpr auto insert_unique(T &value) noexcept -> std::pair<IterMut, bool> {
        InsertCommitData commit_data{};
        const auto node = ValueTraits::to_node_ptr(value);
        const auto [existing, inserted] =
            Algorithms::insert_unique_check(header(), node, NodeCompare{&m_compare}, commit_data);
        if (!inserted) {
            return {IterMut(existing), false};
        }
        Algorithms::insert_unique_commit(header(), node, commit_data);
        ++m_size;
        return {IterMut(node), true};
    }

    template <std::input_iterator InputIter>
    constexpr auto insert_unique(InputIter first, InputIter last) noexcept -> void {
        while (first != last) {
            static_cast<void>(insert_unique(*first));
            ++first;
        }
    }

    [[nodiscard]]
    constexpr auto insert_equal(T &value) noexcept -> IterMut {
        const auto node = ValueTraits::to_node_ptr(value);
        detail::require(Algorithms::unique(node));
        Algorithms::insert_equal_upper_bound(header(), node, NodeCompare{&m_compare});
        ++m_size;
        return IterMut(node);
    }

    template <std::input_iterator InputIter>
    constexpr auto insert_equal(InputIter first, InputIter last) noexcept -> void {
        while (first != last) {
            static_cast<void>(insert_equal(*first));
            ++first;
        }
    }

    [[nodiscard]]
    constexpr auto insert_before(Iter position, T &value) noexcept -> IterMut {
        const auto node = ValueTraits::to_node_ptr(value);
        detail::require(Algorithms::unique(node));
        Algorithms::insert_before(header(), position.pointed_node(), node);
        ++m_size;
        return IterMut(node);
    }

    constexpr auto push_back(T &value) noexcept -> void {
        static_cast<void>(insert_before(end(), value));
    }

    constexpr auto push_front(T &value) noexcept -> void {
        static_cast<void>(insert_before(begin(), value));
    }

    [[nodiscard]]
    constexpr auto erase(Iter position) noexcept -> IterMut {
        return erase_and_dispose(position, [](T *) noexcept {});
    }

    [[nodiscard]]
    constexpr auto erase(Iter first, Iter last) noexcept -> IterMut {
        return erase_and_dispose(first, last, [](T *) noexcept {});
    }

    constexpr auto erase(T &value) noexcept -> void {
        const auto node = ValueTraits::to_node_ptr(value);
        if (!Algorithms::unique(node)) {
            static_cast<void>(erase(IterMut(node)));
        }
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto erase_and_dispose(Iter position, Disposer disposer) noexcept -> IterMut {
        const auto node = position.pointed_node();
        detail::require(node != header());
        const auto next = Algorithms::next_node(node);
        static_cast<void>(Algorithms::erase(header(), node));
        Algorithms::init(node);
        --m_size;
        std::invoke(disposer, ValueTraits::to_value_ptr(node));
        return IterMut(next);
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto erase_and_dispose(Iter first, Iter last, Disposer disposer) noexcept -> IterMut {
        auto current = IterMut(first.pointed_node());
        while (current != last) {
            current = erase_and_dispose(current, disposer);
        }
        return current;
    }

    constexpr auto clear() noexcept -> void {
        static_cast<void>(erase(begin(), end()));
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    constexpr auto clear_and_dispose(Disposer disposer) noexcept -> void {
        static_cast<void>(erase_and_dispose(begin(), end(), disposer));
    }

    constexpr auto remove_node(T &value) noexcept -> void {
        erase(value);
    }

    constexpr auto replace_node(Iter position, T &replacement) noexcept -> void {
        const auto node = position.pointed_node();
        const auto new_node = ValueTraits::to_node_ptr(replacement);
        detail::require(node != header());
        detail::require(Algorithms::unique(new_node));
        Algorithms::replace_node(node, header(), new_node);
        Algorithms::init(node);
    }

    constexpr auto swap(Bstree &other) noexcept -> void {
        Algorithms::swap_tree(header(), other.header());
        std::swap(m_compare, other.m_compare);
        std::swap(m_size, other.m_size);
    }

    [[nodiscard]]
    constexpr auto find(T const &value) noexcept -> ustl::Option<T &> {
        const auto node = Algorithms::find(header(), value, NodeCompare{&m_compare});
        return node == header() ? ustl::NONE : ustl::some(*ValueTraits::to_value_ptr(node));
    }

    [[nodiscard]]
    constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
        const auto node = Algorithms::find(header(), value, NodeCompare{&m_compare});
        return node == header()
                   ? ustl::NONE
                   : ustl::some(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
    }

    [[nodiscard]]
    constexpr auto lower_bound(T const &value) noexcept -> ustl::Option<T &> {
        const auto node = Algorithms::lower_bound(header(), value, NodeCompare{&m_compare});
        return node == header() ? ustl::NONE : ustl::some(*ValueTraits::to_value_ptr(node));
    }

    [[nodiscard]]
    constexpr auto lower_bound(T const &value) const noexcept -> ustl::Option<T const &> {
        const auto node = Algorithms::lower_bound(header(), value, NodeCompare{&m_compare});
        return node == header()
                   ? ustl::NONE
                   : ustl::some(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
    }

    [[nodiscard]]
    constexpr auto upper_bound(T const &value) noexcept -> ustl::Option<T &> {
        const auto node = Algorithms::upper_bound(header(), value, NodeCompare{&m_compare});
        return node == header() ? ustl::NONE : ustl::some(*ValueTraits::to_value_ptr(node));
    }

    [[nodiscard]]
    constexpr auto upper_bound(T const &value) const noexcept -> ustl::Option<T const &> {
        const auto node = Algorithms::upper_bound(header(), value, NodeCompare{&m_compare});
        return node == header()
                   ? ustl::NONE
                   : ustl::some(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
    }

    [[nodiscard]]
    constexpr auto count(T const &value) const noexcept -> Size {
        return Algorithms::count(header(), value, NodeCompare{&m_compare});
    }

    [[nodiscard]]
    static constexpr auto s_iterator_to(T &value) noexcept -> IterMut {
        const auto node = ValueTraits::to_node_ptr(value);
        detail::require(!Algorithms::inited(node));
        return IterMut(node);
    }

    [[nodiscard]]
    static constexpr auto s_iterator_to(T const &value) noexcept -> Iter {
        const auto node = ValueTraits::to_node_ptr(value);
        detail::require(!Algorithms::inited(node));
        return Iter(node_from_const(node));
    }

    [[nodiscard]]
    constexpr auto iterator_to(T &value) noexcept -> IterMut {
        return s_iterator_to(value);
    }

    [[nodiscard]]
    constexpr auto iterator_to(T const &value) const noexcept -> Iter {
        return s_iterator_to(value);
    }

  private:
    [[nodiscard]]
    constexpr auto header() noexcept -> NodePtr {
        return PointerTraits<NodePtr>::pointer_to(m_header);
    }

    [[nodiscard]]
    constexpr auto header() const noexcept -> NodePtr {
        return PointerTraits<NodePtr>::pointer_to(const_cast<Node &>(m_header));
    }

    [[nodiscard]]
    static constexpr auto node_from_const(typename NodeTraits::ConstNodePtr const node) noexcept
        -> NodePtr {
        return PointerTraits<NodePtr>::pointer_to(const_cast<Node &>(
            *PointerTraits<typename NodeTraits::ConstNodePtr>::to_raw_pointer(node)));
    }

    Node m_header{};
    KeyCompare m_compare{};
    Size m_size{};
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_BSTREE_HPP
