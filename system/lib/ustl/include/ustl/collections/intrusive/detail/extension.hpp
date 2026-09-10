/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_EXTENSION_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_EXTENSION_HPP 1

#include <ustl/collections/intrusive/detail/augment.hpp>
#include <ustl/collections/intrusive/detail/instrument.hpp>

#include <cstdint>

namespace ustl::collections::intrusive::detail {

/// What a container policy asks the tree to do beyond storing elements.
///
/// One policy declares a set of these, and the algorithms switch on the set
/// with `if constexpr`, so a extension nobody asked for costs nothing at
/// all -- not even a null check. Adding a extension means adding a bit, a
/// slot in the callback table and an `if constexpr` block; it never adds an
/// overload or another branch to a call site, which is what kept happening
/// while augmentation and instrumentation were two separate options.
enum class Extension : std::uint32_t {
    None = 0,
    /// Keep a per-node aggregate over that node's subtree current.
    Augment = 1U << 0,
    /// Report structural events: search depth, rotations, rebalancing steps.
    Instrument = 1U << 1,
};

[[nodiscard]]
constexpr auto operator|(Extension lhs, Extension rhs) noexcept -> Extension {
    return static_cast<Extension>(static_cast<std::uint32_t>(lhs) |
                                  static_cast<std::uint32_t>(rhs));
}

[[nodiscard]]
constexpr auto operator&(Extension lhs, Extension rhs) noexcept -> Extension {
    return static_cast<Extension>(static_cast<std::uint32_t>(lhs) &
                                  static_cast<std::uint32_t>(rhs));
}

[[nodiscard]]
constexpr auto has_extension(Extension set, Extension bit) noexcept -> bool {
    return (set & bit) != Extension::None;
}

/// The extensions a policy declares, or `None` for a policy-free tree.
template <typename Policy>
inline constexpr Extension EXTENSIONS_OF = Policy::EXTENSIONS;

template <>
inline constexpr Extension EXTENSIONS_OF<void> = Extension::None;

/// One table covering every extension. A slot the policy did not ask for
/// stays null and is never reached, because the matching `if constexpr` is
/// false wherever it would have been called.
template <typename NodePtr>
struct ExtensionCallbacks final {
    // Extension::Augment
    auto (*propagate)(NodePtr node, NodePtr stop) noexcept -> void = nullptr;
    auto (*copy)(NodePtr src, NodePtr dst) noexcept -> void = nullptr;
    auto (*rotate)(NodePtr old_root, NodePtr new_root) noexcept -> void = nullptr;

    // Extension::Instrument
    auto (*on_insert)(void *context, NodePtr node, std::size_t depth) noexcept -> void = nullptr;
    auto (*on_erase)(void *context, NodePtr node) noexcept -> void = nullptr;
    auto (*on_rotate)(void *context, NodePtr old_root, NodePtr new_root) noexcept -> void = nullptr;
    auto (*on_rebalance_step)(void *context, bool inserting) noexcept -> void = nullptr;
};

// Each slot is filled only when its extension is declared. The adapter is
// named solely inside the taken branch, so a policy that never asked for a
// extension is never required to provide its members.
template <typename Policy, typename ValueTraits>
constexpr auto pick_augment() noexcept {
    if constexpr (has_extension(EXTENSIONS_OF<Policy>, Extension::Augment)) {
        return &AugmentAdapter<Policy, ValueTraits>::propagate;
    } else {
        return nullptr;
    }
}

template <typename Policy, typename ValueTraits>
constexpr auto pick_copy() noexcept {
    if constexpr (has_extension(EXTENSIONS_OF<Policy>, Extension::Augment)) {
        return &AugmentAdapter<Policy, ValueTraits>::copy;
    } else {
        return nullptr;
    }
}

template <typename Policy, typename ValueTraits>
constexpr auto pick_rotate() noexcept {
    if constexpr (has_extension(EXTENSIONS_OF<Policy>, Extension::Augment)) {
        return &AugmentAdapter<Policy, ValueTraits>::rotate;
    } else {
        return nullptr;
    }
}

template <typename Policy, typename ValueTraits>
constexpr auto pick_on_insert() noexcept {
    if constexpr (has_extension(EXTENSIONS_OF<Policy>, Extension::Instrument)) {
        return &InstrumentAdapter<Policy, ValueTraits>::on_insert;
    } else {
        return nullptr;
    }
}

template <typename Policy, typename ValueTraits>
constexpr auto pick_on_erase() noexcept {
    if constexpr (has_extension(EXTENSIONS_OF<Policy>, Extension::Instrument)) {
        return &InstrumentAdapter<Policy, ValueTraits>::on_erase;
    } else {
        return nullptr;
    }
}

template <typename Policy, typename ValueTraits>
constexpr auto pick_on_rotate() noexcept {
    if constexpr (has_extension(EXTENSIONS_OF<Policy>, Extension::Instrument)) {
        return &InstrumentAdapter<Policy, ValueTraits>::on_rotate;
    } else {
        return nullptr;
    }
}

template <typename Policy, typename ValueTraits>
constexpr auto pick_on_rebalance_step() noexcept {
    if constexpr (has_extension(EXTENSIONS_OF<Policy>, Extension::Instrument)) {
        return &InstrumentAdapter<Policy, ValueTraits>::on_rebalance_step;
    } else {
        return nullptr;
    }
}

/// The single static table for one (policy, container) pairing.
template <typename Policy, typename ValueTraits>
inline constexpr ExtensionCallbacks<typename ValueTraits::NodePtr> CALLBACKS_OF{
    .propagate = pick_augment<Policy, ValueTraits>(),
    .copy = pick_copy<Policy, ValueTraits>(),
    .rotate = pick_rotate<Policy, ValueTraits>(),
    .on_insert = pick_on_insert<Policy, ValueTraits>(),
    .on_erase = pick_on_erase<Policy, ValueTraits>(),
    .on_rotate = pick_on_rotate<Policy, ValueTraits>(),
    .on_rebalance_step = pick_on_rebalance_step<Policy, ValueTraits>(),
};

/// Per-tree policy state, handed back to the instrumentation hooks. Empty
/// (and therefore free) unless the policy asked for `Instrument`.
template <bool Stateful>
struct PolicyContext;

template <>
struct PolicyContext<false> final {
    [[nodiscard]]
    static constexpr auto get() noexcept -> void * {
        return nullptr;
    }
};

template <>
struct PolicyContext<true> final {
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

namespace ustl::collections::intrusive {
/// The extension set is part of the public vocabulary: a policy names it,
/// so it must be reachable without pulling in `detail`.
using detail::Extension;
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_EXTENSION_HPP
