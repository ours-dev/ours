/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_RBTREE_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_RBTREE_ALGORITHMS_HPP 1

#include <cstddef>

#include <ustl/collections/intrusive/bstree_algorithms.hpp>

namespace ustl::collections::intrusive {
    template <typename TNodeTraits>
    class RBTreeAlgorithms final : public BstreeAlgorithms<TNodeTraits> {
        using BstreeAlgo = BstreeAlgorithms<TNodeTraits>;

    public:
        using Node = typename BstreeAlgo::Node;
        using NodeTraits = TNodeTraits;
        using NodePtr = typename BstreeAlgo::NodePtr;
        using ConstNodePtr = typename BstreeAlgo::ConstNodePtr;
        using InsertCommitData = typename BstreeAlgo::InsertCommitData;
        using DataForRebalance = typename BstreeAlgo::DataForRebalance;
        using Color = typename NodeTraits::Color;

        using BstreeAlgo::begin_node;
        using BstreeAlgo::bounded_range;
        using BstreeAlgo::count;
        using BstreeAlgo::end_node;
        using BstreeAlgo::equal_range;
        using BstreeAlgo::find;
        using BstreeAlgo::get_header;
        using BstreeAlgo::insert_unique_check;
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

        static constexpr auto init(NodePtr const node) noexcept -> void {
            BstreeAlgo::init(node);
            NodeTraits::set_color(node, NodeTraits::black());
        }

        static constexpr auto init_header(NodePtr const header) noexcept -> void {
            BstreeAlgo::init_header(header);
            NodeTraits::set_color(header, NodeTraits::red());
        }

        [[nodiscard]] static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
            return BstreeAlgo::inited(node);
        }

        [[nodiscard]] static constexpr auto is_header(ConstNodePtr const node) noexcept -> bool {
            return NodeTraits::get_color(node) == NodeTraits::red() && BstreeAlgo::is_header(node);
        }

        static constexpr auto swap_nodes(NodePtr const node_1, NodePtr const node_2) noexcept -> void {
            if (node_1 == node_2) {
                return;
            }
            swap_nodes(node_1, BstreeAlgo::get_header(node_1), node_2, BstreeAlgo::get_header(node_2));
        }

        static constexpr auto swap_nodes(NodePtr const node_1, NodePtr const header_1, NodePtr const node_2,
                                         NodePtr const header_2) noexcept -> void {
            if (node_1 == node_2) {
                return;
            }
            const auto color = NodeTraits::get_color(node_1);
            BstreeAlgo::swap_nodes(node_1, header_1, node_2, header_2);
            NodeTraits::set_color(node_1, NodeTraits::get_color(node_2));
            NodeTraits::set_color(node_2, color);
        }

        template <typename NodePointerCompare>
        static constexpr auto insert_equal_upper_bound(NodePtr const header, NodePtr const new_node,
                                                       NodePointerCompare compare,
                                                       std::size_t *const depth = nullptr) noexcept -> NodePtr {
            BstreeAlgo::insert_equal_upper_bound(header, new_node, compare, depth);
            rebalance_after_insertion(header, new_node);
            return new_node;
        }

        template <typename NodePointerCompare>
        static constexpr auto insert_equal_lower_bound(NodePtr const header, NodePtr const new_node,
                                                       NodePointerCompare compare,
                                                       std::size_t *const depth = nullptr) noexcept -> NodePtr {
            BstreeAlgo::insert_equal_lower_bound(header, new_node, compare, depth);
            rebalance_after_insertion(header, new_node);
            return new_node;
        }

        template <typename NodePointerCompare>
        static constexpr auto insert_equal(NodePtr const header, NodePtr const hint, NodePtr const new_node,
                                           NodePointerCompare compare, std::size_t *const depth = nullptr) noexcept
            -> NodePtr {
            BstreeAlgo::insert_equal(header, hint, new_node, compare, depth);
            rebalance_after_insertion(header, new_node);
            return new_node;
        }

        static constexpr auto insert_before(NodePtr const header, NodePtr const position, NodePtr const new_node,
                                            std::size_t *const depth = nullptr) noexcept -> NodePtr {
            BstreeAlgo::insert_before(header, position, new_node, depth);
            rebalance_after_insertion(header, new_node);
            return new_node;
        }

