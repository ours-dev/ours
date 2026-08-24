/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/circular_list_algorithms.hpp>
#include <ustl/collections/intrusive/detail/list_node.hpp>

namespace {
    using Node = ustl::collections::intrusive::ListNode<void *>;
    using Traits = ustl::collections::intrusive::ListNodeTraits<void *>;
    using Algorithms = ustl::collections::intrusive::CircularListAlgorithms<Traits>;
} // namespace

auto main() -> int {
    Node first{};
    Node second{};
    Node third{};
    Node spare{};
    Node header{};
    Node second_header{};

    Algorithms::init_header(&header);
    Algorithms::init_header(&second_header);
    Algorithms::init(&first);
    Algorithms::init(&second);
    Algorithms::init(&third);
    Algorithms::init(&spare);
    Algorithms::link_before(&header, &first);
    Algorithms::link_before(&header, &second);
    Algorithms::link_before(&header, &third);
    if (Algorithms::is_empty(&header) || Algorithms::count(&header) != 4 ||
        Algorithms::distance(Traits::get_next(&header), &header) != 3) {
        return 1;
    }

    Algorithms::transfer(&second_header, &first, &third);
    if (Algorithms::count(&header) != 2 || Algorithms::count(&second_header) != 3 ||
        Traits::get_next(&second_header) != &first || Traits::get_previous(&second_header) != &second) {
        return 2;
    }

    Algorithms::reverse(&second_header);
    Algorithms::move_backwards(&second, 1);
    if (Traits::get_next(&second_header) != &first || Traits::get_previous(&second_header) != &second) {
        return 3;
    }

    Algorithms::move_forward(&second, 1);
    if (Traits::get_next(&second_header) != &second || Traits::get_previous(&second_header) != &first) {
        return 4;
    }

    Algorithms::StablePartitionInfo info{};
    Algorithms::stable_partition(
        Traits::get_next(&second_header),
        &second_header,
        [&first](Node *const node) noexcept -> bool { return node == &first; },
        info);
    if (info.m_num_first_partition != 1 || info.m_num_second_partition != 1 ||
        info.m_begin_second_partition != &second || Traits::get_next(&second_header) != &first) {
        return 5;
    }

    Algorithms::swap_nodes(&spare, &first);
    return Algorithms::inited(&first) && Traits::get_next(&second_header) == &spare ? 0 : 6;
}
