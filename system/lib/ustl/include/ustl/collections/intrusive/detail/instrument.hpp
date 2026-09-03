/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_INSTRUMENT_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_INSTRUMENT_HPP 1

#include <cstddef>

namespace ustl::collections::intrusive::detail {

/// Diagnostic events a balanced tree can report: how deep an insertion had to
/// search, how much rebalancing that cost, and every rotation performed.
/// Enough to measure depth distribution and to check that rebalancing really
/// is amortized constant.
///
/// Built exactly like `AugmentCallbacks`: a plain table of function pointers
/// parameterised only on `NodePtr`, so the rebalancing code stays a single
/// body no matter how many observers a program defines. A null table means
/// "not instrumented", and since a tree is uninstrumented unless its options
/// say otherwise, the hook sites fold away at compile time in a shipping
/// build rather than costing a branch per rotation.
///
/// `context` is what an observer hangs per-tree state on; it is handed back
/// to every callback, so two trees can be measured independently instead of
/// accumulating into shared globals.
template <typename NodePtr>
struct InstrumentCallbacks final {
    /// A node was linked in, after searching `depth` levels.
    auto (*on_insert)(void *context, NodePtr node, std::size_t depth) noexcept -> void;

    /// A node was unlinked.
    auto (*on_erase)(void *context, NodePtr node) noexcept -> void;

    /// One rotation; `new_root` has taken `old_root`'s place.
    auto (*on_rotate)(void *context, NodePtr old_root, NodePtr new_root) noexcept -> void;

    /// One iteration of a rebalancing loop that recoloured without rotating.
    /// `inserting` separates the insert path from the erase path.
    auto (*on_rebalance_step)(void *context, bool inserting) noexcept -> void;
};

/// Lifts a user-written instrumentation policy into `InstrumentCallbacks`,
/// the same way `AugmentAdapter` does for augmentation: the policy is written
/// against the container's element type and never sees a tree node.
///
///     struct Policy {
///         using Element = T;
///         static auto on_insert(void *ctx, Element &node, std::size_t depth) noexcept -> void;
///         static auto on_erase(void *ctx, Element &node) noexcept -> void;
///         static auto on_rotate(void *ctx, Element &old_root, Element &new_root) noexcept -> void;
///         static auto on_rebalance_step(void *ctx, bool inserting) noexcept -> void;
///     };
template <typename Policy, typename ValueTraits>
struct InstrumentAdapter final {
    using NodePtr = typename ValueTraits::NodePtr;
    using Element = typename ValueTraits::Element;

    [[nodiscard]]
    static constexpr auto element_of(NodePtr const node) noexcept -> Element * {
        return &*ValueTraits::to_value_ptr(node);
    }

    static constexpr auto on_insert(void *context, NodePtr const node,
                                    std::size_t const depth) noexcept -> void {
        Policy::on_insert(context, *element_of(node), depth);
    }

    static constexpr auto on_erase(void *context, NodePtr const node) noexcept -> void {
        Policy::on_erase(context, *element_of(node));
    }

    static constexpr auto on_rotate(void *context, NodePtr const old_root,
                                    NodePtr const new_root) noexcept -> void {
        Policy::on_rotate(context, *element_of(old_root), *element_of(new_root));
    }

    static constexpr auto on_rebalance_step(void *context, bool const inserting) noexcept -> void {
        Policy::on_rebalance_step(context, inserting);
    }
};

/// The single static callback table for one (policy, container) pairing.
template <typename Policy, typename ValueTraits>
inline constexpr InstrumentCallbacks<typename ValueTraits::NodePtr> INSTRUMENT_CALLBACKS_OF{
    .on_insert = &InstrumentAdapter<Policy, ValueTraits>::on_insert,
    .on_erase = &InstrumentAdapter<Policy, ValueTraits>::on_erase,
    .on_rotate = &InstrumentAdapter<Policy, ValueTraits>::on_rotate,
    .on_rebalance_step = &InstrumentAdapter<Policy, ValueTraits>::on_rebalance_step,
};

/// Per-tree observer state. An empty type on an uninstrumented tree, so
/// `[[no_unique_address]]` keeps the container exactly the size it was.
template <bool Instrumented>
struct InstrumentContext;

template <>
struct InstrumentContext<false> final {
    [[nodiscard]]
    static constexpr auto get() noexcept -> void * {
        return nullptr;
    }
};

template <>
struct InstrumentContext<true> final {
    [[nodiscard]]
    constexpr auto get() const noexcept -> void * {
        return m_context;
    }

    constexpr auto set(void *context) noexcept -> void {
        m_context = context;
    }

    void *m_context = nullptr;
};

} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_INSTRUMENT_HPP