        static constexpr auto push_back(NodePtr const header, NodePtr const new_node,
                                        std::size_t *const depth = nullptr) noexcept -> void {
            BstreeAlgo::push_back(header, new_node, depth);
            rebalance_after_insertion(header, new_node);
        }

        static constexpr auto push_front(NodePtr const header, NodePtr const new_node,
                                         std::size_t *const depth = nullptr) noexcept -> void {
            BstreeAlgo::push_front(header, new_node, depth);
            rebalance_after_insertion(header, new_node);
        }

        static constexpr auto insert_unique_commit(NodePtr const header, NodePtr const new_node,
                                                   InsertCommitData const &commit_data) noexcept -> void {
            BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
            rebalance_after_insertion(header, new_node);
        }

        [[nodiscard]] static constexpr auto erase(NodePtr const header, NodePtr const node) noexcept -> NodePtr {
            DataForRebalance info {};
            BstreeAlgo::erase(header, node, info);
            rebalance_after_erasure(header, node, info);
            return node;
        }

        static constexpr auto unlink(NodePtr const node) noexcept -> void {
            if (!unique(node)) {
                static_cast<void>(erase(BstreeAlgo::get_header(node), node));
            }
        }

        template <typename Disposer>
        static constexpr auto clear_and_dispose(NodePtr const header, Disposer disposer) noexcept -> void {
            while (!unique(header)) {
                const auto node = begin_node(header);
                erase(header, node);
                disposer(node);
            }
        }

    private:
        static constexpr auto rebalance_after_erasure(NodePtr const header, NodePtr const erased,
                                                      DataForRebalance const &info) noexcept -> void {
            auto removed_color = NodeTraits::get_color(erased);
            if (info.m_y != erased) {
                removed_color = NodeTraits::get_color(info.m_y);
                NodeTraits::set_color(info.m_y, NodeTraits::get_color(erased));
            }
            if (removed_color != NodeTraits::red()) {
                rebalance_after_erasure_restore_invariants(header, info.m_x, info.m_x_parent);
            }
        }

