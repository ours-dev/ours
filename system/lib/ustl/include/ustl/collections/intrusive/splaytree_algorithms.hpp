/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP 1

#include <cstddef>

#include <ustl/collections/intrusive/bstree_algorithms.hpp>

namespace ustl::collections::intrusive {
    template <typename TNodeTraits>
    class SplaytreeAlgorithms final : public BstreeAlgorithms<TNodeTraits> {
        using BstreeAlgo = BstreeAlgorithms<TNodeTraits>;

    public:
        using Node = typename TNodeTraits::Node;
        using NodeTraits = TNodeTraits;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;
        using InsertCommitData = typename BstreeAlgo::InsertCommitData;

        using BstreeAlgo::begin_node;
        using BstreeAlgo::bounded_range;
        using BstreeAlgo::count;
        using BstreeAlgo::end_node;
        using BstreeAlgo::equal_range;
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
        [[nodiscard]] static constexpr auto find(ConstNodePtr const header, Key const &key,
                                                 KeyNodePointerCompare compare) noexcept -> NodePtr {
            const auto node = BstreeAlgo::find(header, key, compare);
            if (node != end_node(header)) {
                splay_up(node, end_node(header));
            }
            return node;
        }

        template <typename NodePointerCompare>
        static constexpr auto insert_equal_upper_bound(NodePtr const header, NodePtr const new_node,
                                                       NodePointerCompare compare,
                                                       std::size_t *const depth = nullptr) noexcept -> NodePtr {
            BstreeAlgo::insert_equal_upper_bound(header, new_node, compare, depth);
            splay_up(new_node, header);
            return new_node;
        }

        template <typename NodePointerCompare>
        static constexpr auto insert_equal_lower_bound(NodePtr const header, NodePtr const new_node,
                                                       NodePointerCompare compare,
                                                       std::size_t *const depth = nullptr) noexcept -> NodePtr {
            BstreeAlgo::insert_equal_lower_bound(header, new_node, compare, depth);
            splay_up(new_node, header);
            return new_node;
        }

        template <typename NodePointerCompare>
        static constexpr auto insert_equal(NodePtr const header, NodePtr const hint, NodePtr const new_node,
                                           NodePointerCompare compare, std::size_t *const depth = nullptr) noexcept
            -> NodePtr {
            BstreeAlgo::insert_equal(header, hint, new_node, compare, depth);
            splay_up(new_node, header);
            return new_node;
        }

        static constexpr auto insert_unique_commit(NodePtr const header, NodePtr const new_node,
                                                   InsertCommitData const &commit_data) noexcept -> void {
            BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
            splay_up(new_node, header);
        }

        [[nodiscard]] static constexpr auto erase(NodePtr const header, NodePtr const node) noexcept -> NodePtr {
            if (NodeTraits::get_left(node) != NodePtr {}) {
                splay_up(prev_node(node), header);
            }
            BstreeAlgo::erase(header, node);
            return node;
        }

        static constexpr auto unlink(NodePtr const node) noexcept -> void {
            if (!unique(node)) {
                static_cast<void>(erase(get_header(node), node));
            }
        }

    private:
        static constexpr auto splay_up(NodePtr node, NodePtr const header) noexcept -> void {
            while (NodeTraits::get_parent(node) != header) {
                const auto parent = NodeTraits::get_parent(node);
                const auto grandparent = NodeTraits::get_parent(parent);
                if (grandparent == header) {
                    if (NodeTraits::get_left(parent) == node) {
                        BstreeAlgo::rotate_right(parent, node, header, header);
                    } else {
                        BstreeAlgo::rotate_left(parent, node, header, header);
                    }
                    continue;
                }

                const auto great_grandparent = NodeTraits::get_parent(grandparent);
                const auto node_is_left = NodeTraits::get_left(parent) == node;
                const auto parent_is_left = NodeTraits::get_left(grandparent) == parent;
                if (node_is_left && parent_is_left) {
                    BstreeAlgo::rotate_right(grandparent, parent, great_grandparent, header);
                    BstreeAlgo::rotate_right(parent, node, great_grandparent, header);
                } else if (!node_is_left && !parent_is_left) {
                    BstreeAlgo::rotate_left(grandparent, parent, great_grandparent, header);
                    BstreeAlgo::rotate_left(parent, node, great_grandparent, header);
                } else if (node_is_left) {
                    BstreeAlgo::rotate_right(parent, node, grandparent, header);
                    BstreeAlgo::rotate_left(grandparent, node, great_grandparent, header);
                } else {
                    BstreeAlgo::rotate_left(parent, node, grandparent, header);
                    BstreeAlgo::rotate_right(grandparent, node, great_grandparent, header);
                }
            }
        }
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
