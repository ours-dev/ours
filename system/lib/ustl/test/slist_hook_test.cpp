/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/circular_slist_algorithms.hpp>
#include <ustl/collections/intrusive/slist_hook.hpp>

namespace {
using SafeHook = ustl::collections::intrusive::SlistBaseHook<>;
using AutoHook =
    ustl::collections::intrusive::SlistBaseHook<ustl::collections::intrusive::AutoUnlink>;
using MemberHook = ustl::collections::intrusive::SlistMemberHook<>;
using Traits = SafeHook::NodeTraits;
using Algorithms = ustl::collections::intrusive::CircularSlistAlgorithms<Traits>;
} // namespace

auto main() -> int {
    SafeHook safe{};
    SafeHook copied(safe);
    MemberHook member{};
    if (safe.is_linked() || copied.is_linked() || member.is_linked()) {
        return 1;
    }

    typename Traits::Node header{};
    Algorithms::init_header(&header);
    AutoHook first{};
    AutoHook second{};
    Algorithms::link_after(&header, static_cast<typename Traits::Node *>(&first));
    if (!first.is_linked()) {
        return 2;
    }
    first.unlink();
    if (first.is_linked() || !Algorithms::is_empty(&header)) {
        return 3;
    }

    typename Traits::Node first_header{};
    typename Traits::Node second_header{};
    Algorithms::init_header(&first_header);
    Algorithms::init_header(&second_header);
    Algorithms::link_after(&first_header, static_cast<typename Traits::Node *>(&first));
    Algorithms::link_after(&second_header, static_cast<typename Traits::Node *>(&second));
    first.swap_nodes(second);
    return Traits::get_next(&first_header) == static_cast<typename Traits::Node *>(&second) &&
                   Traits::get_next(&second_header) == static_cast<typename Traits::Node *>(&first)
               ? 0
               : 4;
}
