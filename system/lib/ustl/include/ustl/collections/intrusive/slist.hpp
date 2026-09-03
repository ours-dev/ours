/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SLIST_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SLIST_HPP 1

#include <ustl/collections/error.hpp>
#include <ustl/collections/intrusive/circular_slist_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/slist_iterator.hpp>
#include <ustl/collections/intrusive/detail/value_traits_access.hpp>
#include <ustl/collections/intrusive/linear_slist_algorithms.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/slist_hook.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

#include <array>
#include <functional>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename T, typename... Options>
class Slist final {
    struct Defaults: HookDefaults {
        using ProtoValueTraits = SlistBaseHook<>;
        static constexpr bool CONSTANT_TIME_SIZE = true;
        static constexpr bool LINEAR = false;
        static constexpr bool CACHE_LAST = false;
    };

    using PackedOptions = typename PackOptions<Defaults, Options...>::Type;
    using RawValueTraits = typename PackedOptions::ProtoValueTraits;
    using ValueTraits = detail::ContainerValueTraits<T, RawValueTraits>;
    using NodeTraits = typename ValueTraits::NodeTraits;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using Algorithms = std::conditional_t<PackedOptions::LINEAR, LinearSlistAlgorithms<NodeTraits>,
                                          CircularSlistAlgorithms<NodeTraits>>;

    static_assert(!(PackedOptions::CONSTANT_TIME_SIZE &&
                    ValueTraits::LINK_MODE == LinkModeType::AutoUnlink));
    static_assert(!(PackedOptions::LINEAR && ValueTraits::LINK_MODE == LinkModeType::AutoUnlink));
    static_assert(!(PackedOptions::CACHE_LAST &&
                    ValueTraits::LINK_MODE == LinkModeType::AutoUnlink));

  public:
    using Element = T;
    using Size = std::size_t;
    using DifferenceType = std::ptrdiff_t;
    using PtrMut = T *;
    using Ptr = T const *;
    using RefMut = T &;
    using Ref = T const &;
    using IterMut = detail::SlistIter<ValueTraits, false>;
    using Iter = detail::SlistIter<ValueTraits, true>;

    constexpr Slist() noexcept {
        reset();
    }

    template <std::input_iterator InputIter>
    constexpr Slist(InputIter first, InputIter last) noexcept : Slist() {
        insert_after(before_begin(), first, last);
    }

    constexpr Slist(Slist const &) = delete;
    constexpr auto operator=(Slist const &) -> Slist & = delete;

    constexpr Slist(Slist &&other) noexcept : Slist() {
        swap(other);
    }

    constexpr auto operator=(Slist &&other) noexcept -> Slist & {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }

    constexpr ~Slist() noexcept {
        clear();
    }

    [[nodiscard]]
    constexpr auto before_begin() noexcept -> IterMut {
        return IterMut(header());
    }

    [[nodiscard]]
    constexpr auto before_begin() const noexcept -> Iter {
        return Iter(header());
    }

    [[nodiscard]]
    constexpr auto cbefore_begin() const noexcept -> Iter {
        return before_begin();
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
        return IterMut(end_node());
    }

    [[nodiscard]]
    constexpr auto end() const noexcept -> Iter {
        return Iter(end_node());
    }

    [[nodiscard]]
    constexpr auto cend() const noexcept -> Iter {
        return end();
    }

    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return Algorithms::is_empty(header());
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> Size {
        if constexpr (PackedOptions::CONSTANT_TIME_SIZE) {
            return m_size;
        } else {
            return Algorithms::distance(NodeTraits::get_next(header()), end_node());
        }
    }

    [[nodiscard]]
    constexpr auto front() noexcept -> ustl::Option<T &> {
        const auto node = NodeTraits::get_next(header());
        return node == end_node() ? ustl::NONE : ustl::some_ref(*ValueTraits::to_value_ptr(node));
    }

    [[nodiscard]]
    constexpr auto front() const noexcept -> ustl::Option<T const &> {
        const auto node = NodeTraits::get_next(header());
        return node == end_node() ? ustl::NONE : ustl::some_ref(*ValueTraits::to_value_ptr(node));
    }

    constexpr auto push_front(T &value) noexcept -> void {
        static_cast<void>(insert_after(before_begin(), value));
    }

