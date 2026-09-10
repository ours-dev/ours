/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_BSTREE_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_BSTREE_ALGORITHMS_HPP 1

#include <utility>

#include <cstddef>

#include <ustl/collections/intrusive/detail/bstree_algorithms_base.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
template <typename NodePointer>
struct InsertCommitDataT {
    bool m_link_left{};
    NodePointer m_node{};
};

template <typename NodePointer>
struct DataForRebalanceT final {
    NodePointer m_x{};
    NodePointer m_x_parent{};
    NodePointer m_y{};
};

template <typename TNodeTraits>
class BstreeAlgorithms: public detail::BstreeAlgorithmsBase<TNodeTraits> {
    using Base = detail::BstreeAlgorithmsBase<TNodeTraits>;

  public:
    using Node = typename TNodeTraits::Node;
    using NodeTraits = TNodeTraits;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;
    using InsertCommitData = InsertCommitDataT<NodePtr>;
    using DataForRebalance = DataForRebalanceT<NodePtr>;

    using Base::get_header;
    using Base::is_header;
    using Base::maximum;
    using Base::minimum;
    using Base::next_node;
    using Base::prev_node;

    [[nodiscard]]
    static constexpr auto begin_node(ConstNodePtr const header) noexcept -> NodePtr {
        return NodeTraits::get_left(header);
    }

    [[nodiscard]]
    static constexpr auto end_node(ConstNodePtr const header) noexcept -> NodePtr {
        return unconst(header);
    }

    [[nodiscard]]
    static constexpr auto root_node(ConstNodePtr const header) noexcept -> NodePtr {
        const auto root = NodeTraits::get_parent(header);
        return root != NodePtr{} ? root : unconst(header);
    }

    [[nodiscard]]
    static constexpr auto unique(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_parent(node) == NodePtr{};
    }

    static constexpr auto init(NodePtr const node) noexcept -> void {
        NodeTraits::set_parent(node, NodePtr{});
        NodeTraits::set_left(node, NodePtr{});
        NodeTraits::set_right(node, NodePtr{});
    }

