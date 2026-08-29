/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_CIRCULAR_LIST_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_CIRCULAR_LIST_ALGORITHMS_HPP 1

#include <functional>
#include <type_traits>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename TNodeTraits>
class CircularListAlgorithms final {
  public:
    using Node = typename TNodeTraits::Node;
    using NodePtr = typename TNodeTraits::NodePtr;
    using ConstNodePtr = typename TNodeTraits::ConstNodePtr;
    using NodeTraits = TNodeTraits;

    static constexpr auto init(NodePtr const node) noexcept -> void {
        NodeTraits::set_next(node, NodePtr{});
        NodeTraits::set_previous(node, NodePtr{});
    }
    [[nodiscard]]
    static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
        return !NodeTraits::get_next(node);
    }
    static constexpr auto init_header(NodePtr const node) noexcept -> void {
        NodeTraits::set_next(node, node);
        NodeTraits::set_previous(node, node);
    }
    [[nodiscard]]
    static constexpr auto is_empty(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_next(node) == node;
    }
    [[nodiscard]]
    static constexpr auto unique(ConstNodePtr const node) noexcept -> bool {
        const auto next = NodeTraits::get_next(node);
        return !next || next == node;
    }
    [[nodiscard]]
    static constexpr auto count(ConstNodePtr const node) noexcept -> std::size_t {
        std::size_t result = 0;
        auto current = node;
        do {
            current = NodeTraits::get_next(current);
            ++result;
        } while (current != node);
        return result;
    }
    [[nodiscard]]
    static constexpr auto unlink(NodePtr const node) noexcept -> NodePtr {
        const auto next = NodeTraits::get_next(node);
        const auto previous = NodeTraits::get_previous(node);
        NodeTraits::set_next(previous, next);
        NodeTraits::set_previous(next, previous);
        return next;
    }
    static constexpr auto unlink(NodePtr const begin, NodePtr const end) noexcept -> void {
        if (begin != end) {
            const auto previous = NodeTraits::get_previous(begin);
            NodeTraits::set_previous(end, previous);
            NodeTraits::set_next(previous, end);
        }
    }
    static constexpr auto link_before(NodePtr const next, NodePtr const node) noexcept -> void {
        const auto previous = NodeTraits::get_previous(next);
        NodeTraits::set_previous(node, previous);
        NodeTraits::set_next(node, next);
        NodeTraits::set_previous(next, node);
        NodeTraits::set_next(previous, node);
    }
    static constexpr auto link_after(NodePtr const previous, NodePtr const node) noexcept -> void {
        const auto next = NodeTraits::get_next(previous);
        NodeTraits::set_previous(node, previous);
        NodeTraits::set_next(node, next);
        NodeTraits::set_next(previous, node);
        NodeTraits::set_previous(next, node);
    }
    static constexpr auto swap_nodes(NodePtr const node, NodePtr const other) noexcept -> void {
        if (node == other)
            return;
        const auto node_inited = inited(node);
        const auto other_inited = inited(other);
        if (node_inited)
            init_header(node);
        if (other_inited)
            init_header(other);
        const auto next_node = NodeTraits::get_next(node);
        const auto previous_node = NodeTraits::get_previous(node);
        const auto next_other = NodeTraits::get_next(other);
        const auto previous_other = NodeTraits::get_previous(other);
        swap_previous(next_node, next_other);
        swap_next(previous_node, previous_other);
        swap_next(node, other);
        swap_previous(node, other);
        if (node_inited)
            init(other);
        if (other_inited)
            init(node);
    }
    static constexpr auto transfer(NodePtr const position, NodePtr const begin,
                                   NodePtr const end) noexcept -> void {
        if (begin != end && position != begin && position != end) {
            const auto previous_position = NodeTraits::get_previous(position);
            const auto previous_begin = NodeTraits::get_previous(begin);
            const auto previous_end = NodeTraits::get_previous(end);
            NodeTraits::set_next(previous_end, position);
            NodeTraits::set_previous(position, previous_end);
            NodeTraits::set_next(previous_begin, end);
            NodeTraits::set_previous(end, previous_begin);
            NodeTraits::set_next(previous_position, begin);
            NodeTraits::set_previous(begin, previous_position);
        }
    }
    static constexpr auto transfer(NodePtr const position, NodePtr const node) noexcept -> void {
        const auto next = NodeTraits::get_next(node);
        if (next != position && node != position) {
            const auto previous_position = NodeTraits::get_previous(position);
            const auto previous_node = NodeTraits::get_previous(node);
            NodeTraits::set_next(previous_position, node);
            NodeTraits::set_previous(node, previous_position);
            NodeTraits::set_next(node, position);
            NodeTraits::set_previous(position, node);
            NodeTraits::set_previous(next, previous_node);
            NodeTraits::set_next(previous_node, next);
        }
    }
    static constexpr auto reverse(NodePtr const node) noexcept -> void {
        auto first = NodeTraits::get_next(node);
        auto current = NodeTraits::get_next(first);
        while (current != node) {
            const auto next = NodeTraits::get_next(current);
            transfer(first, current, next);
            first = current;
            current = next;
        }
    }
    static constexpr auto move_backwards(NodePtr const node, std::size_t positions) noexcept
        -> void {
        if (positions == 0)
            return;
        auto first = NodeTraits::get_next(node);
        if (first == NodeTraits::get_previous(node))
            return;
        static_cast<void>(unlink(node));
        while (positions-- != 0)
            first = NodeTraits::get_next(first);
        link_before(first, node);
    }
    static constexpr auto move_forward(NodePtr const node, std::size_t positions) noexcept -> void {
        if (positions == 0)
            return;
        auto last = NodeTraits::get_previous(node);
        if (last == NodeTraits::get_next(node))
            return;
        static_cast<void>(unlink(node));
        while (positions-- != 0)
            last = NodeTraits::get_previous(last);
        link_after(last, node);
    }
    [[nodiscard]]
    static constexpr auto distance(ConstNodePtr begin, ConstNodePtr const end) noexcept
        -> std::size_t {
        std::size_t result = 0;
        while (begin != end) {
            begin = NodeTraits::get_next(begin);
            ++result;
        }
        return result;
    }
    struct StablePartitionInfo final {
        std::size_t m_num_first_partition;
        std::size_t m_num_second_partition;
        NodePtr m_begin_second_partition;
    };
    template <typename Pred>
        requires(std::is_nothrow_invocable_r_v<bool, Pred &, NodePtr>)
    static constexpr auto stable_partition(NodePtr const begin, NodePtr const end, Pred &&pred,
                                           StablePartitionInfo &info) noexcept -> void {
        auto before_current = NodeTraits::get_previous(begin);
        auto current = begin;
        auto new_first = end;
        std::size_t first_count = 0;
        std::size_t second_count = 0;
        while (current != end) {
            if (std::invoke(pred, current)) {
                ++first_count;
                before_current = current;
                current = NodeTraits::get_next(current);
                continue;
            }
            ++second_count;
            auto last_removed = before_current;
            new_first = current;
            before_current = current;
            current = NodeTraits::get_next(current);
            while (current != end) {
                if (std::invoke(pred, current)) {
                    ++first_count;
                    NodeTraits::set_next(last_removed, current);
                    NodeTraits::set_previous(current, last_removed);
                    last_removed = current;
                    const auto next = NodeTraits::get_next(current);
                    NodeTraits::set_next(before_current, next);
                    NodeTraits::set_previous(next, before_current);
                    current = next;
                } else {
                    ++second_count;
                    before_current = current;
                    current = NodeTraits::get_next(current);
                }
            }
            NodeTraits::set_next(last_removed, new_first);
            NodeTraits::set_previous(new_first, last_removed);
            break;
        }
        info.m_num_first_partition = first_count;
        info.m_num_second_partition = second_count;
        info.m_begin_second_partition = new_first;
    }

  private:
    static constexpr auto swap_previous(NodePtr const node, NodePtr const other) noexcept -> void {
        const auto previous = NodeTraits::get_previous(node);
        NodeTraits::set_previous(node, NodeTraits::get_previous(other));
        NodeTraits::set_previous(other, previous);
    }
    static constexpr auto swap_next(NodePtr const node, NodePtr const other) noexcept -> void {
        const auto next = NodeTraits::get_next(node);
        NodeTraits::set_next(node, NodeTraits::get_next(other));
        NodeTraits::set_next(other, next);
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_CIRCULAR_LIST_ALGORITHMS_HPP
