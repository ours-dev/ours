/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_AUGMENT_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_AUGMENT_HPP 1

namespace ustl::collections::intrusive::detail {

/// Callbacks a tree invokes at every point where a subtree's shape changes,
/// so an augmented tree can keep a per-node aggregate of its subtree up to
/// date. Modelled on Linux's `struct rb_augment_callbacks`.
///
/// This is deliberately a plain struct of function pointers rather than a
/// template parameter on the algorithms: the rebalancing machinery is by far
/// the bulkiest code in the tree, and making it generic over the policy would
/// re-emit all of it for every distinct augmentation. `AugmentCallbacks` is
/// parameterised only on `NodePtr` — which each tree already fixes through
/// its own `NodeTraits` — so the algorithms stay a single body and each
/// policy costs one static table plus the small thunks in `AugmentAdapter`.
///
/// A null `AugmentCallbacks const *` means "no augmentation"; the algorithms
/// branch on it rather than calling through no-op pointers, so a plain tree
/// pays a predictable, well-predicted test and no indirect call.
template <typename NodePtr>
struct AugmentCallbacks final {
    /// Recomputes aggregates from `node` upwards, stopping before `stop`
    /// (the header) or as soon as a node's value is unchanged — if a node's
    /// aggregate did not move, no ancestor's can either.
    auto (*propagate)(NodePtr node, NodePtr stop) noexcept -> void;

    /// Hands `src`'s aggregate to `dst`, which is taking over its position
    /// in the tree (the successor transplant inside `erase`).
    auto (*copy)(NodePtr src, NodePtr dst) noexcept -> void;

    /// Repairs the two nodes a single rotation moved: `new_root` took over
    /// `old_root`'s subtree, so it inherits its aggregate, and `old_root` is
    /// recomputed from the children it has now.
    auto (*rotate)(NodePtr old_root, NodePtr new_root) noexcept -> void;
};

/// Lifts a user-written augmentation policy into `AugmentCallbacks`.
///
/// The policy is written purely in terms of the container's element type and
/// never sees a node: walking parent pointers, telling a missing child from a
/// present one, and converting nodes back to elements all happen here. What
/// is left for the policy is the one genuinely local question — "given my two
/// children, what is my aggregate?" — expressed as:
///
///     struct Policy {
///         using Element = T;
///         /// Recomputes `self` from its children (null when absent).
///         /// Returns true when the value did not change, which lets
///         /// propagation stop early.
///         static auto recompute(Element &self, Element const *left,
///                               Element const *right) noexcept -> bool;
///         /// Gives `dst` the aggregate `src` currently holds.
///         static auto copy(Element const &src, Element &dst) noexcept -> void;
///     };
///
/// `rotate` is derived rather than asked for: it is exactly `copy` followed
/// by a `recompute` of the demoted node.
template <typename Policy, typename ValueTraits>
struct AugmentAdapter final {
    using NodeTraits = typename ValueTraits::NodeTraits;
    using NodePtr = typename ValueTraits::NodePtr;
    using Element = typename ValueTraits::Element;

    [[nodiscard]]
    static constexpr auto element_of(NodePtr const node) noexcept -> Element * {
        return &*ValueTraits::to_value_ptr(node);
    }

    /// A child slot as the policy sees it: nullptr when the slot is empty.
    [[nodiscard]]
    static constexpr auto child_of(NodePtr const node) noexcept -> Element * {
        if (node == NodePtr{}) {
            return nullptr;
        }
        return element_of(node);
    }

    static constexpr auto recompute(NodePtr const node) noexcept -> bool {
        return Policy::recompute(*element_of(node), child_of(NodeTraits::get_left(node)),
                                 child_of(NodeTraits::get_right(node)));
    }

    static constexpr auto propagate(NodePtr node, NodePtr const stop) noexcept -> void {
        if (node == stop || node == NodePtr{}) {
            return;
        }
        // The caller only ever names a node whose subtree just changed --
        // a freshly linked leaf, or the parent of a slot erase emptied -- so
        // this first value has to be recomputed even when that turns out to
        // be a no-op. Early-stopping here instead would be wrong exactly
        // when it matters: a new leaf whose own aggregate happens to equal
        // what it was seeded with still has to be accounted for by its
        // ancestors, whose child set just grew.
        auto _ = recompute(node);
        node = NodeTraits::get_parent(node);

        // From here up the premise holds -- each node's stored aggregate was
        // correct for its old subtree -- so an unchanged value really does
        // mean nothing above can change either.
        while (node != stop && node != NodePtr{}) {
            if (recompute(node)) {
                break;
            }
            node = NodeTraits::get_parent(node);
        }
    }

    static constexpr auto copy(NodePtr const src, NodePtr const dst) noexcept -> void {
        Policy::copy(*element_of(src), *element_of(dst));
    }

    static constexpr auto rotate(NodePtr const old_root, NodePtr const new_root) noexcept -> void {
        Policy::copy(*element_of(old_root), *element_of(new_root));
        auto _ = recompute(old_root);
    }
};

/// The single static callback table for one (policy, container) pairing.
template <typename Policy, typename ValueTraits>
inline constexpr AugmentCallbacks<typename ValueTraits::NodePtr> AUGMENT_CALLBACKS_OF{
    .propagate = &AugmentAdapter<Policy, ValueTraits>::propagate,
    .copy = &AugmentAdapter<Policy, ValueTraits>::copy,
    .rotate = &AugmentAdapter<Policy, ValueTraits>::rotate,
};

} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_AUGMENT_HPP
