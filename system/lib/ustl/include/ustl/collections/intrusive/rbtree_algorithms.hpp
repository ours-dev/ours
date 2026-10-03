/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_RBTREE_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_RBTREE_ALGORITHMS_HPP 1

#include <ustl/collections/intrusive/bstree_algorithms.hpp>
#include <ustl/collections/intrusive/detail/extension.hpp>

#include <cstddef>

namespace ustl::collections::intrusive {
/// Red-black tree algorithms.
///
/// Every operation that reshapes the tree comes in exactly two variants: the
/// plain one, and one taking a extension set plus the policy's callback
/// table. That pair does not grow when a extension is added -- a new
/// extension is a bit in `Extension`, a slot in the table and an
/// `if constexpr` block, never another overload.
///
/// The split between the two is deliberate. `Caps` is a template parameter,
/// so a extension nobody asked for compiles away completely rather than
/// costing a null check in the rebalancing inner loops; the callback table
/// stays a runtime pointer, so any number of policies sharing a extension
/// set share one set of algorithm bodies. Bodies are therefore bounded by
/// the number of distinct extension *sets* in a program, not by the number
/// of policies.
template <typename TNodeTraits>
class RBTreeAlgorithms final: public BstreeAlgorithms<TNodeTraits> {
    using BstreeAlgo = BstreeAlgorithms<TNodeTraits>;

  public:
    using Node = typename BstreeAlgo::Node;
    using NodeTraits = TNodeTraits;
    using NodePtr = typename BstreeAlgo::NodePtr;
    using ConstNodePtr = typename BstreeAlgo::ConstNodePtr;
    using InsertCommitData = typename BstreeAlgo::InsertCommitData;
    using DataForRebalance = typename BstreeAlgo::DataForRebalance;
    using Color = typename NodeTraits::Color;
    using Extension = detail::Extension;
    using Callbacks = detail::ExtensionCallbacks<NodePtr>;

    /// What a mutation carries besides the nodes: the policy's callback
    /// table and whatever per-tree state it wants handed back. Which hooks
    /// actually fire is decided at compile time by the `Caps` template
    /// argument below, so an unused extension costs nothing -- not even a
    /// null check -- while the table itself stays a runtime value so many
    /// policies share one set of algorithm bodies.
    struct Hooks final {
        Callbacks const *callbacks = nullptr;
        void *context = nullptr;
    };

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

    [[nodiscard]]
    static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
        return BstreeAlgo::inited(node);
    }

    [[nodiscard]]
    static constexpr auto is_header(ConstNodePtr const node) noexcept -> bool {
        return NodeTraits::get_color(node) == NodeTraits::red() && BstreeAlgo::is_header(node);
    }

    static constexpr auto swap_nodes(NodePtr const node_1, NodePtr const node_2) noexcept -> void {
        if (node_1 == node_2) {
            return;
        }
        swap_nodes(node_1, BstreeAlgo::get_header(node_1), node_2, BstreeAlgo::get_header(node_2));
    }

    static constexpr auto swap_nodes(NodePtr const node_1, NodePtr const header_1,
                                     NodePtr const node_2, NodePtr const header_2) noexcept
        -> void {
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
                                                   std::size_t *const depth = nullptr) noexcept
        -> NodePtr {
        BstreeAlgo::insert_equal_upper_bound(header, new_node, compare, depth);
        after_link<Extension::None>(header, new_node, Hooks{});
        return new_node;
    }

