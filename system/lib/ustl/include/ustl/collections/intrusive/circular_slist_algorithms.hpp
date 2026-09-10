/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_CIRCULAR_SLIST_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_CIRCULAR_SLIST_ALGORITHMS_HPP 1

#include <ustl/collections/intrusive/detail/common_slist_algorithms.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

#include <type_traits>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename TNodeTraits>
class CircularSlistAlgorithms final {
    using Base = detail::CommonSlistAlgorithms<TNodeTraits>;

  public:
    using Node = typename TNodeTraits::Node;
    using NodePtr = typename TNodeTraits::NodePtr;
    using ConstNodePtr = typename TNodeTraits::ConstNodePtr;
    using NodeTraits = TNodeTraits;
    using StablePartitionInfo = typename Base::StablePartitionInfo;

    static constexpr auto init(NodePtr const node) noexcept -> void {
        Base::init(node);
    }
    [[nodiscard]]
    static constexpr auto unique(ConstNodePtr const node) noexcept -> bool {
        return Base::unique(node);
    }
    [[nodiscard]]
    static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
        return Base::inited(node);
    }
    static constexpr auto unlink_after(NodePtr const previous) noexcept -> void {
        Base::unlink_after(previous);
    }
    static constexpr auto unlink_after(NodePtr const previous, NodePtr const last) noexcept
        -> void {
        Base::unlink_after(previous, last);
    }
    static constexpr auto link_after(NodePtr const previous, NodePtr const node) noexcept -> void {
        Base::link_after(previous, node);
    }
    static constexpr auto incorporate_after(NodePtr const before_position, NodePtr const begin,
                                            NodePtr const before_end) noexcept -> void {
        Base::incorporate_after(before_position, begin, before_end);
    }
    static constexpr auto transfer_after(NodePtr const before_position, NodePtr const before_begin,
                                         NodePtr const before_end) noexcept -> void {
        Base::transfer_after(before_position, before_begin, before_end);
    }
    template <typename Pred>
        requires(std::is_nothrow_invocable_r_v<bool, Pred &, NodePtr>)
    static constexpr auto stable_partition(NodePtr const before_begin, NodePtr const end,
                                           Pred &&pred, StablePartitionInfo &info) noexcept
        -> void {
        Base::stable_partition(before_begin, end, static_cast<Pred &&>(pred), info);
    }
    [[nodiscard]]
    static constexpr auto distance(ConstNodePtr begin, ConstNodePtr const end) noexcept
        -> std::size_t {
        return Base::distance(begin, end);
    }

    static constexpr auto init_header(NodePtr const node) noexcept -> void {
        NodeTraits::set_next(node, node);
    }
    [[nodiscard]]
    static constexpr auto end_node(ConstNodePtr const node) noexcept -> NodePtr {
        return PointerTraits<NodePtr>::const_cast_from(node);
    }
    [[nodiscard]]
    static constexpr auto is_empty(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_next(node) == node;
    }
    [[nodiscard]]
    static constexpr auto is_sentinel(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_next(node) == NodePtr{};
    }
    static constexpr auto set_sentinel(NodePtr const node) noexcept -> void {
        NodeTraits::set_next(node, NodePtr{});
    }
    [[nodiscard]]
    static constexpr auto get_previous_node(NodePtr const previous_init,
                                            NodePtr const node) noexcept -> NodePtr {
        return Base::get_previous_node(previous_init, node);
    }
    [[nodiscard]]
    static constexpr auto get_previous_node(NodePtr const node) noexcept -> NodePtr {
        return get_previous_node(node, node);
    }
    [[nodiscard]]
    static constexpr auto get_previous_previous_node(NodePtr const node) noexcept -> NodePtr {
        return get_previous_previous_node(node, node);
    }
    [[nodiscard]]
    static constexpr auto get_previous_previous_node(NodePtr previous, NodePtr const node) noexcept
        -> NodePtr {
        auto previous_next = NodeTraits::get_next(previous);
        auto previous_next_next = NodeTraits::get_next(previous_next);
        while (node != previous_next_next) {
            previous = previous_next;
            previous_next = previous_next_next;
            previous_next_next = NodeTraits::get_next(previous_next);
        }
        return previous;
    }
    [[nodiscard]]
    static constexpr auto count(ConstNodePtr const node) noexcept -> std::size_t {
        auto result = std::size_t{0};
        auto current = node;
        do {
            current = NodeTraits::get_next(current);
            ++result;
        } while (current != node);
        return result;
    }
    static constexpr auto unlink(NodePtr const node) noexcept -> void {
        if (NodeTraits::get_next(node) != NodePtr{}) {
            unlink_after(get_previous_node(node));
        }
    }
    static constexpr auto link_before(NodePtr const next, NodePtr const node) noexcept -> void {
        link_after(get_previous_node(next), node);
    }
    static constexpr auto swap_nodes(NodePtr const node, NodePtr const other) noexcept -> void {
        if (node == other) {
            return;
        }
        const auto next = NodeTraits::get_next(node);
        const auto other_next = NodeTraits::get_next(other);
        const auto is_null = next == NodePtr{};
        const auto other_is_null = other_next == NodePtr{};
        const auto is_empty = next == node;
        const auto other_is_empty = other_next == other;

        if (!other_is_null && !other_is_empty) {
            NodeTraits::set_next(next == other ? other : get_previous_node(other), node);
        }
        if (!is_null && !is_empty) {
            NodeTraits::set_next(other_next == node ? node : get_previous_node(node), other);
        }
        NodeTraits::set_next(node,
                             other_is_empty ? other : (other_next == node ? other : other_next));
        NodeTraits::set_next(other, is_empty ? node : (next == other ? node : next));
    }
    static constexpr auto reverse(NodePtr const node) noexcept -> void {
        const auto end = node;
        const auto current = NodeTraits::get_next(node);
        for (;;) {
            const auto next = NodeTraits::get_next(current);
            if (next == end) {
                return;
            }
            transfer_after(end, current, next);
        }
    }
    [[nodiscard]]
    static constexpr auto move_backwards(NodePtr const node, std::size_t positions) noexcept
        -> NodePtr {
        if (positions == 0) {
            return NodePtr{};
        }
        auto first = NodeTraits::get_next(node);
        if (NodeTraits::get_next(first) == node) {
            return NodePtr{};
        }

        auto end_found = false;
        auto new_last = NodePtr{};
        for (auto index = std::size_t{1}; index <= positions; ++index) {
            new_last = first;
            first = NodeTraits::get_next(first);
            if (first == node) {
                positions %= index;
                if (positions == 0) {
                    return NodePtr{};
                }
                index = 0;
                first = NodeTraits::get_next(node);
                unlink_after(new_last);
                end_found = true;
            }
        }
        if (!end_found) {
            unlink_after(get_previous_node(first, node));
        }
        link_after(new_last, node);
        return new_last;
    }
    [[nodiscard]]
    static constexpr auto move_forward(NodePtr const node, std::size_t positions) noexcept
        -> NodePtr {
        if (positions == 0) {
            return NodePtr{};
        }
        const auto first = NodeTraits::get_next(node);
        if (NodeTraits::get_next(first) == node) {
            return NodePtr{};
        }

        auto old_last = first;
        auto next_to_last = NodePtr{};
        auto new_last = node;
        auto distance_to_last = std::size_t{1};
        while (node != (next_to_last = NodeTraits::get_next(old_last))) {
            if (++distance_to_last > positions) {
                new_last = NodeTraits::get_next(new_last);
            }
            old_last = next_to_last;
        }
        if (distance_to_last <= positions) {
            auto new_before_last_position =
                (distance_to_last - (positions % distance_to_last)) % distance_to_last;
            if (new_before_last_position == 0) {
                return NodePtr{};
            }
            for (new_last = node; new_before_last_position-- != 0;
                 new_last = NodeTraits::get_next(new_last)) {
            }
        }
        unlink_after(old_last);
        link_after(new_last, node);
        return new_last;
    }
    static constexpr auto transfer_after(NodePtr const position, NodePtr const other) noexcept
        -> void {
        transfer_after(position, other, get_previous_node(other));
    }
    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, NodePtr>)
    [[nodiscard]]
    static constexpr auto detach_and_dispose(NodePtr const node, Disposer &&disposer) noexcept
        -> std::size_t {
        return Base::unlink_after_and_dispose(node, node, static_cast<Disposer &&>(disposer));
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_CIRCULAR_SLIST_ALGORITHMS_HPP
