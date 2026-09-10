/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_COMMON_SLIST_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_COMMON_SLIST_ALGORITHMS_HPP 1

#include <ustl/collections/intrusive/detail/assert.hpp>

#include <functional>
#include <type_traits>

#include <cstddef>

namespace ustl::collections::intrusive::detail {
template <typename TNodeTraits>
class CommonSlistAlgorithms final {
  public:
    using Node = typename TNodeTraits::Node;
    using NodePtr = typename TNodeTraits::NodePtr;
    using ConstNodePtr = typename TNodeTraits::ConstNodePtr;
    using NodeTraits = TNodeTraits;

    [[nodiscard]]
    static constexpr auto get_previous_node(NodePtr node, NodePtr const target) noexcept
        -> NodePtr {
        while (target != NodeTraits::get_next(node)) {
            node = NodeTraits::get_next(node);
            require(static_cast<bool>(node));
        }
        return node;
    }
    static constexpr auto init(NodePtr const node) noexcept -> void {
        NodeTraits::set_next(node, NodePtr{});
    }
    [[nodiscard]]
    static constexpr auto unique(ConstNodePtr const node) noexcept -> bool {
        const auto next = NodeTraits::get_next(node);
        return !next || next == node;
    }
    [[nodiscard]]
    static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
        return !NodeTraits::get_next(node);
    }
    static constexpr auto unlink_after(NodePtr const previous) noexcept -> void {
        NodeTraits::set_next(previous, NodeTraits::get_next(NodeTraits::get_next(previous)));
    }
    static constexpr auto unlink_after(NodePtr const previous, NodePtr const last) noexcept
        -> void {
        NodeTraits::set_next(previous, last);
    }
    static constexpr auto link_after(NodePtr const previous, NodePtr const node) noexcept -> void {
        NodeTraits::set_next(node, NodeTraits::get_next(previous));
        NodeTraits::set_next(previous, node);
    }
    static constexpr auto incorporate_after(NodePtr const before_position, NodePtr const begin,
                                            NodePtr const before_end) noexcept -> void {
        const auto next = NodeTraits::get_next(before_position);
        NodeTraits::set_next(before_position, begin);
        NodeTraits::set_next(before_end, next);
    }
    static constexpr auto transfer_after(NodePtr const before_position, NodePtr const before_begin,
                                         NodePtr const before_end) noexcept -> void {
        if (before_position != before_begin && before_position != before_end &&
            before_begin != before_end) {
            const auto next_begin = NodeTraits::get_next(before_begin);
            const auto next_end = NodeTraits::get_next(before_end);
            const auto next_position = NodeTraits::get_next(before_position);
            NodeTraits::set_next(before_begin, next_end);
            NodeTraits::set_next(before_end, next_position);
            NodeTraits::set_next(before_position, next_begin);
        }
    }
    struct StablePartitionInfo final {
        std::size_t m_num_first_partition;
        std::size_t m_num_second_partition;
        NodePtr m_begin_second_partition;
        NodePtr m_new_last_node;
    };
    template <typename Pred>
        requires(std::is_nothrow_invocable_r_v<bool, Pred &, NodePtr>)
    static constexpr auto stable_partition(NodePtr const before_begin, NodePtr const end,
                                           Pred &&pred, StablePartitionInfo &info) noexcept
        -> void {
        auto before_current = before_begin;
        auto current = NodeTraits::get_next(before_current);
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
                    last_removed = current;
                    const auto next = NodeTraits::get_next(current);
                    NodeTraits::set_next(before_current, next);
                    current = next;
                } else {
                    ++second_count;
                    before_current = current;
                    current = NodeTraits::get_next(current);
                }
            }
            NodeTraits::set_next(last_removed, new_first);
            break;
        }
        info.m_num_first_partition = first_count;
        info.m_num_second_partition = second_count;
        info.m_begin_second_partition = new_first;
        info.m_new_last_node = before_current;
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
    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, NodePtr>)
    [[nodiscard]]
    static constexpr auto unlink_after_and_dispose(NodePtr const before_begin, NodePtr const end,
                                                   Disposer &&disposer) noexcept -> std::size_t {
        std::size_t count = 0;
        auto current = NodeTraits::get_next(before_begin);
        while (current != end) {
            const auto erased = current;
            current = NodeTraits::get_next(current);
            std::invoke(disposer, erased);
            ++count;
        }
        NodeTraits::set_next(before_begin, end);
        return count;
    }
    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, NodePtr>)
    static constexpr auto unlink_after_and_dispose(NodePtr const before_begin,
                                                   Disposer &&disposer) noexcept -> void {
        const auto erased = NodeTraits::get_next(before_begin);
        NodeTraits::set_next(before_begin, NodeTraits::get_next(erased));
        std::invoke(disposer, erased);
    }
    template <typename Disposer>
        requires(std::is_nothrow_invocable_v<Disposer &, NodePtr>)
    [[nodiscard]]
    static constexpr auto detach_and_dispose(NodePtr const node, Disposer &&disposer) noexcept
        -> std::size_t {
        auto count = std::size_t{0};
        auto current = NodeTraits::get_next(node);
        while (current != node && current != NodePtr{}) {
            const auto erased = current;
            current = NodeTraits::get_next(current);
            std::invoke(disposer, erased);
            ++count;
        }
        NodeTraits::set_next(node, current);
        return count;
    }
};
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_COMMON_SLIST_ALGORITHMS_HPP