    template <Extension Caps, typename NodePointerCompare>
    static constexpr auto insert_equal_upper_bound(NodePtr const header, NodePtr const new_node,
                                                   NodePointerCompare compare,
                                                   std::size_t *const depth,
                                                   Hooks const &hooks) noexcept -> NodePtr {
        std::size_t measured = 0;
        BstreeAlgo::insert_equal_upper_bound(header, new_node, compare,
                                             depth != nullptr ? depth : &measured);
        after_link<Caps>(header, new_node, hooks, depth != nullptr ? *depth : measured);
        return new_node;
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal_lower_bound(NodePtr const header, NodePtr const new_node,
                                                   NodePointerCompare compare,
                                                   std::size_t *const depth = nullptr) noexcept
        -> NodePtr {
        BstreeAlgo::insert_equal_lower_bound(header, new_node, compare, depth);
        after_link<Extension::None>(header, new_node, Hooks{});
        return new_node;
    }

    template <typename NodePointerCompare>
    static constexpr auto insert_equal(NodePtr const header, NodePtr const hint,
                                       NodePtr const new_node, NodePointerCompare compare,
                                       std::size_t *const depth = nullptr) noexcept -> NodePtr {
        BstreeAlgo::insert_equal(header, hint, new_node, compare, depth);
        after_link<Extension::None>(header, new_node, Hooks{});
        return new_node;
    }

    static constexpr auto insert_before(NodePtr const header, NodePtr const position,
                                        NodePtr const new_node,
                                        std::size_t *const depth = nullptr) noexcept -> NodePtr {
        BstreeAlgo::insert_before(header, position, new_node, depth);
        after_link<Extension::None>(header, new_node, Hooks{});
        return new_node;
    }

    template <Extension Caps>
    static constexpr auto insert_before(NodePtr const header, NodePtr const position,
                                        NodePtr const new_node, std::size_t *const depth,
                                        Hooks const &hooks) noexcept -> NodePtr {
        std::size_t measured = 0;
        BstreeAlgo::insert_before(header, position, new_node, depth != nullptr ? depth : &measured);
        after_link<Caps>(header, new_node, hooks, depth != nullptr ? *depth : measured);
        return new_node;
    }

    static constexpr auto push_back(NodePtr const header, NodePtr const new_node,
                                    std::size_t *const depth = nullptr) noexcept -> void {
        BstreeAlgo::push_back(header, new_node, depth);
        after_link<Extension::None>(header, new_node, Hooks{});
    }

    static constexpr auto push_front(NodePtr const header, NodePtr const new_node,
                                     std::size_t *const depth = nullptr) noexcept -> void {
        BstreeAlgo::push_front(header, new_node, depth);
        after_link<Extension::None>(header, new_node, Hooks{});
    }

    static constexpr auto insert_unique_commit(NodePtr const header, NodePtr const new_node,
                                               InsertCommitData const &commit_data) noexcept
        -> void {
        BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
        after_link<Extension::None>(header, new_node, Hooks{});
    }

    template <Extension Caps>
    static constexpr auto insert_unique_commit(NodePtr const header, NodePtr const new_node,
                                               InsertCommitData const &commit_data,
                                               Hooks const &hooks) noexcept -> void {
        BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
        after_link<Caps>(header, new_node, hooks);
    }

    [[nodiscard]]
    static constexpr auto erase(NodePtr const header, NodePtr const node) noexcept -> NodePtr {
        return erase_impl<Extension::None>(header, node, Hooks{});
    }

    template <Extension Caps>
    [[nodiscard]]
    static constexpr auto erase(NodePtr const header, NodePtr const node,
                                Hooks const &hooks) noexcept -> NodePtr {
        return erase_impl<Caps>(header, node, hooks);
    }

    static constexpr auto unlink(NodePtr const node) noexcept -> void {
        if (!unique(node)) {
            auto _ = erase_impl<Extension::None>(BstreeAlgo::get_header(node), node, Hooks{});
        }
    }

    template <typename Disposer>
    static constexpr auto clear_and_dispose(NodePtr const header, Disposer disposer) noexcept
        -> void {
        while (!unique(header)) {
            const auto node = begin_node(header);
            auto _ = erase_impl<Extension::None>(header, node, Hooks{});
            disposer(node);
        }
    }

  private:
    /// A rotation as the augmented tree needs it: the structural rotation
    /// first, then the aggregate repair for the two nodes it moved.
    template <Extension Caps>
    static constexpr auto rotate_left_aug(NodePtr const node, NodePtr const right,
                                          NodePtr const parent, NodePtr const header,
                                          Hooks const &hooks) noexcept -> void {
        BstreeAlgo::rotate_left(node, right, parent, header);
        if constexpr (detail::has_extension(Caps, Extension::Augment)) {
            hooks.callbacks->rotate(node, right);
        }
        if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
            hooks.callbacks->on_rotate(hooks.context, node, right);
        }
    }

