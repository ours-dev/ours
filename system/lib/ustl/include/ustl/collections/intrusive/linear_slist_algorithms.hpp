/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_LINEAR_SLIST_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_LINEAR_SLIST_ALGORITHMS_HPP 1

#include <ustl/collections/intrusive/detail/common_slist_algorithms.hpp>

#include <type_traits>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename TNodeTraits>
class LinearSlistAlgorithms final {
    using Base = detail::CommonSlistAlgorithms<TNodeTraits>;

  public:
    using Node = typename TNodeTraits::Node;
    using NodePtr = typename TNodeTraits::NodePtr;
    using ConstNodePtr = typename TNodeTraits::ConstNodePtr;
    using NodeTraits = TNodeTraits;
    using StablePartitionInfo = typename Base::StablePartitionInfo;

    struct NodePair final {
        NodePtr m_first{};
        NodePtr m_second{};
    };

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
        NodeTraits::set_next(node, NodePtr{});
    }
    [[nodiscard]]
    static constexpr auto end_node(ConstNodePtr const) noexcept -> NodePtr {
        return NodePtr{};
    }
    [[nodiscard]]
    static constexpr auto is_empty(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_next(node) == NodePtr{};
    }
    [[nodiscard]]
    static constexpr auto is_sentinel(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_next(node) == node;
    }
    static constexpr auto set_sentinel(NodePtr const node) noexcept -> void {
        NodeTraits::set_next(node, node);
    }
    [[nodiscard]]
    static constexpr auto get_previous_node(NodePtr const previous_init,
                                            NodePtr const node) noexcept -> NodePtr {
        return Base::get_previous_node(previous_init, node);
    }
    [[nodiscard]]
    static constexpr auto count(ConstNodePtr const node) noexcept -> std::size_t {
        auto result = std::size_t{0};
        auto current = node;
        do {
            current = NodeTraits::get_next(current);
            ++result;
        } while (current != NodePtr{});
        return result;
    }
    static constexpr auto swap_trailing_nodes(NodePtr const node, NodePtr const other) noexcept
        -> void {
        const auto next = NodeTraits::get_next(node);
        const auto other_next = NodeTraits::get_next(other);
        NodeTraits::set_next(node, other_next);
        NodeTraits::set_next(other, next);
    }
    [[nodiscard]]
    static constexpr auto reverse(NodePtr const node) noexcept -> NodePtr {
        if (node == NodePtr{}) {
            return NodePtr{};
        }
        auto current = NodeTraits::get_next(node);
        auto first = node;
        while (current != NodePtr{}) {
            const auto next = NodeTraits::get_next(current);
            unlink_after(node);
            NodeTraits::set_next(current, first);
            first = current;
            current = next;
        }
        return first;
    }
    [[nodiscard]]
    static constexpr auto move_first_n_backwards(NodePtr const node, std::size_t positions) noexcept
        -> NodePair {
        auto result = NodePair{};
        if (positions == 0 || node == NodePtr{} || NodeTraits::get_next(node) == NodePtr{}) {
            return result;
        }

        auto first = node;
        auto end_found = false;
        auto new_last = NodePtr{};
        auto old_last = NodePtr{};
        for (auto index = std::size_t{1}; index <= positions; ++index) {
            new_last = first;
            first = NodeTraits::get_next(first);
            if (first == NodePtr{}) {
                positions %= index;
                if (positions == 0) {
                    return result;
                }
                old_last = new_last;
                index = 0;
                first = node;
                end_found = true;
            }
        }
        if (!end_found) {
            old_last = get_previous_node(first, NodePtr{});
        }
        NodeTraits::set_next(old_last, node);
        NodeTraits::set_next(new_last, NodePtr{});
        result.m_first = first;
        result.m_second = new_last;
        return result;
    }
    [[nodiscard]]
    static constexpr auto move_first_n_forward(NodePtr const node, std::size_t positions) noexcept
        -> NodePair {
        auto result = NodePair{};
        if (positions == 0 || node == NodePtr{} || NodeTraits::get_next(node) == NodePtr{}) {
            return result;
        }

        auto old_last = node;
        auto next_to_last = NodePtr{};
        auto new_last = node;
        auto distance_to_last = std::size_t{1};
        while ((next_to_last = NodeTraits::get_next(old_last)) != NodePtr{}) {
            if (distance_to_last++ > positions) {
                new_last = NodeTraits::get_next(new_last);
            }
            old_last = next_to_last;
        }
        if (distance_to_last <= positions) {
            const auto new_before_last_position =
                (distance_to_last - (positions % distance_to_last)) % distance_to_last;
            if (new_before_last_position == 0) {
                return result;
            }
            for (auto steps = new_before_last_position - 1; steps != 0; --steps) {
                new_last = NodeTraits::get_next(new_last);
            }
        }
        const auto new_first = NodeTraits::get_next(new_last);
        NodeTraits::set_next(old_last, node);
        NodeTraits::set_next(new_last, NodePtr{});
        result.m_first = new_first;
        result.m_second = new_last;
        return result;
    }
    static constexpr auto transfer_after(NodePtr const position, NodePtr const other) noexcept
        -> void {
        if (is_empty(position)) {
            swap_trailing_nodes(position, other);
            return;
        }
        transfer_after(position, other, get_previous_node(other, NodePtr{}));
    }
    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, NodePtr>)
    [[nodiscard]]
    static constexpr auto detach_and_dispose(NodePtr const node, Disposer &&disposer) noexcept
        -> std::size_t {
        return Base::unlink_after_and_dispose(node, NodePtr{}, static_cast<Disposer &&>(disposer));
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_LINEAR_SLIST_ALGORITHMS_HPP
