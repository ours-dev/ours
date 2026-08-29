/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/set_hook.hpp>

namespace {
using SafeHook = ustl::collections::intrusive::SetBaseHook<>;
using AutoHook =
    ustl::collections::intrusive::SetBaseHook<ustl::collections::intrusive::AutoUnlink>;
using MemberHook = ustl::collections::intrusive::SetMemberHook<>;
using Traits = SafeHook::NodeTraits;
using Algorithms = ustl::collections::intrusive::RBTreeAlgorithms<Traits>;
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
    Algorithms::push_back(&header, static_cast<typename Traits::Node *>(&first));
    Algorithms::push_back(&header, static_cast<typename Traits::Node *>(&second));
    if (!first.is_linked() || !second.is_linked()) {
        return 2;
    }

    first.unlink();
    if (first.is_linked() || Algorithms::size(&header) != 1U) {
        return 3;
    }

    Algorithms::push_front(&header, static_cast<typename Traits::Node *>(&first));
    first.swap_nodes(second);
    return Algorithms::size(&header) == 2U && first.is_linked() && second.is_linked() ? 0 : 4;
}