    template <Extension Caps>
    static constexpr auto rotate_right_aug(NodePtr const node, NodePtr const left,
                                           NodePtr const parent, NodePtr const header,
                                           Hooks const &hooks) noexcept -> void {
        BstreeAlgo::rotate_right(node, left, parent, header);
        if constexpr (detail::has_extension(Caps, Extension::Augment)) {
            hooks.callbacks->rotate(node, left);
        }
        if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
            hooks.callbacks->on_rotate(hooks.context, node, left);
        }
    }

    template <Extension Caps>
    static constexpr auto rotate_left_no_parent_fix_aug(NodePtr const node, NodePtr const right,
                                                        Hooks const &hooks) noexcept -> void {
        BstreeAlgo::rotate_left_no_parent_fix(node, right);
        if constexpr (detail::has_extension(Caps, Extension::Augment)) {
            hooks.callbacks->rotate(node, right);
        }
        if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
            hooks.callbacks->on_rotate(hooks.context, node, right);
        }
    }

    template <Extension Caps>
    static constexpr auto rotate_right_no_parent_fix_aug(NodePtr const node, NodePtr const left,
                                                         Hooks const &hooks) noexcept -> void {
        BstreeAlgo::rotate_right_no_parent_fix(node, left);
        if constexpr (detail::has_extension(Caps, Extension::Augment)) {
            hooks.callbacks->rotate(node, left);
        }
        if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
            hooks.callbacks->on_rotate(hooks.context, node, left);
        }
    }

    /// Shared tail of every insertion: seed the freshly linked leaf's own
    /// aggregate and refresh its ancestors, then restore the colour
    /// invariants (whose rotations carry the aggregates along themselves).
    template <Extension Caps>
    static constexpr auto after_link(NodePtr const header, NodePtr const new_node,
                                     Hooks const &hooks, std::size_t const depth = 0) noexcept
        -> void {
        if constexpr (detail::has_extension(Caps, Extension::Augment)) {
            hooks.callbacks->propagate(new_node, header);
        }
        if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
            hooks.callbacks->on_insert(hooks.context, new_node, depth);
        }
        rebalance_after_insertion<Caps>(header, new_node, hooks);
    }

    template <Extension Caps>
    [[nodiscard]]
    static constexpr auto erase_impl(NodePtr const header, NodePtr const node,
                                     Hooks const &hooks) noexcept -> NodePtr {
        DataForRebalance info{};
        BstreeAlgo::erase(header, node, info);
        if constexpr (detail::has_extension(Caps, Extension::Augment)) {
            // Refresh upwards from the deepest slot whose contents changed.
            hooks.callbacks->propagate(info.m_x_parent, header);
            if (info.m_y != node) {
                // The successor was transplanted into the erased node's
                // slot, so it now roots a different subtree than it did.
                // Seeding it from the erased node is not enough: the walk
                // above starts below the successor and is allowed to stop
                // early, so it can return before ever reaching it. Recompute
                // the successor itself, then carry that upwards.
                hooks.callbacks->copy(node, info.m_y);
                hooks.callbacks->propagate(info.m_y, header);
            }
        }
        if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
            hooks.callbacks->on_erase(hooks.context, node);
        }
        rebalance_after_erasure<Caps>(header, node, info, hooks);
        return node;
    }

    template <Extension Caps>
    static constexpr auto rebalance_after_erasure(NodePtr const header, NodePtr const erased,
                                                  DataForRebalance const &info,
                                                  Hooks const &hooks) noexcept -> void {
        auto removed_color = NodeTraits::get_color(erased);
        if (info.m_y != erased) {
            removed_color = NodeTraits::get_color(info.m_y);
            NodeTraits::set_color(info.m_y, NodeTraits::get_color(erased));
        }
        if (removed_color != NodeTraits::red()) {
            rebalance_after_erasure_restore_invariants<Caps>(header, info.m_x, info.m_x_parent,
                                                             hooks);
        }
    }

    template <Extension Caps>
    static constexpr auto rebalance_after_erasure_restore_invariants(NodePtr const header,
                                                                     NodePtr child, NodePtr parent,
                                                                     Hooks const &hooks) noexcept
        -> void {
        while (parent != header &&
               (child == NodePtr{} || NodeTraits::get_color(child) == NodeTraits::black())) {
            if (child == NodeTraits::get_left(parent)) {
                auto sibling = NodeTraits::get_right(parent);
                if (NodeTraits::get_color(sibling) == NodeTraits::red()) {
                    NodeTraits::set_color(sibling, NodeTraits::black());
                    NodeTraits::set_color(parent, NodeTraits::red());
                    rotate_left_aug<Caps>(parent, sibling, NodeTraits::get_parent(parent), header,
                                          hooks);
                    sibling = NodeTraits::get_right(parent);
                }
                const auto sibling_left = NodeTraits::get_left(sibling);
                const auto sibling_right = NodeTraits::get_right(sibling);
                if ((sibling_left == NodePtr{} ||
                     NodeTraits::get_color(sibling_left) == NodeTraits::black()) &&
                    (sibling_right == NodePtr{} ||
                     NodeTraits::get_color(sibling_right) == NodeTraits::black())) {
                    NodeTraits::set_color(sibling, NodeTraits::red());
                    if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
                        hooks.callbacks->on_rebalance_step(hooks.context, false);
                    }
                    child = parent;
                    parent = NodeTraits::get_parent(parent);
                } else {
                    if (sibling_right == NodePtr{} ||
                        NodeTraits::get_color(sibling_right) == NodeTraits::black()) {
                        NodeTraits::set_color(sibling_left, NodeTraits::black());
                        NodeTraits::set_color(sibling, NodeTraits::red());
                        rotate_right_aug<Caps>(sibling, sibling_left,
                                               NodeTraits::get_parent(sibling), header, hooks);
                        sibling = NodeTraits::get_right(parent);
                    }
                    NodeTraits::set_color(sibling, NodeTraits::get_color(parent));
                    NodeTraits::set_color(parent, NodeTraits::black());
                    const auto new_sibling_right = NodeTraits::get_right(sibling);
                    if (new_sibling_right != NodePtr{}) {
                        NodeTraits::set_color(new_sibling_right, NodeTraits::black());
                    }
                    rotate_left_aug<Caps>(parent, sibling, NodeTraits::get_parent(parent), header,
                                          hooks);
                    child = NodeTraits::get_parent(header);
                    parent = header;
                }
            } else {
                auto sibling = NodeTraits::get_left(parent);
                if (NodeTraits::get_color(sibling) == NodeTraits::red()) {
                    NodeTraits::set_color(sibling, NodeTraits::black());
                    NodeTraits::set_color(parent, NodeTraits::red());
                    rotate_right_aug<Caps>(parent, sibling, NodeTraits::get_parent(parent), header,
                                           hooks);
                    sibling = NodeTraits::get_left(parent);
                }
                const auto sibling_left = NodeTraits::get_left(sibling);
                const auto sibling_right = NodeTraits::get_right(sibling);
                if ((sibling_left == NodePtr{} ||
                     NodeTraits::get_color(sibling_left) == NodeTraits::black()) &&
                    (sibling_right == NodePtr{} ||
                     NodeTraits::get_color(sibling_right) == NodeTraits::black())) {
                    NodeTraits::set_color(sibling, NodeTraits::red());
                    if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
                        hooks.callbacks->on_rebalance_step(hooks.context, false);
                    }
                    child = parent;
                    parent = NodeTraits::get_parent(parent);
                } else {
                    if (sibling_left == NodePtr{} ||
                        NodeTraits::get_color(sibling_left) == NodeTraits::black()) {
                        NodeTraits::set_color(sibling_right, NodeTraits::black());
                        NodeTraits::set_color(sibling, NodeTraits::red());
                        rotate_left_aug<Caps>(sibling, sibling_right,
                                              NodeTraits::get_parent(sibling), header, hooks);
                        sibling = NodeTraits::get_left(parent);
                    }
                    NodeTraits::set_color(sibling, NodeTraits::get_color(parent));
                    NodeTraits::set_color(parent, NodeTraits::black());
                    const auto new_sibling_left = NodeTraits::get_left(sibling);
                    if (new_sibling_left != NodePtr{}) {
                        NodeTraits::set_color(new_sibling_left, NodeTraits::black());
                    }
                    rotate_right_aug<Caps>(parent, sibling, NodeTraits::get_parent(parent), header,
                                           hooks);
                    child = NodeTraits::get_parent(header);
                    parent = header;
                }
            }
        }
        if (child != NodePtr{}) {
            NodeTraits::set_color(child, NodeTraits::black());
        }
    }

    template <Extension Caps>
    static constexpr auto rebalance_after_insertion(NodePtr const header, NodePtr node,
                                                    Hooks const &hooks) noexcept -> void {
        NodeTraits::set_color(node, NodeTraits::red());
        while (true) {
            auto parent = NodeTraits::get_parent(node);
            const auto grandparent = NodeTraits::get_parent(parent);
            if (parent == header || NodeTraits::get_color(parent) == NodeTraits::black() ||
                grandparent == header) {
                break;
            }
            NodeTraits::set_color(grandparent, NodeTraits::red());
            const auto grandparent_left = NodeTraits::get_left(grandparent);
            const auto parent_is_left = parent == grandparent_left;
            const auto uncle =
                parent_is_left ? NodeTraits::get_right(grandparent) : grandparent_left;
            if (uncle != NodePtr{} && NodeTraits::get_color(uncle) == NodeTraits::red()) {
                NodeTraits::set_color(uncle, NodeTraits::black());
                NodeTraits::set_color(parent, NodeTraits::black());
                if constexpr (detail::has_extension(Caps, Extension::Instrument)) {
                    hooks.callbacks->on_rebalance_step(hooks.context, true);
                }
                node = grandparent;
                continue;
            }
            const auto node_is_left = NodeTraits::get_left(parent) == node;
            if (parent_is_left) {
                if (!node_is_left) {
                    rotate_left_no_parent_fix_aug<Caps>(parent, node, hooks);
                    parent = node;
                }
                rotate_right_aug<Caps>(grandparent, parent, NodeTraits::get_parent(grandparent),
                                       header, hooks);
            } else {
                if (node_is_left) {
                    rotate_right_no_parent_fix_aug<Caps>(parent, node, hooks);
                    parent = node;
                }
                rotate_left_aug<Caps>(grandparent, parent, NodeTraits::get_parent(grandparent),
                                      header, hooks);
            }
            NodeTraits::set_color(parent, NodeTraits::black());
            break;
        }
        NodeTraits::set_color(NodeTraits::get_parent(header), NodeTraits::black());
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_RBTREE_ALGORITHMS_HPP