    constexpr auto push_back(T &value) noexcept -> void {
        static_cast<void>(insert_after(IterMut(last_node()), value));
    }

    [[nodiscard]]
    constexpr auto pop_front() noexcept -> ustl::Option<T &> {
        const auto value = front();
        if (value.is_none()) {
            return ustl::NONE;
        }
        static_cast<void>(erase_after(before_begin()));
        return value;
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    constexpr auto pop_front_and_dispose(Disposer disposer) noexcept -> void {
        detail::require(!empty());
        static_cast<void>(erase_after_and_dispose(before_begin(), disposer));
    }

    [[nodiscard]]
    constexpr auto insert_after(Iter position, T &value) noexcept -> IterMut {
        const auto node = ValueTraits::to_node_ptr(value);
        detail::require(Algorithms::unique(node));
        Algorithms::link_after(position.pointed_node(), node);
        increment_size();
        refresh_last();
        return IterMut(node);
    }

    template <std::input_iterator InputIter>
    constexpr auto insert_after(Iter position, InputIter first, InputIter last) noexcept -> void {
        auto previous = IterMut(position.pointed_node());
        while (first != last) {
            previous = insert_after(previous, *first);
            ++first;
        }
    }

    [[nodiscard]]
    constexpr auto insert(Iter position, T &value) noexcept -> IterMut {
        return insert_after(previous(position), value);
    }

    template <std::input_iterator InputIter>
    constexpr auto insert(Iter position, InputIter first, InputIter last) noexcept -> void {
        insert_after(previous(position), first, last);
    }

    [[nodiscard]]
    constexpr auto erase_after(Iter position) noexcept -> IterMut {
        return erase_after_and_dispose(position, [](T *) noexcept {});
    }

    [[nodiscard]]
    constexpr auto erase_after(Iter before_first, Iter last) noexcept -> IterMut {
        return erase_after_and_dispose(before_first, last, [](T *) noexcept {});
    }

    [[nodiscard]]
    constexpr auto erase(Iter position) noexcept -> IterMut {
        return erase_after(previous(position));
    }

    [[nodiscard]]
    constexpr auto erase(Iter first, Iter last) noexcept -> IterMut {
        if (first == last) {
            return IterMut(last.pointed_node());
        }
        return erase_after(previous(first), last);
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto erase_after_and_dispose(Iter position, Disposer disposer) noexcept -> IterMut {
        const auto previous_node = position.pointed_node();
        const auto node = NodeTraits::get_next(previous_node);
        detail::require(node != end_node());
        const auto next = NodeTraits::get_next(node);
        Algorithms::unlink_after(previous_node);
        Algorithms::init(node);
        decrement_size();
        refresh_last();
        std::invoke(disposer, ValueTraits::to_value_ptr(node));
        return IterMut(next);
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto erase_after_and_dispose(Iter before_first, Iter last, Disposer disposer) noexcept
        -> IterMut {
        auto current = IterMut(NodeTraits::get_next(before_first.pointed_node()));
        while (current != last) {
            current = erase_after_and_dispose(before_first, disposer);
        }
        return current;
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto erase_and_dispose(Iter position, Disposer disposer) noexcept -> IterMut {
        return erase_after_and_dispose(previous(position), disposer);
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    [[nodiscard]]
    constexpr auto erase_and_dispose(Iter first, Iter last, Disposer disposer) noexcept -> IterMut {
        if (first == last) {
            return IterMut(last.pointed_node());
        }
        return erase_after_and_dispose(previous(first), last, disposer);
    }

    constexpr auto clear() noexcept -> void {
        static_cast<void>(erase_after(before_begin(), end()));
    }

    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, T *>)
    constexpr auto clear_and_dispose(Disposer disposer) noexcept -> void {
        static_cast<void>(erase_after_and_dispose(before_begin(), end(), disposer));
    }

    constexpr auto swap(Slist &other) noexcept -> void {
        Algorithms::swap_nodes(header(), other.header());
        if constexpr (PackedOptions::CONSTANT_TIME_SIZE) {
            std::swap(m_size, other.m_size);
        }
        refresh_last();
        other.refresh_last();
    }

    [[nodiscard]]
    constexpr auto previous(IterMut position) noexcept -> IterMut {
        return IterMut(Algorithms::get_previous_node(header(), position.pointed_node()));
    }

    [[nodiscard]]
    constexpr auto previous(Iter position) const noexcept -> Iter {
        return Iter(Algorithms::get_previous_node(header(), position.pointed_node()));
    }

    [[nodiscard]]
    constexpr auto previous(IterMut previous_from, IterMut position) noexcept -> IterMut {
        return IterMut(
            Algorithms::get_previous_node(previous_from.pointed_node(), position.pointed_node()));
    }

    [[nodiscard]]
    constexpr auto previous(Iter previous_from, Iter position) const noexcept -> Iter {
        return Iter(
            Algorithms::get_previous_node(previous_from.pointed_node(), position.pointed_node()));
    }

    constexpr auto splice_after(Iter position, Slist &source) noexcept -> void {
        if (this == &source || source.empty()) {
            return;
        }
        splice_after(position, source, source.before_begin(), source.previous(source.end()),
                     source.size());
    }

    constexpr auto splice_after(Iter position, Slist &source, Iter element) noexcept -> void {
        splice_after(position, source, source.previous(element), element, 1U);
    }

    constexpr auto splice_after(Iter position, Slist &source, Iter before_first,
                                Iter before_last) noexcept -> void {
        const auto first = NodeTraits::get_next(before_first.pointed_node());
        if (first == NodeTraits::get_next(before_last.pointed_node())) {
            return;
        }
        splice_after(position, source, before_first, before_last,
                     Algorithms::distance(first, NodeTraits::get_next(before_last.pointed_node())));
    }

    constexpr auto splice_after(Iter position, Slist &source, Iter before_first, Iter before_last,
                                Size const count) noexcept -> void {
        if (count == 0U) {
            return;
        }
        Algorithms::transfer_after(position.pointed_node(), before_first.pointed_node(),
                                   before_last.pointed_node());
        if (this != &source) {
            increase_size(count);
            source.decrease_size(count);
        }
        refresh_last();
        source.refresh_last();
    }

    constexpr auto splice(Iter position, Slist &source) noexcept -> void {
        splice_after(previous(position), source);
    }

    constexpr auto splice(Iter position, Slist &source, Iter element) noexcept -> void {
        splice_after(previous(position), source, element);
    }

    constexpr auto splice(Iter position, Slist &source, Iter first, Iter last) noexcept -> void {
        if (first == last) {
            return;
        }
        splice_after(previous(position), source, source.previous(first), source.previous(last));
    }

    constexpr auto splice(Iter position, Slist &source, Iter first, Iter last,
                          Size const count) noexcept -> void {
        if (first == last) {
            return;
        }
        splice_after(previous(position), source, source.previous(first), source.previous(last),
                     count);
    }

    template <typename Compare>
        requires(std::is_nothrow_invocable_r_v<bool, Compare &, T const &, T const &>)
    constexpr auto merge(Slist &source, Compare compare) noexcept -> void {
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

    constexpr auto merge(Slist &source) noexcept -> void
        requires(std::is_nothrow_invocable_r_v<bool, std::less<>, T const &, T const &>)
    {
        merge(source, std::less<>{});
    }

    template <typename Compare>
        requires(std::is_nothrow_invocable_r_v<bool, Compare &, T const &, T const &>)
    constexpr auto sort(Compare compare) noexcept -> void {
        if (size() < 2U) {
            return;
        }

        Slist carry{};
        std::array<Slist, std::numeric_limits<Size>::digits> bins{};
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
        if constexpr (PackedOptions::LINEAR) {
            NodeTraits::set_next(header(), Algorithms::reverse(NodeTraits::get_next(header())));
        } else {
            Algorithms::reverse(header());
        }
        refresh_last();
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
        auto before_current = before_begin();
        while (NodeTraits::get_next(before_current.pointed_node()) != end_node()) {
            const auto current = IterMut(NodeTraits::get_next(before_current.pointed_node()));
            if (std::invoke(predicate, *current)) {
                static_cast<void>(erase_after_and_dispose(before_current, disposer));
                ++removed;
            } else {
                ++before_current;
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
        if (size() < 2U) {
            return 0U;
        }

        auto removed = Size{};
        auto current = begin();
        auto next = current;
        ++next;
        while (next != end()) {
            if (std::invoke(predicate, *current, *next)) {
                next = erase_after_and_dispose(current, disposer);
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
    constexpr auto clone_from(Slist const &source, Cloner cloner, Disposer disposer) noexcept
        -> ustl::Result<void, ustl::collections::Error> {
        detail::require(this != &source);
        clear_and_dispose(disposer);
        auto previous = before_begin();
        for (auto const &source_value: source) {
            auto clone = std::invoke(cloner, source_value);
            if (clone.is_err()) {
                const auto error = clone.error();
                clear_and_dispose(disposer);
                return ustl::err(error);
            }
            detail::require(clone.value() != nullptr);
            previous = insert_after(previous, *clone.value());
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
    constexpr auto reset() noexcept -> void {
        Algorithms::init_header(header());
        if constexpr (PackedOptions::CONSTANT_TIME_SIZE) {
            m_size = 0U;
        }
        if constexpr (PackedOptions::CACHE_LAST) {
            m_last = header();
        }
    }

    constexpr auto increment_size() noexcept -> void {
        if constexpr (PackedOptions::CONSTANT_TIME_SIZE) {
            ++m_size;
        }
    }

    constexpr auto decrement_size() noexcept -> void {
        if constexpr (PackedOptions::CONSTANT_TIME_SIZE) {
            --m_size;
        }
    }

    constexpr auto increase_size(Size const count) noexcept -> void {
        if constexpr (PackedOptions::CONSTANT_TIME_SIZE) {
            m_size += count;
        }
    }

    constexpr auto decrease_size(Size const count) noexcept -> void {
        if constexpr (PackedOptions::CONSTANT_TIME_SIZE) {
            m_size -= count;
        }
    }

    [[nodiscard]]
    constexpr auto header() noexcept -> NodePtr {
        return PointerTraits<NodePtr>::pointer_to(m_header);
    }

    [[nodiscard]]
    constexpr auto header() const noexcept -> NodePtr {
        return PointerTraits<NodePtr>::pointer_to(const_cast<Node &>(m_header));
    }

    [[nodiscard]]
    constexpr auto end_node() const noexcept -> NodePtr {
        return Algorithms::end_node(header());
    }

    [[nodiscard]]
    constexpr auto last_node() const noexcept -> NodePtr {
        if constexpr (PackedOptions::CACHE_LAST) {
            return m_last;
        } else {
            return Algorithms::get_previous_node(header(), end_node());
        }
    }

    constexpr auto refresh_last() noexcept -> void {
        if constexpr (PackedOptions::CACHE_LAST) {
            m_last = Algorithms::get_previous_node(header(), end_node());
        }
    }

    [[nodiscard]]
    static constexpr auto node_from_const(typename NodeTraits::ConstNodePtr const node) noexcept
        -> NodePtr {
        return PointerTraits<NodePtr>::pointer_to(const_cast<Node &>(
            *PointerTraits<typename NodeTraits::ConstNodePtr>::to_raw_pointer(node)));
    }

    /// Placeholders for the two optional members below. Every read and
    /// write of m_size/m_last already goes through an `if constexpr`
    /// guarded helper, so when an option is off the member only has to
    /// stop occupying space -- which [[no_unique_address]] does for an
    /// empty type.
    ///
    /// This matters to more than footprint: Slab embeds an
    /// Slist<Object, ConstantTimeSize<false>> in a page-frame descriptor
    /// and static_asserts that it is one pointer wide (see
    /// kernel/mm/slab.hpp).
    struct NoSize {};
    struct NoLast {};

    Node m_header{};
    [[no_unique_address]]
    std::conditional_t<PackedOptions::CONSTANT_TIME_SIZE, Size, NoSize> m_size{};
    [[no_unique_address]]
    std::conditional_t<PackedOptions::CACHE_LAST, NodePtr, NoLast> m_last{};
};
} // namespace ustl::collections::intrusive

#ifndef USTL_DECLARE_SLIST
#define USTL_DECLARE_SLIST(OWNER, ALIAS, ...)                                                      \
    using ALIAS = ::ustl::collections::intrusive::Slist<OWNER __VA_OPT__(, ) __VA_ARGS__>
#endif

#endif // USTL_COLLECTIONS_INTRUSIVE_SLIST_HPP