    [[nodiscard]]
    static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_parent(node) == NodePtr{} &&
               NodeTraits::get_left(node) == NodePtr{} && NodeTraits::get_right(node) == NodePtr{};
    }

    static constexpr auto init_header(NodePtr const header) noexcept -> void {
        NodeTraits::set_parent(header, NodePtr{});
        NodeTraits::set_left(header, header);
        NodeTraits::set_right(header, header);
    }

    static constexpr auto swap_nodes(NodePtr const node_1, NodePtr const node_2) noexcept -> void {
        if (node_1 == node_2) {
            return;
        }
        swap_nodes(node_1, Base::get_header(node_1), node_2, Base::get_header(node_2));
    }

    static constexpr auto swap_nodes(NodePtr const node_1, NodePtr const header_1,
                                     NodePtr const node_2, NodePtr const header_2) noexcept
        -> void {
        if (node_1 == node_2) {
            return;
        }
        if (header_1 != header_2) {
            replace_header_reference(header_1, node_1, node_2);
            replace_header_reference(header_2, node_2, node_1);
        } else {
            if (node_1 == NodeTraits::get_left(header_1)) {
                NodeTraits::set_left(header_1, node_2);
            } else if (node_2 == NodeTraits::get_left(header_1)) {
                NodeTraits::set_left(header_1, node_1);
            }
            if (node_1 == NodeTraits::get_right(header_1)) {
                NodeTraits::set_right(header_1, node_2);
            } else if (node_2 == NodeTraits::get_right(header_1)) {
                NodeTraits::set_right(header_1, node_1);
            }
            if (node_1 == NodeTraits::get_parent(header_1)) {
                NodeTraits::set_parent(header_1, node_2);
            } else if (node_2 == NodeTraits::get_parent(header_1)) {
                NodeTraits::set_parent(header_1, node_1);
            }
            adjust_adjacent_nodes(node_1, node_2);
        }

        const auto node_1_left = NodeTraits::get_left(node_1);
        const auto node_1_right = NodeTraits::get_right(node_1);
        const auto node_1_parent = NodeTraits::get_parent(node_1);
        const auto node_2_left = NodeTraits::get_left(node_2);
        const auto node_2_right = NodeTraits::get_right(node_2);
        const auto node_2_parent = NodeTraits::get_parent(node_2);
        NodeTraits::set_left(node_1, node_2_left);
        NodeTraits::set_right(node_1, node_2_right);
        NodeTraits::set_parent(node_1, node_2_parent);
        NodeTraits::set_left(node_2, node_1_left);
        NodeTraits::set_right(node_2, node_1_right);
        NodeTraits::set_parent(node_2, node_1_parent);

        fix_children_parent(node_1);
        fix_children_parent(node_2);
        const auto node_1_parent_after_swap = NodeTraits::get_parent(node_1);
        const auto node_2_parent_after_swap = NodeTraits::get_parent(node_2);
        if (node_1_parent_after_swap == node_2_parent_after_swap) {
            const auto left = NodeTraits::get_left(node_1_parent_after_swap);
            NodeTraits::set_left(node_1_parent_after_swap,
                                 NodeTraits::get_right(node_1_parent_after_swap));
            NodeTraits::set_right(node_1_parent_after_swap, left);
        } else {
            replace_parent_reference(node_1, node_2, header_2);
            replace_parent_reference(node_2, node_1, header_1);
        }
    }

    static constexpr auto replace_node(NodePtr const node_to_replace,
                                       NodePtr const new_node) noexcept -> void {
        replace_node(node_to_replace, Base::get_header(node_to_replace), new_node);
    }

    static constexpr auto replace_node(NodePtr const node_to_replace, NodePtr const header,
                                       NodePtr const new_node) noexcept -> void {
        if (node_to_replace == NodeTraits::get_left(header)) {
            NodeTraits::set_left(header, new_node);
        }
        if (node_to_replace == NodeTraits::get_right(header)) {
            NodeTraits::set_right(header, new_node);
        }
        if (node_to_replace == NodeTraits::get_parent(header)) {
            NodeTraits::set_parent(header, new_node);
        }

        NodeTraits::set_left(new_node, NodeTraits::get_left(node_to_replace));
        NodeTraits::set_right(new_node, NodeTraits::get_right(node_to_replace));
        NodeTraits::set_parent(new_node, NodeTraits::get_parent(node_to_replace));
        fix_children_parent(new_node);

        const auto parent = NodeTraits::get_parent(new_node);
        if (parent != NodePtr{} && parent != header) {
            if (NodeTraits::get_left(parent) == node_to_replace) {
                NodeTraits::set_left(parent, new_node);
            }
            if (NodeTraits::get_right(parent) == node_to_replace) {
                NodeTraits::set_right(parent, new_node);
            }
        }
    }

    template <typename Disposer>
    static constexpr auto clear_and_dispose(NodePtr const header, Disposer disposer) noexcept
        -> void {
        while (!unique(header)) {
            const auto node = begin_node(header);
            erase(header, node);
            disposer(node);
        }
    }

    [[nodiscard]]
    static constexpr auto unlink_leftmost_without_rebalance(NodePtr const header) noexcept
        -> NodePtr {
        const auto leftmost = NodeTraits::get_left(header);
        if (leftmost == header) {
            return NodePtr{};
        }

        const auto parent = NodeTraits::get_parent(leftmost);
        const auto right = NodeTraits::get_right(leftmost);
        if (right != NodePtr{}) {
            NodeTraits::set_parent(right, parent);
            set_child(header, right, parent, NodeTraits::get_left(parent) == leftmost);
            NodeTraits::set_left(header, Base::minimum(right));
        } else if (parent == header) {
            init_header(header);
        } else {
            NodeTraits::set_left(parent, NodePtr{});
            NodeTraits::set_left(header, parent);
        }
        init(leftmost);
        return leftmost;
    }

    [[nodiscard]]
    static constexpr auto size(ConstNodePtr const header) noexcept -> std::size_t {
        std::size_t result{};
        const auto end = end_node(header);
        for (auto node = begin_node(header); node != end; node = Base::next_node(node)) {
            ++result;
        }
        return result;
    }

    static constexpr auto swap_tree(NodePtr const header_1, NodePtr const header_2) noexcept
        -> void {
        if (header_1 == header_2) {
            return;
        }

        const auto root_1 = NodeTraits::get_parent(header_1);
        const auto left_1 = NodeTraits::get_left(header_1);
        const auto right_1 = NodeTraits::get_right(header_1);
        const auto root_2 = NodeTraits::get_parent(header_2);
        const auto left_2 = NodeTraits::get_left(header_2);
        const auto right_2 = NodeTraits::get_right(header_2);
        NodeTraits::set_parent(header_1, root_2);
        NodeTraits::set_left(header_1, left_2);
        NodeTraits::set_right(header_1, right_2);
        NodeTraits::set_parent(header_2, root_1);
        NodeTraits::set_left(header_2, left_1);
        NodeTraits::set_right(header_2, right_1);
        fix_header_root(header_1);
        fix_header_root(header_2);
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto find(ConstNodePtr const header, Key const &key,
                               KeyNodePointerCompare compare) noexcept -> NodePtr {
        const auto result = lower_bound(header, key, compare);
        return result == end_node(header) || compare(key, result) ? end_node(header) : result;
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto bounded_range(ConstNodePtr const header, Key const &lower_key,
                                        Key const &upper_key, KeyNodePointerCompare compare,
                                        bool const left_closed, bool const right_closed) noexcept
        -> std::pair<NodePtr, NodePtr> {
        const auto first = left_closed ? lower_bound(header, lower_key, compare)
                                       : upper_bound(header, lower_key, compare);
        const auto second = right_closed ? upper_bound(header, upper_key, compare)
                                         : lower_bound(header, upper_key, compare);
        return {first, second};
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto count(ConstNodePtr const header, Key const &key,
                                KeyNodePointerCompare compare) noexcept -> std::size_t {
        const auto [first, last] = equal_range(header, key, compare);
        std::size_t result{};
        for (auto node = first; node != last; node = Base::next_node(node)) {
            ++result;
        }
        return result;
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto equal_range(ConstNodePtr const header, Key const &key,
                                      KeyNodePointerCompare compare) noexcept
        -> std::pair<NodePtr, NodePtr> {
        return bounded_range(header, key, key, compare, true, true);
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto lower_bound_range(ConstNodePtr const header, Key const &key,
                                            KeyNodePointerCompare compare) noexcept
        -> std::pair<NodePtr, NodePtr> {
        const auto first = lower_bound(header, key, compare);
        auto last = first;
        if (first != end_node(header) && !compare(key, first)) {
            last = Base::next_node(first);
        }
        return {first, last};
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto lower_bound(ConstNodePtr const header, Key const &key,
                                      KeyNodePointerCompare compare) noexcept -> NodePtr {
        auto node = NodeTraits::get_parent(header);
        auto result = end_node(header);
        while (node != NodePtr{}) {
            if (compare(node, key)) {
                node = NodeTraits::get_right(node);
            } else {
                result = node;
                node = NodeTraits::get_left(node);
            }
        }
        return result;
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto upper_bound(ConstNodePtr const header, Key const &key,
                                      KeyNodePointerCompare compare) noexcept -> NodePtr {
        auto node = NodeTraits::get_parent(header);
        auto result = end_node(header);
        while (node != NodePtr{}) {
            if (compare(key, node)) {
                result = node;
                node = NodeTraits::get_left(node);
            } else {
                node = NodeTraits::get_right(node);
            }
        }
        return result;
    }

    static constexpr auto insert_unique_commit(NodePtr const header, NodePtr const new_node,
                                               InsertCommitData const &commit_data) noexcept
        -> void {
        insert_commit(header, new_node, commit_data);
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto
    insert_unique_check(ConstNodePtr const header, Key const &key, KeyNodePointerCompare compare,
                        InsertCommitData &commit_data, std::size_t *const depth = nullptr) noexcept
        -> std::pair<NodePtr, bool> {
        std::size_t measured_depth{};
        const auto mutable_header = end_node(header);
        auto parent = mutable_header;
        auto node = NodeTraits::get_parent(header);
        auto previous = NodePtr{};
        auto link_left = true;
        while (node != NodePtr{}) {
            ++measured_depth;
            parent = node;
            link_left = compare(key, node);
            if (link_left) {
                node = NodeTraits::get_left(node);
            } else {
                previous = node;
                node = NodeTraits::get_right(node);
            }
        }
        if (depth != nullptr) {
            *depth = measured_depth;
        }
        const auto is_unique = previous == NodePtr{} || compare(previous, key);
        if (is_unique) {
            commit_data.m_link_left = link_left;
            commit_data.m_node = parent;
        }
        return {previous, is_unique};
    }

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto insert_unique_check(ConstNodePtr const header, NodePtr const hint,
                                              Key const &key, KeyNodePointerCompare compare,
                                              InsertCommitData &commit_data,
                                              std::size_t *const depth = nullptr) noexcept
        -> std::pair<NodePtr, bool> {
        if (hint == end_node(header) || compare(key, hint)) {
            auto previous = hint;
            if (hint == begin_node(header) || compare(previous = Base::prev_node(hint), key)) {
                commit_data.m_link_left = unique(header) || NodeTraits::get_left(hint) == NodePtr{};
                commit_data.m_node = commit_data.m_link_left ? hint : previous;
                if (depth != nullptr) {
                    *depth = commit_data.m_node == end_node(header)
                                 ? 0U
                                 : depth_of(commit_data.m_node) + 1U;
                }
                return {NodePtr{}, true};
            }
        }
        return insert_unique_check(header, key, compare, commit_data, depth);
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal(NodePtr const header, NodePtr const hint,
                                       NodePtr const new_node, NodePointerCompare compare,
                                       std::size_t *const depth = nullptr) noexcept -> NodePtr {
        InsertCommitData commit_data{};
        insert_equal_check(header, hint, new_node, compare, commit_data, depth);
        insert_commit(header, new_node, commit_data);
        return new_node;
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal_upper_bound(NodePtr const header, NodePtr const new_node,
                                                   NodePointerCompare compare,
                                                   std::size_t *const depth = nullptr) noexcept
        -> NodePtr {
        InsertCommitData commit_data{};
        insert_equal_upper_bound_check(header, new_node, compare, commit_data, depth);
        insert_commit(header, new_node, commit_data);
        return new_node;
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal_lower_bound(NodePtr const header, NodePtr const new_node,
                                                   NodePointerCompare compare,
                                                   std::size_t *const depth = nullptr) noexcept
        -> NodePtr {
        InsertCommitData commit_data{};
        insert_equal_lower_bound_check(header, new_node, compare, commit_data, depth);
        insert_commit(header, new_node, commit_data);
        return new_node;
    }

    static constexpr auto insert_before(NodePtr const header, NodePtr const position,
                                        NodePtr const new_node,
                                        std::size_t *const depth = nullptr) noexcept -> NodePtr {
        InsertCommitData commit_data{};
        insert_before_check(header, position, commit_data, depth);
        insert_commit(header, new_node, commit_data);
        return new_node;
    }

    static constexpr auto push_back(NodePtr const header, NodePtr const new_node,
                                    std::size_t *const depth = nullptr) noexcept -> void {
        InsertCommitData commit_data{};
        push_back_check(header, commit_data, depth);
        insert_commit(header, new_node, commit_data);
    }

    static constexpr auto push_front(NodePtr const header, NodePtr const new_node,
                                     std::size_t *const depth = nullptr) noexcept -> void {
        InsertCommitData commit_data{};
        push_front_check(header, commit_data, depth);
        insert_commit(header, new_node, commit_data);
    }

    [[nodiscard]]
    static constexpr auto depth(ConstNodePtr const node) noexcept -> std::size_t {
        return depth_of(unconst(node));
    }

    static constexpr auto erase(NodePtr const header, NodePtr const node) noexcept -> void {
        DataForRebalance ignored{};
        erase(header, node, ignored);
    }

    static constexpr auto unlink(NodePtr const node) noexcept -> void {
        if (!unique(node)) {
            erase(Base::get_header(node), node);
        }
    }

    [[nodiscard]]
    static constexpr auto subtree_size(ConstNodePtr const subtree) noexcept -> std::size_t {
        if (subtree == ConstNodePtr{}) {
            return 0U;
        }
        std::size_t result{};
        auto node = unconst(subtree);
        const auto end = Base::next_node(Base::maximum(node));
        node = Base::minimum(node);
        while (node != end) {
            ++result;
            node = Base::next_node(node);
        }
        return result;
    }

    [[nodiscard]]
    static constexpr auto is_left_child(NodePtr const node) noexcept -> bool {
        return NodeTraits::get_left(NodeTraits::get_parent(node)) == node;
    }

    [[nodiscard]]
    static constexpr auto is_right_child(NodePtr const node) noexcept -> bool {
        return NodeTraits::get_right(NodeTraits::get_parent(node)) == node;
    }

  protected:
    static constexpr auto erase(NodePtr const header, NodePtr const node,
                                DataForRebalance &info) noexcept -> void {
        auto replacement = node;
        const auto node_left = NodeTraits::get_left(node);
        const auto node_right = NodeTraits::get_right(node);
        auto child = NodePtr{};
        if (node_left == NodePtr{}) {
            child = node_right;
        } else if (node_right == NodePtr{}) {
            child = node_left;
        } else {
            replacement = Base::minimum(node_right);
            child = NodeTraits::get_right(replacement);
        }

        const auto node_parent = NodeTraits::get_parent(node);
        const auto node_was_left = NodeTraits::get_left(node_parent) == node;
        auto child_parent = NodePtr{};
        if (replacement != node) {
            NodeTraits::set_parent(node_left, replacement);
            NodeTraits::set_left(replacement, node_left);
            if (replacement != node_right) {
                child_parent = NodeTraits::get_parent(replacement);
                NodeTraits::set_left(child_parent, child);
                if (child != NodePtr{}) {
                    NodeTraits::set_parent(child, child_parent);
                }
                NodeTraits::set_right(replacement, node_right);
                NodeTraits::set_parent(node_right, replacement);
            } else {
                child_parent = replacement;
            }
            NodeTraits::set_parent(replacement, node_parent);
            set_child(header, replacement, node_parent, node_was_left);
        } else {
            child_parent = node_parent;
            if (child != NodePtr{}) {
                NodeTraits::set_parent(child, node_parent);
            }
            set_child(header, child, node_parent, node_was_left);
            if (NodeTraits::get_left(header) == node) {
                NodeTraits::set_left(header, node_right == NodePtr{} ? node_parent
                                                                     : Base::minimum(node_right));
            }
            if (NodeTraits::get_right(header) == node) {
                NodeTraits::set_right(header, node_left == NodePtr{} ? node_parent
                                                                     : Base::maximum(node_left));
            }
        }

        info.m_x = child;
        info.m_y = replacement;
        info.m_x_parent = child_parent;
        init(node);
    }

    static constexpr auto set_child(NodePtr const header, NodePtr const child, NodePtr const parent,
                                    bool const link_left) noexcept -> void {
        if (parent == header) {
            NodeTraits::set_parent(header, child);
        } else if (link_left) {
            NodeTraits::set_left(parent, child);
        } else {
            NodeTraits::set_right(parent, child);
        }
    }

    static constexpr auto rotate_left_no_parent_fix(NodePtr const node,
                                                    NodePtr const right) noexcept -> void {
        const auto right_left = NodeTraits::get_left(right);
        NodeTraits::set_right(node, right_left);
        if (right_left != NodePtr{}) {
            NodeTraits::set_parent(right_left, node);
        }
        NodeTraits::set_left(right, node);
        NodeTraits::set_parent(node, right);
    }

    static constexpr auto rotate_left(NodePtr const node, NodePtr const right, NodePtr const parent,
                                      NodePtr const header) noexcept -> void {
        const auto was_left = NodeTraits::get_left(parent) == node;
        rotate_left_no_parent_fix(node, right);
        NodeTraits::set_parent(right, parent);
        set_child(header, right, parent, was_left);
    }

    static constexpr auto rotate_right_no_parent_fix(NodePtr const node,
                                                     NodePtr const left) noexcept -> void {
        const auto left_right = NodeTraits::get_right(left);
        NodeTraits::set_left(node, left_right);
        if (left_right != NodePtr{}) {
            NodeTraits::set_parent(left_right, node);
        }
        NodeTraits::set_right(left, node);
        NodeTraits::set_parent(node, left);
    }

    static constexpr auto rotate_right(NodePtr const node, NodePtr const left, NodePtr const parent,
                                       NodePtr const header) noexcept -> void {
        const auto was_left = NodeTraits::get_left(parent) == node;
        rotate_right_no_parent_fix(node, left);
        NodeTraits::set_parent(left, parent);
        set_child(header, left, parent, was_left);
    }

  protected:
    [[nodiscard]]
    static constexpr auto unconst(ConstNodePtr const node) noexcept -> NodePtr {
        return PointerTraits<NodePtr>::const_cast_from(node);
    }

    static constexpr auto replace_header_reference(NodePtr const header, NodePtr const old_node,
                                                   NodePtr const new_node) noexcept -> void {
        if (NodeTraits::get_left(header) == old_node) {
            NodeTraits::set_left(header, new_node);
        }
        if (NodeTraits::get_right(header) == old_node) {
            NodeTraits::set_right(header, new_node);
        }
        if (NodeTraits::get_parent(header) == old_node) {
            NodeTraits::set_parent(header, new_node);
        }
    }

    static constexpr auto adjust_adjacent_nodes(NodePtr const node_1, NodePtr const node_2) noexcept
        -> void {
        if (NodeTraits::get_parent(node_2) == node_1) {
            NodeTraits::set_parent(node_2, node_2);
            if (NodeTraits::get_left(node_1) == node_2) {
                NodeTraits::set_left(node_1, node_1);
            } else {
                NodeTraits::set_right(node_1, node_1);
            }
        } else if (NodeTraits::get_parent(node_1) == node_2) {
            NodeTraits::set_parent(node_1, node_1);
            if (NodeTraits::get_left(node_2) == node_1) {
                NodeTraits::set_left(node_2, node_2);
            } else {
                NodeTraits::set_right(node_2, node_2);
            }
        }
    }

    static constexpr auto fix_children_parent(NodePtr const node) noexcept -> void {
        const auto left = NodeTraits::get_left(node);
        const auto right = NodeTraits::get_right(node);
        if (left != NodePtr{}) {
            NodeTraits::set_parent(left, node);
        }
        if (right != NodePtr{}) {
            NodeTraits::set_parent(right, node);
        }
    }

    static constexpr auto replace_parent_reference(NodePtr const node, NodePtr const old_node,
                                                   NodePtr const old_header) noexcept -> void {
        const auto parent = NodeTraits::get_parent(node);
        if (parent != NodePtr{} && parent != old_header) {
            if (NodeTraits::get_left(parent) == old_node) {
                NodeTraits::set_left(parent, node);
            }
            if (NodeTraits::get_right(parent) == old_node) {
                NodeTraits::set_right(parent, node);
            }
        }
    }

    static constexpr auto fix_header_root(NodePtr const header) noexcept -> void {
        const auto root = NodeTraits::get_parent(header);
        if (root == NodePtr{}) {
            NodeTraits::set_left(header, header);
            NodeTraits::set_right(header, header);
        } else {
            NodeTraits::set_parent(root, header);
        }
    }

    [[nodiscard]]
    static constexpr auto depth_of(NodePtr node) noexcept -> std::size_t {
        std::size_t result{};
        auto parent = NodeTraits::get_parent(node);
        while (node != NodeTraits::get_parent(parent)) {
            ++result;
            node = parent;
            parent = NodeTraits::get_parent(parent);
        }
        return result;
    }

    static constexpr auto insert_before_check(NodePtr const header, NodePtr const position,
                                              InsertCommitData &commit_data,
                                              std::size_t *const depth) noexcept -> void {
        auto previous = position;
        if (position != NodeTraits::get_left(header)) {
            previous = Base::prev_node(position);
        }
        const auto link_left = unique(header) || NodeTraits::get_left(position) == NodePtr{};
        commit_data.m_link_left = link_left;
        commit_data.m_node = link_left ? position : previous;
        if (depth != nullptr) {
            *depth = commit_data.m_node == header ? 0U : depth_of(commit_data.m_node) + 1U;
        }
    }

    static constexpr auto push_back_check(NodePtr const header, InsertCommitData &commit_data,
                                          std::size_t *const depth) noexcept -> void {
        const auto previous = NodeTraits::get_right(header);
        commit_data.m_link_left = false;
        commit_data.m_node = previous;
        if (depth != nullptr) {
            *depth = previous == header ? 0U : depth_of(previous) + 1U;
        }
    }

    static constexpr auto push_front_check(NodePtr const header, InsertCommitData &commit_data,
                                           std::size_t *const depth) noexcept -> void {
        const auto position = NodeTraits::get_left(header);
        commit_data.m_link_left = true;
        commit_data.m_node = position;
        if (depth != nullptr) {
            *depth = position == header ? 0U : depth_of(position) + 1U;
        }
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal_check(NodePtr const header, NodePtr const hint,
                                             NodePtr const new_node, NodePointerCompare compare,
                                             InsertCommitData &commit_data,
                                             std::size_t *const depth) noexcept -> void {
        if (hint == header || !compare(hint, new_node)) {
            auto previous = hint;
            if (hint == NodeTraits::get_left(header) ||
                !compare(new_node, previous = Base::prev_node(hint))) {
                commit_data.m_link_left = unique(header) || NodeTraits::get_left(hint) == NodePtr{};
                commit_data.m_node = commit_data.m_link_left ? hint : previous;
                if (depth != nullptr) {
                    *depth = commit_data.m_node == header ? 0U : depth_of(commit_data.m_node) + 1U;
                }
                return;
            }
            insert_equal_upper_bound_check(header, new_node, compare, commit_data, depth);
            return;
        }
        insert_equal_lower_bound_check(header, new_node, compare, commit_data, depth);
    }

    template <typename NodePointerCompare>
    static constexpr auto
    insert_equal_upper_bound_check(NodePtr const header, NodePtr const new_node,
                                   NodePointerCompare compare, InsertCommitData &commit_data,
                                   std::size_t *const depth) noexcept -> void {
        std::size_t measured_depth{};
        auto parent = header;
        auto node = NodeTraits::get_parent(header);
        while (node != NodePtr{}) {
            ++measured_depth;
            parent = node;
            node =
                compare(new_node, node) ? NodeTraits::get_left(node) : NodeTraits::get_right(node);
        }
        commit_data.m_link_left = parent == header || compare(new_node, parent);
        commit_data.m_node = parent;
        if (depth != nullptr) {
            *depth = measured_depth;
        }
    }

    template <typename NodePointerCompare>
    static constexpr auto
    insert_equal_lower_bound_check(NodePtr const header, NodePtr const new_node,
                                   NodePointerCompare compare, InsertCommitData &commit_data,
                                   std::size_t *const depth) noexcept -> void {
        std::size_t measured_depth{};
        auto parent = header;
        auto node = NodeTraits::get_parent(header);
        while (node != NodePtr{}) {
            ++measured_depth;
            parent = node;
            node =
                !compare(node, new_node) ? NodeTraits::get_left(node) : NodeTraits::get_right(node);
        }
        commit_data.m_link_left = parent == header || !compare(parent, new_node);
        commit_data.m_node = parent;
        if (depth != nullptr) {
            *depth = measured_depth;
        }
    }

    static constexpr auto insert_commit(NodePtr const header, NodePtr const new_node,
                                        InsertCommitData const &commit_data) noexcept -> void {
        const auto parent = commit_data.m_node;
        if (parent == header) {
            NodeTraits::set_parent(header, new_node);
            NodeTraits::set_left(header, new_node);
            NodeTraits::set_right(header, new_node);
        } else if (commit_data.m_link_left) {
            NodeTraits::set_left(parent, new_node);
            if (parent == NodeTraits::get_left(header)) {
                NodeTraits::set_left(header, new_node);
            }
        } else {
            NodeTraits::set_right(parent, new_node);
            if (parent == NodeTraits::get_right(header)) {
                NodeTraits::set_right(header, new_node);
            }
        }
        NodeTraits::set_parent(new_node, parent);
        NodeTraits::set_left(new_node, NodePtr{});
        NodeTraits::set_right(new_node, NodePtr{});
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_BSTREE_ALGORITHMS_HPP
