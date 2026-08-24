/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/slist_node.hpp>
#include <ustl/collections/intrusive/linear_slist_algorithms.hpp>

namespace {
    using Node = ustl::collections::intrusive::SlistNode<void *>;
    using Traits = ustl::collections::intrusive::SlistNodeTraits<void *>;
    using Algorithms = ustl::collections::intrusive::LinearSlistAlgorithms<Traits>;
} // namespace

auto main() -> int {
    Node first{};
    Node second{};
    Node third{};
    Node source{};
    Node destination{};
    Node spare{};

    Algorithms::init_header(&source);
    Algorithms::init_header(&destination);
    Algorithms::init(&first);
    Algorithms::init(&second);
    Algorithms::init(&third);
    Algorithms::init(&spare);
    Algorithms::link_after(&source, &first);
    Algorithms::link_after(&first, &second);
    Algorithms::link_after(&second, &third);
    if (Algorithms::is_empty(&source) || Algorithms::count(&source) != 4 ||
        Algorithms::distance(Traits::get_next(&source), nullptr) != 3 ||
        Algorithms::end_node(&source) != nullptr || Algorithms::is_sentinel(&source) ||
        Algorithms::get_previous_node(&source, &third) != &second) {
        return 1;
    }

    const auto backward = Algorithms::move_first_n_backwards(&first, 1);
    if (backward.m_first != &second || backward.m_second != &first || Traits::get_next(&third) != &first ||
        Traits::get_next(&first) != nullptr) {
        return 2;
    }
    Traits::set_next(&source, backward.m_first);

    const auto forward = Algorithms::move_first_n_forward(backward.m_first, 1);
    if (forward.m_first != &first || forward.m_second != &third || Traits::get_next(&third) != nullptr) {
        return 3;
    }
    Traits::set_next(&source, forward.m_first);

    Algorithms::transfer_after(&destination, &source);
    if (!Algorithms::is_empty(&source) || Traits::get_next(&destination) != &first) {
        return 4;
    }

    Algorithms::link_after(&source, &spare);
    Algorithms::swap_trailing_nodes(&source, &destination);
    if (Traits::get_next(&source) != &first || Traits::get_next(&destination) != &spare) {
        return 5;
    }

    auto disposed = 0;
    const auto disposed_count = Algorithms::detach_and_dispose(&source, [&disposed](Node *const) noexcept -> void { ++disposed; });
    if (disposed_count != 3 || disposed != 3 || !Algorithms::is_empty(&source)) {
        return 6;
    }

    Algorithms::set_sentinel(&source);
    if (!Algorithms::is_sentinel(&source)) {
        return 7;
    }

    Algorithms::init(&first);
    Algorithms::init(&second);
    Algorithms::init(&third);
    Algorithms::link_after(&first, &second);
    Algorithms::link_after(&second, &third);
    const auto reversed = Algorithms::reverse(&first);
    return reversed == &third && Traits::get_next(&third) == &second && Traits::get_next(&second) == &first &&
                   Traits::get_next(&first) == nullptr
               ? 0
               : 8;
}
