/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_LIST_HPP
#define USTL_COLLECTIONS_INTRUSIVE_LIST_HPP 1

#include <array>
#include <functional>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include <cstddef>

#include <ustl/collections/error.hpp>
#include <ustl/collections/intrusive/circular_list_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/list_iterator.hpp>
#include <ustl/collections/intrusive/detail/reverse_iter.hpp>
#include <ustl/collections/intrusive/detail/value_traits_access.hpp>
#include <ustl/collections/intrusive/list_hook.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace ustl::collections::intrusive {
template <typename T, typename... Options>
class List final {
    struct Defaults: HookDefaults {
        using ProtoValueTraits = ListBaseHook<>;
    };

    using PackedOptions = typename PackOptions<Defaults, Options...>::Type;
    using RawValueTraits = typename PackedOptions::ProtoValueTraits;
    using ValueTraits = detail::ContainerValueTraits<T, RawValueTraits>;
    using NodeTraits = typename ValueTraits::NodeTraits;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using Algorithms = CircularListAlgorithms<NodeTraits>;

  public:
    using Element = T;
    using Size = std::size_t;
    using DifferenceType = std::ptrdiff_t;
    using PtrMut = T *;
    using Ptr = T const *;
    using RefMut = T &;
    using Ref = T const &;
    using IterMut = detail::ListIter<ValueTraits, false>;
    using Iter = detail::ListIter<ValueTraits, true>;
    using ReverseIterMut = detail::ReverseIter<IterMut>;
    using ReverseIter = detail::ReverseIter<Iter>;

    constexpr List() noexcept {
        Algorithms::init_header(header());
    }

    template <std::input_iterator InputIter>
    constexpr List(InputIter first, InputIter last) noexcept : List() {
        insert(end(), first, last);
    }

    constexpr List(List const &) = delete;
    constexpr auto operator=(List const &) -> List & = delete;

    constexpr List(List &&other) noexcept : List() {
        swap(other);
    }

    constexpr auto operator=(List &&other) noexcept -> List & {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }

    constexpr ~List() noexcept {
        clear();
    }

    [[nodiscard]]
    constexpr auto begin() noexcept -> IterMut {
        return IterMut(NodeTraits::get_next(header()));
    }

    [[nodiscard]]
    constexpr auto begin() const noexcept -> Iter {
        return Iter(NodeTraits::get_next(header()));
    }

    [[nodiscard]]
    constexpr auto cbegin() const noexcept -> Iter {
        return begin();
    }

    [[nodiscard]]
    constexpr auto end() noexcept -> IterMut {
        return IterMut(header());
    }

