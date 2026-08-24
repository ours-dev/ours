/// Copyright(C) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef USTL_COLLECTIONS_DETAIL_BTREE_NODE_ALGORITHMS_HPP
#define USTL_COLLECTIONS_DETAIL_BTREE_NODE_ALGORITHMS_HPP 1

#include <cstddef>

namespace ustl::collections::priv {
    using Size = std::size_t;

    inline constexpr auto DEFAULT_BTREE_ORDER = Size(8);

    template <typename NodeTraitsType>
    struct BTreeNodeAlgorithmsImpl {
        using NodeTraits = NodeTraitsType;
        using Node = typename NodeTraits::Node;
        using NodePtr = typename NodeTraits::NodePtr;
        using NodePtrMut = typename NodeTraits::NodePtrMut;

        enum class RebalancePlan {
            BorrowFromPrev,
            BorrowFromNext,
            MergeWithNext,
            MergeWithPrev,
        };

        template <typename Ptr>
        struct Position {
            Ptr m_node {};
            Size m_slot {};
        };

        using PositionPtr = Position<NodePtr>;
        using PositionPtrMut = Position<NodePtrMut>;

        static constexpr auto min_degree(Size order) -> Size {
            return order;
        }

        static constexpr auto max_values(Size order) -> Size {
            return (order * 2) - 1;
        }

        static constexpr auto max_children(Size order) -> Size {
            return order * 2;
        }

        template <typename Ptr>
        static constexpr auto parent(Ptr node) -> Ptr {
            return NodeTraits::get_parent(node);
        }

        template <typename Ptr>
        static constexpr auto child(Ptr node, Size slot) -> Ptr {
            return NodeTraits::get_child(node, slot);
        }

        template <typename Ptr>
        static constexpr auto count(Ptr node) -> Size {
            return NodeTraits::get_count(node);
        }

        template <typename Ptr>
        static constexpr auto is_leaf(Ptr node) -> bool {
            return NodeTraits::is_leaf(node);
        }

        template <typename Ptr>
        static constexpr auto is_full(Ptr node, Size order) -> bool {
            return count(node) == max_values(order);
        }

        template <typename Ptr>
        static constexpr auto has_min_degree(Ptr node, Size order) -> bool {
            return count(node) >= min_degree(order);
        }

        template <typename Ptr>
        static constexpr auto needs_fill(Ptr node, Size order) -> bool {
            return count(node) < min_degree(order);
        }

        template <typename Ptr>
        static constexpr auto child_index(Ptr parent, Ptr child) -> Size {
            for (Size i = 0; i <= count(parent); ++i) {
                if (BTreeNodeAlgorithmsImpl::child(parent, i) == child) {
                    return i;
                }
            }
            return count(parent) + 1;
        }

        template <typename Ptr>
        static constexpr auto leftmost(Ptr node) -> Ptr {
            while (node != nullptr && !is_leaf(node)) {
                node = child(node, 0);
            }
            return node;
        }

        template <typename Ptr>
        static constexpr auto rightmost(Ptr node) -> Ptr {
            while (node != nullptr && !is_leaf(node)) {
                node = child(node, count(node));
            }
            return node;
        }

        template <typename Ptr>
        static constexpr auto first_child(Ptr node) -> Ptr {
            if (is_leaf(node)) {
                return nullptr;
            }

            for (Size i = 0; i <= count(node); ++i) {
                if (auto child = BTreeNodeAlgorithmsImpl::child(node, i); child != nullptr) {
                    return child;
                }
            }
            return nullptr;
        }

        template <typename Ptr>
        static constexpr auto next_position(Ptr node, Size slot) -> Position<Ptr> {
            if (node == nullptr) {
                return {};
            }

            if (!is_leaf(node)) {
                return {
                    .m_node = leftmost(child(node, slot + 1)),
                    .m_slot = 0,
                };
            }

            if (slot + 1 < count(node)) {
                return {
                    .m_node = node,
                    .m_slot = slot + 1,
                };
            }

            auto child = node;
            auto parent = BTreeNodeAlgorithmsImpl::parent(node);
            while (parent != nullptr) {
                auto const child_slot = child_index(parent, child);
                if (child_slot < count(parent)) {
                    return {
                        .m_node = parent,
                        .m_slot = child_slot,
                    };
                }
                child = parent;
                parent = BTreeNodeAlgorithmsImpl::parent(parent);
            }

            return {};
        }