        static constexpr auto rebalance_after_erasure_restore_invariants(NodePtr const header, NodePtr child,
                                                                         NodePtr parent) noexcept -> void {
            while (parent != header &&
                   (child == NodePtr {} || NodeTraits::get_color(child) == NodeTraits::black())) {
                if (child == NodeTraits::get_left(parent)) {
                    auto sibling = NodeTraits::get_right(parent);
                    if (NodeTraits::get_color(sibling) == NodeTraits::red()) {
                        NodeTraits::set_color(sibling, NodeTraits::black());
                        NodeTraits::set_color(parent, NodeTraits::red());
                        BstreeAlgo::rotate_left(parent, sibling, NodeTraits::get_parent(parent), header);
                        sibling = NodeTraits::get_right(parent);
                    }
                    const auto sibling_left = NodeTraits::get_left(sibling);
                    const auto sibling_right = NodeTraits::get_right(sibling);
                    if ((sibling_left == NodePtr {} || NodeTraits::get_color(sibling_left) == NodeTraits::black()) &&
                        (sibling_right == NodePtr {} || NodeTraits::get_color(sibling_right) == NodeTraits::black())) {
                        NodeTraits::set_color(sibling, NodeTraits::red());
                        child = parent;
                        parent = NodeTraits::get_parent(parent);
                    } else {
                        if (sibling_right == NodePtr {} || NodeTraits::get_color(sibling_right) == NodeTraits::black()) {
                            NodeTraits::set_color(sibling_left, NodeTraits::black());
                            NodeTraits::set_color(sibling, NodeTraits::red());
                            BstreeAlgo::rotate_right(sibling, sibling_left, NodeTraits::get_parent(sibling), header);
                            sibling = NodeTraits::get_right(parent);
                        }
                        NodeTraits::set_color(sibling, NodeTraits::get_color(parent));
                        NodeTraits::set_color(parent, NodeTraits::black());
                        const auto new_sibling_right = NodeTraits::get_right(sibling);
                        if (new_sibling_right != NodePtr {}) {
                            NodeTraits::set_color(new_sibling_right, NodeTraits::black());
                        }
                        BstreeAlgo::rotate_left(parent, sibling, NodeTraits::get_parent(parent), header);
                        child = NodeTraits::get_parent(header);
                        parent = header;
                    }
                } else {
                    auto sibling = NodeTraits::get_left(parent);
                    if (NodeTraits::get_color(sibling) == NodeTraits::red()) {
                        NodeTraits::set_color(sibling, NodeTraits::black());
                        NodeTraits::set_color(parent, NodeTraits::red());
                        BstreeAlgo::rotate_right(parent, sibling, NodeTraits::get_parent(parent), header);
                        sibling = NodeTraits::get_left(parent);
                    }
                    const auto sibling_left = NodeTraits::get_left(sibling);
                    const auto sibling_right = NodeTraits::get_right(sibling);
                    if ((sibling_left == NodePtr {} || NodeTraits::get_color(sibling_left) == NodeTraits::black()) &&
                        (sibling_right == NodePtr {} || NodeTraits::get_color(sibling_right) == NodeTraits::black())) {
                        NodeTraits::set_color(sibling, NodeTraits::red());
                        child = parent;
                        parent = NodeTraits::get_parent(parent);
                    } else {
                        if (sibling_left == NodePtr {} || NodeTraits::get_color(sibling_left) == NodeTraits::black()) {
                            NodeTraits::set_color(sibling_right, NodeTraits::black());
                            NodeTraits::set_color(sibling, NodeTraits::red());
                            BstreeAlgo::rotate_left(sibling, sibling_right, NodeTraits::get_parent(sibling), header);
                            sibling = NodeTraits::get_left(parent);
                        }
                        NodeTraits::set_color(sibling, NodeTraits::get_color(parent));
                        NodeTraits::set_color(parent, NodeTraits::black());
                        const auto new_sibling_left = NodeTraits::get_left(sibling);
                        if (new_sibling_left != NodePtr {}) {
                            NodeTraits::set_color(new_sibling_left, NodeTraits::black());
                        }
                        BstreeAlgo::rotate_right(parent, sibling, NodeTraits::get_parent(parent), header);
                        child = NodeTraits::get_parent(header);
                        parent = header;
                    }
                }
            }
            if (child != NodePtr {}) {
                NodeTraits::set_color(child, NodeTraits::black());
            }
        }

        static constexpr auto rebalance_after_insertion(NodePtr const header, NodePtr node) noexcept -> void {
            NodeTraits::set_color(node, NodeTraits::red());
            while (true) {
                auto parent = NodeTraits::get_parent(node);
                const auto grandparent = NodeTraits::get_parent(parent);
                if (parent == header || NodeTraits::get_color(parent) == NodeTraits::black() || grandparent == header) {
                    break;
                }
                NodeTraits::set_color(grandparent, NodeTraits::red());
                const auto grandparent_left = NodeTraits::get_left(grandparent);
                const auto parent_is_left = parent == grandparent_left;
                const auto uncle = parent_is_left ? NodeTraits::get_right(grandparent) : grandparent_left;
                if (uncle != NodePtr {} && NodeTraits::get_color(uncle) == NodeTraits::red()) {
                    NodeTraits::set_color(uncle, NodeTraits::black());
                    NodeTraits::set_color(parent, NodeTraits::black());
                    node = grandparent;
                    continue;
                }
                const auto node_is_left = NodeTraits::get_left(parent) == node;
                if (parent_is_left) {
                    if (!node_is_left) {
                        BstreeAlgo::rotate_left_no_parent_fix(parent, node);
                        parent = node;
                    }
                    BstreeAlgo::rotate_right(grandparent, parent, NodeTraits::get_parent(grandparent), header);
                } else {
                    if (node_is_left) {
                        BstreeAlgo::rotate_right_no_parent_fix(parent, node);
                        parent = node;
                    }
                    BstreeAlgo::rotate_left(grandparent, parent, NodeTraits::get_parent(grandparent), header);
                }
                NodeTraits::set_color(parent, NodeTraits::black());
                break;
            }
            NodeTraits::set_color(NodeTraits::get_parent(header), NodeTraits::black());
        }
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_RBTREE_ALGORITHMS_HPP
