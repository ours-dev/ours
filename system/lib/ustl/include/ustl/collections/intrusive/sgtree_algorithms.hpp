/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SGTREE_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SGTREE_ALGORITHMS_HPP 1

#include <utility>

#include <cstddef>

#include <ustl/collections/intrusive/bstree_algorithms.hpp>

namespace ustl::collections::intrusive {
template <typename TNodeTraits>
class SgtreeAlgorithms final: public BstreeAlgorithms<TNodeTraits> {
    using BstreeAlgo = BstreeAlgorithms<TNodeTraits>;

  public:
    using Node = typename TNodeTraits::Node;
    using NodeTraits = TNodeTraits;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;

    struct InsertCommitData final: BstreeAlgo::InsertCommitData {
        std::size_t m_depth{};
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

    template <typename Key, typename KeyNodePointerCompare>
    [[nodiscard]]
    static constexpr auto insert_unique_check(ConstNodePtr const header, Key const &key,
                                              KeyNodePointerCompare compare,
                                              InsertCommitData &commit_data) noexcept
        -> std::pair<NodePtr, bool> {
        const auto result = BstreeAlgo::insert_unique_check(header, key, compare, commit_data,
                                                            &commit_data.m_depth);
        return result;
    }

    template <typename HeightLimit>
    static constexpr auto
    insert_unique_commit(NodePtr const header, NodePtr const new_node,
                         InsertCommitData const &commit_data, std::size_t const tree_size,
                         HeightLimit height_limit, std::size_t &max_tree_size) noexcept -> void {
        BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
        rebalance_after_insertion(header, commit_data.m_depth, tree_size + 1U, height_limit,
                                  max_tree_size);
    }

    template <typename NodePointerCompare, typename HeightLimit>
    static constexpr auto insert_equal_upper_bound(NodePtr const header, NodePtr const new_node,
                                                   NodePointerCompare compare,
                                                   std::size_t const tree_size,
                                                   HeightLimit height_limit,
                                                   std::size_t &max_tree_size) noexcept -> NodePtr {
        std::size_t depth{};
        BstreeAlgo::insert_equal_upper_bound(header, new_node, compare, &depth);
        rebalance_after_insertion(header, depth, tree_size + 1U, height_limit, max_tree_size);
        return new_node;
    }

    template <typename AlphaByMaxSize>
    [[nodiscard]]
    static constexpr auto erase(NodePtr const header, NodePtr const node,
                                std::size_t const tree_size, std::size_t &max_tree_size,
                                AlphaByMaxSize alpha_by_max_size) noexcept -> NodePtr {
        BstreeAlgo::erase(header, node);
        const auto remaining_size = tree_size - 1U;
        if (remaining_size > 0U && remaining_size < alpha_by_max_size(max_tree_size)) {
            rebalance(header, remaining_size);
            max_tree_size = remaining_size;
        }
        return node;
    }

    static constexpr auto rebalance(NodePtr const header, std::size_t const tree_size) noexcept
        -> void {
        if (tree_size < 2U) {
            return;
        }
        rebalance_subtree(header, NodeTraits::get_parent(header), tree_size);
    }

  private:
    template <typename HeightLimit>
    static constexpr auto rebalance_after_insertion(NodePtr const header, std::size_t const depth,
                                                    std::size_t const tree_size,
                                                    HeightLimit height_limit,
                                                    std::size_t &max_tree_size) noexcept -> void {
        if (tree_size > max_tree_size) {
            max_tree_size = tree_size;
        }
        if (tree_size > 2U && depth > height_limit(tree_size)) {
            rebalance(header, tree_size);
        }
    }

    static constexpr auto rebalance_subtree(NodePtr const header, NodePtr const root,
                                            std::size_t const tree_size) noexcept -> void {
        const auto parent = NodeTraits::get_parent(root);
        const auto root_is_left_child = parent != header && NodeTraits::get_left(parent) == root;
        make_vine(header, parent, root);

        auto perfect_size = std::size_t{1U};
        while (perfect_size <= tree_size / 2U) {
            perfect_size = perfect_size * 2U + 1U;
        }
        compress(header, parent, tree_size - perfect_size);
        while (perfect_size > 1U) {
            perfect_size /= 2U;
            compress(header, parent, perfect_size);
        }

        if (parent == header) {
            NodeTraits::set_parent(NodeTraits::get_parent(header), header);
        } else {
            const auto rebuilt_root =
                root_is_left_child ? NodeTraits::get_left(parent) : NodeTraits::get_right(parent);
            NodeTraits::set_parent(rebuilt_root, parent);
        }
    }

    static constexpr auto make_vine(NodePtr const header, NodePtr parent, NodePtr node) noexcept
        -> void {
        while (node != NodePtr{}) {
            const auto left = NodeTraits::get_left(node);
            if (left != NodePtr{}) {
                BstreeAlgo::rotate_right(node, left, parent, header);
                node = left;
            } else {
                parent = node;
                node = NodeTraits::get_right(node);
            }
        }
    }

    static constexpr auto compress(NodePtr const header, NodePtr parent,
                                   std::size_t rotations) noexcept -> void {
        auto node = parent == header ? NodeTraits::get_parent(header)
                                     : (NodeTraits::get_left(parent) != NodePtr{}
                                            ? NodeTraits::get_left(parent)
                                            : NodeTraits::get_right(parent));
        while (rotations != 0U && node != NodePtr{}) {
            const auto right = NodeTraits::get_right(node);
            if (right == NodePtr{}) {
                return;
            }
            BstreeAlgo::rotate_left(node, right, parent, header);
            parent = right;
            node = NodeTraits::get_right(right);
            --rotations;
        }
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_SGTREE_ALGORITHMS_HPP