        template <typename Ptr>
        static constexpr auto prev_position(Ptr root, Ptr node, Size slot) -> Position<Ptr> {
            if (node == nullptr) {
                auto right = rightmost(root);
                return {
                    .m_node = right,
                    .m_slot = right == nullptr ? 0 : count(right) - 1,
                };
            }

            if (!is_leaf(node)) {
                auto pred = rightmost(child(node, slot));
                return {
                    .m_node = pred,
                    .m_slot = count(pred) - 1,
                };
            }

            if (slot > 0) {
                return {
                    .m_node = node,
                    .m_slot = slot - 1,
                };
            }

            auto child = node;
            auto parent = BTreeNodeAlgorithmsImpl::parent(node);
            while (parent != nullptr) {
                auto const child_slot = child_index(parent, child);
                if (child_slot > 0) {
                    return {
                        .m_node = parent,
                        .m_slot = child_slot - 1,
                    };
                }
                child = parent;
                parent = BTreeNodeAlgorithmsImpl::parent(parent);
            }

            return {
                .m_node = node,
                .m_slot = slot,
            };
        }

        static constexpr auto move_upper_children_after_split(NodePtrMut left, NodePtrMut right, Size order) -> void {
            if (is_leaf(left)) {
                return;
            }

            auto const degree = min_degree(order);
            for (Size i = 0; i < degree; ++i) {
                auto child = BTreeNodeAlgorithmsImpl::child(left, i + degree);
                NodeTraits::set_child(right, i, child);
                if (child != nullptr) {
                    NodeTraits::set_parent(child, right);
                }
                NodeTraits::set_child(left, i + degree, nullptr);
            }
        }

        static constexpr auto insert_child_after_split(NodePtrMut parent, Size slot, NodePtrMut right) -> void {
            for (Size i = count(parent) + 1; i > slot + 1; --i) {
                NodeTraits::set_child(parent, i, child(parent, i - 1));
            }
            NodeTraits::set_child(parent, slot + 1, right);
            NodeTraits::set_parent(right, parent);
        }

        static constexpr auto set_root_child(NodePtrMut root, NodePtrMut child) -> void {
            NodeTraits::set_child(root, 0, child);
            if (child != nullptr) {
                NodeTraits::set_parent(child, root);
            }
        }

        template <typename Ptr>
        static constexpr auto search_child(Ptr node, Size slot) -> Ptr {
            return is_leaf(node) ? nullptr : child(node, slot);
        }

        static constexpr auto select_rebalance_plan(NodePtrMut node, Size slot, Size order) -> RebalancePlan {
            if (slot > 0 && has_min_degree(child(node, slot - 1), order)) {
                return RebalancePlan::BorrowFromPrev;
            }
            if (slot < count(node) && has_min_degree(child(node, slot + 1), order)) {
                return RebalancePlan::BorrowFromNext;
            }
            return slot < count(node) ? RebalancePlan::MergeWithNext : RebalancePlan::MergeWithPrev;
        }

        static constexpr auto move_children_for_merge(NodePtrMut left, NodePtrMut right, Size order) -> void {
            if (is_leaf(left)) {
                return;
            }

            auto const degree = min_degree(order);
            for (Size i = 0; i <= count(right); ++i) {
                auto child = BTreeNodeAlgorithmsImpl::child(right, i);
                NodeTraits::set_child(left, i + degree, child);
                if (child != nullptr) {
                    NodeTraits::set_parent(child, left);
                }
                NodeTraits::set_child(right, i, nullptr);
            }
        }

