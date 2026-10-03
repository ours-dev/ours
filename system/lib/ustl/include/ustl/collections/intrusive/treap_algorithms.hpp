/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_TREAP_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_TREAP_ALGORITHMS_HPP 1

#include <ustl/collections/intrusive/bstree_algorithms.hpp>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename TNodeTraits>
class TreapAlgorithms final: public BstreeAlgorithms<TNodeTraits> {
    using BstreeAlgo = BstreeAlgorithms<TNodeTraits>;

  public:
    using Node = typename TNodeTraits::Node;
    using NodeTraits = TNodeTraits;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;

    struct InsertCommitData final: BstreeAlgo::InsertCommitData {
        std::size_t m_rotations{};
    };

    using BstreeAlgo::begin_node;
    using BstreeAlgo::bounded_range;
    using BstreeAlgo::count;
    using BstreeAlgo::end_node;
    using BstreeAlgo::equal_range;
    using BstreeAlgo::find;
    using BstreeAlgo::get_header;
    using BstreeAlgo::init;
    using BstreeAlgo::init_header;
    using BstreeAlgo::inited;
    using BstreeAlgo::is_header;
    using BstreeAlgo::lower_bound;
    using BstreeAlgo::lower_bound_range;
    using BstreeAlgo::maximum;
    using BstreeAlgo::minimum;
    using BstreeAlgo::next_node;
    using BstreeAlgo::prev_node;
    using BstreeAlgo::root_node;
    using BstreeAlgo::size;
    using BstreeAlgo::subtree_size;
    using BstreeAlgo::unique;
    using BstreeAlgo::upper_bound;

    template <typename Key, typename KeyNodePointerCompare, typename NodePointerPriorityCompare>
    [[nodiscard]]
    static constexpr auto insert_unique_check(ConstNodePtr const header, Key const &key,
                                              KeyNodePointerCompare compare,
                                              NodePointerPriorityCompare priority_compare,
                                              InsertCommitData &commit_data) noexcept
        -> std::pair<NodePtr, bool> {
        const auto result = BstreeAlgo::insert_unique_check(header, key, compare, commit_data);
        if (result.second) {
            count_promotions(header, commit_data.m_node, key, priority_compare,
                             commit_data.m_rotations);
        }
        return result;
    }

    template <typename NodePointerCompare, typename NodePointerPriorityCompare>
    static constexpr auto
    insert_equal_upper_bound(NodePtr const header, NodePtr const new_node,
                             NodePointerCompare compare,
                             NodePointerPriorityCompare priority_compare) noexcept -> NodePtr {
        InsertCommitData commit_data{};
        BstreeAlgo::insert_equal_upper_bound_check(header, new_node, compare, commit_data, nullptr);
        commit_with_rebalance(header, new_node, priority_compare, commit_data);
        return new_node;
    }

    static constexpr auto insert_unique_commit(NodePtr const header, NodePtr const new_node,
                                               InsertCommitData const &commit_data) noexcept
        -> void {
        BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
        rotate_up(header, new_node, commit_data.m_rotations);
    }

    template <typename NodePointerPriorityCompare>
    [[nodiscard]]
    static constexpr auto erase(NodePtr const header, NodePtr const node,
                                NodePointerPriorityCompare priority_compare) noexcept -> NodePtr {
        rotate_down(header, node, priority_compare);
        BstreeAlgo::erase(header, node);
        return node;
    }

    template <typename NodePointerPriorityCompare>
    static constexpr auto unlink(NodePtr const node,
                                 NodePointerPriorityCompare priority_compare) noexcept -> void {
        if (!unique(node)) {
            auto _ = erase(get_header(node), node, priority_compare);
        }
    }

  private:
    template <typename Key, typename KeyNodePointerPriorityCompare>
    static constexpr auto count_promotions(ConstNodePtr const header, NodePtr const parent,
                                           Key const &key,
                                           KeyNodePointerPriorityCompare priority_compare,
                                           std::size_t &rotations) noexcept -> void {
        rotations = 0U;
        auto current = parent;
        while (current != header && priority_compare(key, current)) {
            ++rotations;
            current = NodeTraits::get_parent(current);
        }
    }

    template <typename NodePointerPriorityCompare>
    static constexpr auto commit_with_rebalance(NodePtr const header, NodePtr const new_node,
                                                NodePointerPriorityCompare priority_compare,
                                                InsertCommitData &commit_data) noexcept -> void {
        count_promotions(header, commit_data.m_node, new_node, priority_compare,
                         commit_data.m_rotations);
        insert_unique_commit(header, new_node, commit_data);
    }

    static constexpr auto rotate_up(NodePtr const header, NodePtr node,
                                    std::size_t rotations) noexcept -> void {
        while (rotations != 0U) {
            const auto parent = NodeTraits::get_parent(node);
            const auto grandparent = NodeTraits::get_parent(parent);
            if (NodeTraits::get_left(parent) == node) {
                BstreeAlgo::rotate_right(parent, node, grandparent, header);
            } else {
                BstreeAlgo::rotate_left(parent, node, grandparent, header);
            }
            --rotations;
        }
    }

    template <typename NodePointerPriorityCompare>
    static constexpr auto rotate_down(NodePtr const header, NodePtr const node,
                                      NodePointerPriorityCompare priority_compare) noexcept
        -> void {
        while (NodeTraits::get_left(node) != NodePtr{} ||
               NodeTraits::get_right(node) != NodePtr{}) {
            const auto left = NodeTraits::get_left(node);
            const auto right = NodeTraits::get_right(node);
            const auto child =
                right == NodePtr{} || (left != NodePtr{} && priority_compare(left, right)) ? left
                                                                                           : right;
            const auto parent = NodeTraits::get_parent(node);
            if (child == left) {
                BstreeAlgo::rotate_right(node, child, parent, header);
            } else {
                BstreeAlgo::rotate_left(node, child, parent, header);
            }
        }
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_TREAP_ALGORITHMS_HPP
