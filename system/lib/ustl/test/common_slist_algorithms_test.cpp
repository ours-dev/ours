/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/common_slist_algorithms.hpp>
#include <ustl/collections/intrusive/detail/slist_node.hpp>

namespace {
    using Node = ustl::collections::intrusive::SlistNode<void *>;
    using Traits = ustl::collections::intrusive::SlistNodeTraits<void *>;
    using Algorithms = ustl::collections::intrusive::detail::CommonSlistAlgorithms<Traits>;
} // namespace

auto main() -> int {
    Node before{};
    Node first{};
    Node second{};
    Algorithms::init(&before);
    Algorithms::init(&first);
    Algorithms::init(&second);
    Algorithms::link_after(&before, &first);
    Algorithms::link_after(&first, &second);
    if (Traits::get_next(&before) != &first || Traits::get_next(&first) != &second) {
        return 1;
    }
    Algorithms::unlink_after(&before);
    if (Traits::get_next(&before) != &second) {
        return 2;
    }

    auto disposed = 0;
    Algorithms::link_after(&before, &first);
    Algorithms::unlink_after_and_dispose(&before, [&disposed, &first](Node *const node) noexcept -> void {
        if (node != &first) {
            disposed = -1;
            return;
        }
        ++disposed;
    });
    if (disposed != 1 || Traits::get_next(&before) != &second) {
        return 3;
    }

    Algorithms::init(&before);
    Algorithms::init(&first);
    Algorithms::init(&second);
    Algorithms::link_after(&before, &first);
    Algorithms::link_after(&first, &second);
    auto detached = 0;
    const auto detached_count = Algorithms::detach_and_dispose(&before, [&detached](Node *const) noexcept -> void { ++detached; });
    return detached_count == 2 && detached == 2 && Algorithms::inited(&before) ? 0 : 4;
}