        static constexpr auto erase_child_after_merge(NodePtrMut parent, Size slot) -> void {
            for (Size i = slot; i + 1 < count(parent); ++i) {
                NodeTraits::set_child(parent, i + 1, child(parent, i + 2));
            }
            NodeTraits::set_child(parent, count(parent), nullptr);
        }

        static constexpr auto shift_children_right(NodePtrMut node, Size first, Size last) -> void {
            for (Size i = last; i > first; --i) {
                NodeTraits::set_child(node, i, child(node, i - 1));
            }
        }

        static constexpr auto shift_children_left(NodePtrMut node, Size first, Size last) -> void {
            for (Size i = first; i < last; ++i) {
                NodeTraits::set_child(node, i, child(node, i + 1));
            }
            NodeTraits::set_child(node, last, nullptr);
        }

        static constexpr auto move_last_child_to_front(NodePtrMut dst, NodePtrMut src) -> void {
            if (is_leaf(dst)) {
                return;
            }

            shift_children_right(dst, 0, count(dst) + 1);

            auto child = BTreeNodeAlgorithmsImpl::child(src, count(src));
            NodeTraits::set_child(dst, 0, child);
            if (child != nullptr) {
                NodeTraits::set_parent(child, dst);
            }
            NodeTraits::set_child(src, count(src), nullptr);
        }

        static constexpr auto move_first_child_to_back(NodePtrMut dst, NodePtrMut src) -> void {
            if (is_leaf(dst)) {
                return;
            }

            auto child = BTreeNodeAlgorithmsImpl::child(src, 0);
            auto const dst_slot = count(dst) + 1;
            NodeTraits::set_child(dst, dst_slot, child);
            if (child != nullptr) {
                NodeTraits::set_parent(child, dst);
            }
            shift_children_left(src, 0, count(src));
        }

        static constexpr auto take_first_child(NodePtrMut node) -> NodePtrMut {
            if (is_leaf(node)) {
                return nullptr;
            }

            auto child = BTreeNodeAlgorithmsImpl::child(node, 0);
            if (child != nullptr) {
                NodeTraits::set_parent(child, nullptr);
            }
            NodeTraits::set_child(node, 0, nullptr);
            return child;
        }

        static constexpr auto detach_from_parent(NodePtrMut node, Size order) -> NodePtrMut {
            auto parent = BTreeNodeAlgorithmsImpl::parent(node);
            if (parent != nullptr) {
                auto const slot = child_index(parent, node);
                if (slot < max_children(order)) {
                    NodeTraits::set_child(parent, slot, nullptr);
                }
            }
            return parent;
        }
    };

    /// BTree node topology algorithms in the Boost.Intrusive style.
    ///
    /// This class is a compile-time-order facade. `Impl` owns the algorithms;
    /// this wrapper only forwards `ORDER` for call sites that know it statically.
    template <typename NodeTraitsType, Size OrderValue>
    struct BTreeNodeAlgorithms {
        static_assert(OrderValue >= 3, "BTree order must be at least 3");

        using Impl = BTreeNodeAlgorithmsImpl<NodeTraitsType>;

        using NodeTraits = typename Impl::NodeTraits;
        using Node = typename Impl::Node;
        using NodePtr = typename Impl::NodePtr;
        using NodePtrMut = typename Impl::NodePtrMut;
        using RebalancePlan = typename Impl::RebalancePlan;
        using Position = typename Impl::PositionPtr;
        using PositionMut = typename Impl::PositionPtrMut;

        static constexpr auto ORDER = OrderValue;
        static constexpr auto MIN_DEGREE = Impl::min_degree(ORDER);
        static constexpr auto MAX_VALUES = Impl::max_values(ORDER);
        static constexpr auto MAX_CHILDREN = Impl::max_children(ORDER);

        template <typename Ptr>
        static constexpr auto parent(Ptr node) -> Ptr {
            return Impl::parent(node);
        }

        template <typename Ptr>
        static constexpr auto child(Ptr node, Size slot) -> Ptr {
            return Impl::child(node, slot);
        }

        template <typename Ptr>
        static constexpr auto count(Ptr node) -> Size {
            return Impl::count(node);
        }

