/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_AVLTREE_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_AVLTREE_ALGORITHMS_HPP 1

#include <cstddef>

#include <ustl/collections/intrusive/bstree_algorithms.hpp>

namespace ustl::collections::intrusive {
template <typename TNodeTraits>
class AvltreeAlgorithms final: public BstreeAlgorithms<TNodeTraits> {
    using BstreeAlgo = BstreeAlgorithms<TNodeTraits>;

  public:
    using Node = typename TNodeTraits::Node;
    using NodeTraits = TNodeTraits;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;
    using Balance = typename NodeTraits::Balance;
    using InsertCommitData = typename BstreeAlgo::InsertCommitData;
    using DataForRebalance = typename BstreeAlgo::DataForRebalance;

    using BstreeAlgo::begin_node;
    using BstreeAlgo::bounded_range;
    using BstreeAlgo::count;
    using BstreeAlgo::end_node;
    using BstreeAlgo::equal_range;
    using BstreeAlgo::find;
    using BstreeAlgo::get_header;
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
        NodeTraits::set_balance(node, NodeTraits::zero());
    }

    static constexpr auto init_header(NodePtr const header) noexcept -> void {
        BstreeAlgo::init_header(header);
        NodeTraits::set_balance(header, NodeTraits::zero());
    }

    [[nodiscard]]
    static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
        return BstreeAlgo::inited(node);
    }

    [[nodiscard]]
    static constexpr auto is_header(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_balance(node) == NodeTraits::zero() && BstreeAlgo::is_header(node);
    }

    static constexpr auto swap_nodes(NodePtr const left, NodePtr const right) noexcept -> void {
        if (left == right) {
            return;
        }
        swap_nodes(left, BstreeAlgo::get_header(left), right, BstreeAlgo::get_header(right));
    }

    static constexpr auto swap_nodes(NodePtr const left, NodePtr const left_header,
                                     NodePtr const right, NodePtr const right_header) noexcept
        -> void {
        if (left == right) {
            return;
        }
        BstreeAlgo::swap_nodes(left, left_header, right, right_header);
        const auto balance = NodeTraits::get_balance(left);
        NodeTraits::set_balance(left, NodeTraits::get_balance(right));
        NodeTraits::set_balance(right, balance);
    }

    static constexpr auto replace_node(NodePtr const old_node, NodePtr const new_node) noexcept
        -> void {
        replace_node(old_node, BstreeAlgo::get_header(old_node), new_node);
    }

    static constexpr auto replace_node(NodePtr const old_node, NodePtr const header,
                                       NodePtr const new_node) noexcept -> void {
        BstreeAlgo::replace_node(old_node, header, new_node);
        NodeTraits::set_balance(new_node, NodeTraits::get_balance(old_node));
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal_upper_bound(NodePtr const header, NodePtr const new_node,
                                                   NodePointerCompare compare,
                                                   std::size_t *const depth = nullptr) noexcept
        -> NodePtr {
        BstreeAlgo::insert_equal_upper_bound(header, new_node, compare, depth);
        rebalance_after_insertion(header, new_node);
        return new_node;
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal_lower_bound(NodePtr const header, NodePtr const new_node,
                                                   NodePointerCompare compare,
                                                   std::size_t *const depth = nullptr) noexcept
        -> NodePtr {
        BstreeAlgo::insert_equal_lower_bound(header, new_node, compare, depth);
        rebalance_after_insertion(header, new_node);
        return new_node;
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal(NodePtr const header, NodePtr const hint,
                                       NodePtr const new_node, NodePointerCompare compare,
                                       std::size_t *const depth = nullptr) noexcept -> NodePtr {
        BstreeAlgo::insert_equal(header, hint, new_node, compare, depth);
        rebalance_after_insertion(header, new_node);
        return new_node;
    }

    static constexpr auto insert_before(NodePtr const header, NodePtr const position,
                                        NodePtr const new_node,
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
                                               InsertCommitData const &commit_data) noexcept
        -> void {
        BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
        rebalance_after_insertion(header, new_node);
    }

    [[nodiscard]]
    static constexpr auto erase(NodePtr const header, NodePtr const node) noexcept -> NodePtr {
        DataForRebalance info{};
        BstreeAlgo::erase(header, node, info);
        rebalance_after_erasure(header, node, info);
        return node;
    }

    static constexpr auto unlink(NodePtr const node) noexcept -> void {
        auto header = NodeTraits::get_parent(node);
        if (header == NodePtr{}) {
            return;
        }
        while (!is_header(header)) {
            header = NodeTraits::get_parent(header);
        }
        static_cast<void>(erase(header, node));
    }

  private:
    static constexpr auto rebalance_after_erasure(NodePtr const header, NodePtr const erased,
                                                  DataForRebalance const &info) noexcept -> void {
        if (info.m_y != erased) {
            NodeTraits::set_balance(info.m_y, NodeTraits::get_balance(erased));
        }
        rebalance_after_erasure_restore_invariants(header, info.m_x, info.m_x_parent);
    }

    static constexpr auto rebalance_after_erasure_restore_invariants(NodePtr const header,
                                                                     NodePtr child,
                                                                     NodePtr parent) noexcept
        -> void {
        for (auto root = NodeTraits::get_parent(header); child != root;
             root = NodeTraits::get_parent(header), parent = NodeTraits::get_parent(child)) {
            const auto parent_balance = NodeTraits::get_balance(parent);
            const auto parent_left = NodeTraits::get_left(parent);
            const auto parent_right = NodeTraits::get_right(parent);
            if (parent_balance == NodeTraits::zero()) {
                NodeTraits::set_balance(parent, child == parent_right ? NodeTraits::negative()
                                                                      : NodeTraits::positive());
                break;
            }
            if (parent_balance == NodeTraits::negative()) {
                if (child == parent_left) {
                    NodeTraits::set_balance(parent, NodeTraits::zero());
                    child = parent;
                } else {
                    if (NodeTraits::get_balance(parent_left) == NodeTraits::positive()) {
                        child = rotate_left_right(parent, parent_left, header);
                    } else {
                        rotate_right(parent, parent_left, header);
                        child = parent_left;
                    }
                    if (NodeTraits::get_balance(child) == NodeTraits::positive()) {
                        break;
                    }
                }
                continue;
            }
            if (child == parent_right) {
                NodeTraits::set_balance(parent, NodeTraits::zero());
                child = parent;
            } else {
                if (NodeTraits::get_balance(parent_right) == NodeTraits::negative()) {
                    child = rotate_right_left(parent, parent_right, header);
                } else {
                    rotate_left(parent, parent_right, header);
                    child = parent_right;
                }
                if (NodeTraits::get_balance(child) == NodeTraits::negative()) {
                    break;
                }
            }
        }
    }

    static constexpr auto rebalance_after_insertion(NodePtr const header, NodePtr child) noexcept
        -> void {
        NodeTraits::set_balance(child, NodeTraits::zero());
        for (auto root = NodeTraits::get_parent(header); child != root;
             root = NodeTraits::get_parent(header)) {
            const auto parent = NodeTraits::get_parent(child);
            const auto parent_left = NodeTraits::get_left(parent);
            const auto parent_balance = NodeTraits::get_balance(parent);
            const auto child_is_left = child == parent_left;
            if (parent_balance == NodeTraits::zero()) {
                NodeTraits::set_balance(parent, child_is_left ? NodeTraits::negative()
                                                              : NodeTraits::positive());
                child = parent;
                continue;
            }
            if (parent_balance == NodeTraits::positive()) {
                if (child_is_left) {
                    NodeTraits::set_balance(parent, NodeTraits::zero());
                } else if (NodeTraits::get_balance(child) == NodeTraits::negative()) {
                    static_cast<void>(rotate_right_left(parent, child, header));
                } else {
                    rotate_left(parent, child, header);
                }
                break;
            }
            if (child_is_left) {
                if (NodeTraits::get_balance(child) == NodeTraits::positive()) {
                    static_cast<void>(rotate_left_right(parent, child, header));
                } else {
                    rotate_right(parent, child, header);
                }
            } else {
                NodeTraits::set_balance(parent, NodeTraits::zero());
            }
            break;
        }
    }

    static constexpr auto set_double_rotation_balance(NodePtr const outer, NodePtr const inner,
                                                      NodePtr const root) noexcept -> void {
        const auto root_balance = NodeTraits::get_balance(root);
        NodeTraits::set_balance(root, NodeTraits::zero());
        if (root_balance == NodeTraits::negative()) {
            NodeTraits::set_balance(outer, NodeTraits::positive());
            NodeTraits::set_balance(inner, NodeTraits::zero());
        } else if (root_balance == NodeTraits::positive()) {
            NodeTraits::set_balance(outer, NodeTraits::zero());
            NodeTraits::set_balance(inner, NodeTraits::negative());
        } else {
            NodeTraits::set_balance(outer, NodeTraits::zero());
            NodeTraits::set_balance(inner, NodeTraits::zero());
        }
    }

    [[nodiscard]]
    static constexpr auto rotate_left_right(NodePtr const root, NodePtr const left,
                                            NodePtr const header) noexcept -> NodePtr {
        const auto pivot = NodeTraits::get_right(left);
        BstreeAlgo::rotate_left_no_parent_fix(left, pivot);
        BstreeAlgo::rotate_right(root, pivot, NodeTraits::get_parent(root), header);
        set_double_rotation_balance(root, left, pivot);
        return pivot;
    }

    [[nodiscard]]
    static constexpr auto rotate_right_left(NodePtr const root, NodePtr const right,
                                            NodePtr const header) noexcept -> NodePtr {
        const auto pivot = NodeTraits::get_left(right);
        BstreeAlgo::rotate_right_no_parent_fix(right, pivot);
        BstreeAlgo::rotate_left(root, pivot, NodeTraits::get_parent(root), header);
        set_double_rotation_balance(right, root, pivot);
        return pivot;
    }

    static constexpr auto rotate_left(NodePtr const root, NodePtr const right,
                                      NodePtr const header) noexcept -> void {
        BstreeAlgo::rotate_left(root, right, NodeTraits::get_parent(root), header);
        if (NodeTraits::get_balance(right) == NodeTraits::positive()) {
            NodeTraits::set_balance(root, NodeTraits::zero());
            NodeTraits::set_balance(right, NodeTraits::zero());
        } else {
            NodeTraits::set_balance(root, NodeTraits::positive());
            NodeTraits::set_balance(right, NodeTraits::negative());
        }
    }

    static constexpr auto rotate_right(NodePtr const root, NodePtr const left,
                                       NodePtr const header) noexcept -> void {
        BstreeAlgo::rotate_right(root, left, NodeTraits::get_parent(root), header);
        if (NodeTraits::get_balance(left) == NodeTraits::negative()) {
            NodeTraits::set_balance(root, NodeTraits::zero());
            NodeTraits::set_balance(left, NodeTraits::zero());
        } else {
            NodeTraits::set_balance(root, NodeTraits::negative());
            NodeTraits::set_balance(left, NodeTraits::positive());
        }
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_AVLTREE_ALGORITHMS_HPP