    [[nodiscard]]
    constexpr auto end() const noexcept -> Iter {
        return Iter(header());
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
        return Algorithms::is_empty(header());
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> Size {
        return m_size;
    }

    [[nodiscard]]
    constexpr auto front() noexcept -> ustl::Option<T &> {
        const auto node = NodeTraits::get_next(header());
        return node == header() ? ustl::NONE : ustl::some_ref(*ValueTraits::to_value_ptr(node));
    }

    [[nodiscard]]
    constexpr auto front() const noexcept -> ustl::Option<T const &> {
        const auto node = NodeTraits::get_next(header());
        return node == header() ? ustl::NONE : ustl::some_ref(*ValueTraits::to_value_ptr(node));
    }

    [[nodiscard]]
    constexpr auto back() noexcept -> ustl::Option<T &> {
        const auto node = NodeTraits::get_previous(header());
        return node == header() ? ustl::NONE : ustl::some_ref(*ValueTraits::to_value_ptr(node));
    }

    [[nodiscard]]
    constexpr auto back() const noexcept -> ustl::Option<T const &> {
        const auto node = NodeTraits::get_previous(header());
        return node == header() ? ustl::NONE : ustl::some_ref(*ValueTraits::to_value_ptr(node));
    }

    constexpr auto push_front(T &value) noexcept -> void {
        static_cast<void>(insert(begin(), value));
    }

    constexpr auto push_back(T &value) noexcept -> void {
        static_cast<void>(insert(end(), value));
    }

    constexpr auto pop_front() noexcept -> void {
        detail::require(!empty());
        static_cast<void>(erase(begin()));
    }

    constexpr auto pop_back() noexcept -> void {
        detail::require(!empty());
        auto position = end();
        --position;
        static_cast<void>(erase(position));
    }

    [[nodiscard]]
    constexpr auto insert(Iter position, T &value) noexcept -> IterMut {
        const auto node = ValueTraits::to_node_ptr(value);
        detail::require(Algorithms::unique(node));
        Algorithms::link_before(position.pointed_node(), node);
        ++m_size;
        return IterMut(node);
    }

    template <std::input_iterator InputIter>
    constexpr auto insert(Iter position, InputIter first, InputIter last) noexcept -> void {
        while (first != last) {
            static_cast<void>(insert(position, *first));
            ++first;
        }
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
        const auto next = NodeTraits::get_next(node);
        static_cast<void>(Algorithms::unlink(node));
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

    constexpr auto swap(List &other) noexcept -> void {
        Algorithms::swap_nodes(header(), other.header());
        std::swap(m_size, other.m_size);
    }

    constexpr auto shift_backwards(Size const count = 1U) noexcept -> void {
        Algorithms::move_forward(header(), count);
    }

    constexpr auto shift_forward(Size const count = 1U) noexcept -> void {
        Algorithms::move_backwards(header(), count);
    }

    constexpr auto splice(Iter position, List &source) noexcept -> void {
        if (this == &source || source.empty()) {
            return;
        }
        Algorithms::transfer(position.pointed_node(), NodeTraits::get_next(source.header()),
                             source.header());
        m_size += source.m_size;
        source.m_size = 0U;
    }

    constexpr auto splice(Iter position, List &source, Iter element) noexcept -> void {
        const auto node = element.pointed_node();
        detail::require(node != source.header());
        Algorithms::transfer(position.pointed_node(), node);
        if (this != &source) {
            ++m_size;
            --source.m_size;
        }
    }

    constexpr auto splice(Iter position, List &source, Iter first, Iter last) noexcept -> void {
        splice(position, source, first, last,
               Algorithms::distance(first.pointed_node(), last.pointed_node()));
    }

    constexpr auto splice(Iter position, List &source, Iter first, Iter last,
                          Size const count) noexcept -> void {
        if (first == last) {
            return;
        }
        Algorithms::transfer(position.pointed_node(), first.pointed_node(), last.pointed_node());
        if (this != &source) {
            m_size += count;
            source.m_size -= count;
        }
    }

    template <typename Compare>
        requires(std::is_nothrow_invocable_r_v<bool, Compare &, T const &, T const &>)
    constexpr auto merge(List &source, Compare compare) noexcept -> void {
        detail::require(this != &source);
        auto position = begin();
        while (!source.empty()) {
            const auto source_position = source.begin();
            if (position == end() || std::invoke(compare, *source_position, *position)) {
                splice(position, source, source_position);
            } else {
                ++position;
            }
        }
    }

    constexpr auto merge(List &source) noexcept -> void
        requires(std::is_nothrow_invocable_r_v<bool, std::less<>, T const &, T const &>)
    {
        merge(source, std::less<>{});
    }

    template <typename Compare>
        requires(std::is_nothrow_invocable_r_v<bool, Compare &, T const &, T const &>)
    constexpr auto sort(Compare compare) noexcept -> void {
        if (m_size < 2U) {
            return;
        }

        List carry{};
        std::array<List, std::numeric_limits<Size>::digits> bins{};
        auto filled = Size{};
        while (!empty()) {
            carry.splice(carry.begin(), *this, begin());
            auto index = Size{};
            while (index < filled && !bins[index].empty()) {
                bins[index].merge(carry, compare);
                carry.swap(bins[index]);
                ++index;
            }
            carry.swap(bins[index]);
            if (index == filled) {
                ++filled;
            }
        }
        for (auto index = Size{1}; index < filled; ++index) {
            bins[index].merge(bins[index - 1U], compare);
        }
        swap(bins[filled - 1U]);
    }

    constexpr auto sort() noexcept -> void
        requires(std::is_nothrow_invocable_r_v<bool, std::less<>, T const &, T const &>)
    {
        sort(std::less<>{});
    }

    constexpr auto reverse() noexcept -> void {
        Algorithms::reverse(header());
    }

    template <typename Predicate>
        requires(std::is_nothrow_invocable_r_v<bool, Predicate &, T const &>)
    [[nodiscard]]
    constexpr auto remove_if(Predicate predicate) noexcept -> Size {
        return remove_and_dispose_if(predicate, [](T *) noexcept {});
    }

    template <typename Predicate, typename Disposer>
        requires(std::is_nothrow_invocable_r_v<bool, Predicate &, T const &> &&
                 std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto remove_and_dispose_if(Predicate predicate, Disposer disposer) noexcept -> Size {
        auto removed = Size{};
        auto current = begin();
        while (current != end()) {
            if (std::invoke(predicate, *current)) {
                current = erase_and_dispose(current, disposer);
                ++removed;
            } else {
                ++current;
            }
        }
        return removed;
    }

    [[nodiscard]]
    constexpr auto remove(T const &value) noexcept -> Size
        requires(std::is_nothrow_invocable_r_v<bool, std::equal_to<>, T const &, T const &>)
    {
        return remove_if(
            [&value](T const &candidate) noexcept { return std::equal_to<>{}(candidate, value); });
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_r_v<bool, std::equal_to<>, T const &, T const &> &&
                 std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto remove_and_dispose(T const &value, Disposer disposer) noexcept -> Size {
        return remove_and_dispose_if(
            [&value](T const &candidate) noexcept { return std::equal_to<>{}(candidate, value); },
            disposer);
    }

    template <typename Predicate>
        requires(std::is_nothrow_invocable_r_v<bool, Predicate &, T const &, T const &>)
    [[nodiscard]]
    constexpr auto unique(Predicate predicate) noexcept -> Size {
        return unique_and_dispose(predicate, [](T *) noexcept {});
    }

    [[nodiscard]]
    constexpr auto unique() noexcept -> Size
        requires(std::is_nothrow_invocable_r_v<bool, std::equal_to<>, T const &, T const &>)
    {
        return unique(std::equal_to<>{});
    }

    template <typename Predicate, typename Disposer>
        requires(std::is_nothrow_invocable_r_v<bool, Predicate &, T const &, T const &> &&
                 std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto unique_and_dispose(Predicate predicate, Disposer disposer) noexcept -> Size {
        if (m_size < 2U) {
            return 0U;
        }

        auto removed = Size{};
        auto current = begin();
        auto next = current;
        ++next;
        while (next != end()) {
            if (std::invoke(predicate, *current, *next)) {
                next = erase_and_dispose(next, disposer);
                ++removed;
            } else {
                current = next;
                ++next;
            }
        }
        return removed;
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_r_v<bool, std::equal_to<>, T const &, T const &> &&
                 std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto unique_and_dispose(Disposer disposer) noexcept -> Size {
        return unique_and_dispose(std::equal_to<>{}, disposer);
    }

    template <typename Cloner, typename Disposer>
        requires(std::is_nothrow_invocable_v<Cloner &, T const &> &&
                 std::same_as<std::remove_cvref_t<std::invoke_result_t<Cloner &, T const &>>,
                              ustl::Result<T *, ustl::collections::Error>> &&
                 std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto clone_from(List const &source, Cloner cloner, Disposer disposer) noexcept
        -> ustl::Result<void, ustl::collections::Error> {
        detail::require(this != &source);
        clear_and_dispose(disposer);
        for (auto const &source_value: source) {
            auto clone = std::invoke(cloner, source_value);
            if (clone.is_err()) {
                const auto error = clone.error();
                clear_and_dispose(disposer);
                return ustl::err(error);
            }
            detail::require(clone.value() != nullptr);
            push_back(*clone.value());
        }
        return ustl::ok();
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
    Size m_size{};
};
} // namespace ustl::collections::intrusive

#ifndef USTL_DECLARE_LIST
#define USTL_DECLARE_LIST(OWNER, ALIAS, ...)                                                       \
    using ALIAS = ::ustl::collections::intrusive::List<OWNER __VA_OPT__(, ) __VA_ARGS__>
#endif

#ifndef USTL_DECLARE_LIST_MEMBER
#define USTL_DECLARE_LIST_MEMBER(OWNER, ALIAS, HOOK, ...)                                          \
    using ALIAS = ::ustl::collections::intrusive::List<                                            \
        OWNER,                                                                                     \
        ::ustl::collections::intrusive::MemberHook<OWNER, decltype(OWNER::HOOK), &OWNER::HOOK>     \
            __VA_OPT__(, ) __VA_ARGS__>
#endif

#endif // USTL_COLLECTIONS_INTRUSIVE_LIST_HPP