        template <typename Ptr>
        static constexpr auto is_leaf(Ptr node) -> bool {
            return Impl::is_leaf(node);
        }

        template <typename Ptr>
        static constexpr auto is_full(Ptr node) -> bool {
            return Impl::is_full(node, ORDER);
        }

        template <typename Ptr>
        static constexpr auto has_min_degree(Ptr node) -> bool {
            return Impl::has_min_degree(node, ORDER);
        }

        template <typename Ptr>
        static constexpr auto needs_fill(Ptr node) -> bool {
            return Impl::needs_fill(node, ORDER);
        }

        template <typename Ptr>
        static constexpr auto child_index(Ptr parent, Ptr child) -> Size {
            return Impl::child_index(parent, child);
        }

        template <typename Ptr>
        static constexpr auto leftmost(Ptr node) -> Ptr {
            return Impl::leftmost(node);
        }

        template <typename Ptr>
        static constexpr auto rightmost(Ptr node) -> Ptr {
            return Impl::rightmost(node);
        }

        template <typename Ptr>
        static constexpr auto first_child(Ptr node) -> Ptr {
            return Impl::first_child(node);
        }

        static constexpr auto next_position(NodePtr node, Size slot) -> Position {
            return Impl::next_position(node, slot);
        }

        static constexpr auto next_position(NodePtrMut node, Size slot) -> PositionMut {
            return Impl::next_position(node, slot);
        }

        static constexpr auto prev_position(NodePtr root, NodePtr node, Size slot) -> Position {
            return Impl::prev_position(root, node, slot);
        }

        static constexpr auto prev_position(NodePtrMut root, NodePtrMut node, Size slot) -> PositionMut {
            return Impl::prev_position(root, node, slot);
        }

        static constexpr auto move_upper_children_after_split(NodePtrMut left, NodePtrMut right) -> void {
            Impl::move_upper_children_after_split(left, right, ORDER);
        }

        static constexpr auto insert_child_after_split(NodePtrMut parent, Size slot, NodePtrMut right) -> void {
            Impl::insert_child_after_split(parent, slot, right);
        }

        static constexpr auto set_root_child(NodePtrMut root, NodePtrMut child) -> void {
            Impl::set_root_child(root, child);
        }

        template <typename Ptr>
        static constexpr auto search_child(Ptr node, Size slot) -> Ptr {
            return Impl::search_child(node, slot);
        }

        static constexpr auto select_rebalance_plan(NodePtrMut node, Size slot) -> RebalancePlan {
            return Impl::select_rebalance_plan(node, slot, ORDER);
        }

        static constexpr auto move_children_for_merge(NodePtrMut left, NodePtrMut right) -> void {
            Impl::move_children_for_merge(left, right, ORDER);
        }

        static constexpr auto erase_child_after_merge(NodePtrMut parent, Size slot) -> void {
            Impl::erase_child_after_merge(parent, slot);
        }

        static constexpr auto shift_children_right(NodePtrMut node, Size first, Size last) -> void {
            Impl::shift_children_right(node, first, last);
        }

        static constexpr auto shift_children_left(NodePtrMut node, Size first, Size last) -> void {
            Impl::shift_children_left(node, first, last);
        }

        static constexpr auto move_last_child_to_front(NodePtrMut dst, NodePtrMut src) -> void {
            Impl::move_last_child_to_front(dst, src);
        }

        static constexpr auto move_first_child_to_back(NodePtrMut dst, NodePtrMut src) -> void {
            Impl::move_first_child_to_back(dst, src);
        }

        static constexpr auto take_first_child(NodePtrMut node) -> NodePtrMut {
            return Impl::take_first_child(node);
        }

        static constexpr auto detach_from_parent(NodePtrMut node) -> NodePtrMut {
            return Impl::detach_from_parent(node, ORDER);
        }
    };
} // namespace ustl::collections::priv

#endif // #ifndef USTL_COLLECTIONS_DETAIL_BTREE_NODE_ALGORITHMS_HPP
