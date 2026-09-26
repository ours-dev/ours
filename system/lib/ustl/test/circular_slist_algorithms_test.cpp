/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/circular_slist_algorithms.hpp>
#include <ustl/collections/intrusive/detail/slist_node.hpp>

#include <cstddef>

namespace {
using Node = ustl::collections::intrusive::SlistNode<void *>;
using Traits = ustl::collections::intrusive::SlistNodeTraits<void *>;
using Algorithms = ustl::collections::intrusive::CircularSlistAlgorithms<Traits>;
} // namespace

auto main() -> int {
    Node header{};
    Node destination{};
    Node first{};
    Node second{};
    Node third{};
    Node spare{};

    Algorithms::init_header(&header);
    Algorithms::init_header(&destination);
    Algorithms::init(&first);
    Algorithms::init(&second);
    Algorithms::init(&third);
    Algorithms::init(&spare);
    Algorithms::link_after(&header, &first);
    Algorithms::link_after(&first, &second);
    Algorithms::link_after(&second, &third);
    if (Algorithms::is_empty(&header) || Algorithms::count(&header) != 4 ||
        Algorithms::distance(Traits::get_next(&header), &header) != 3 ||
        Algorithms::end_node(static_cast<const Node *>(&header)) != &header ||
        Algorithms::get_previous_node(&first) != &header ||
        Algorithms::get_previous_previous_node(&first) != &third ||
        Algorithms::move_backwards(&second, 0) != nullptr ||
        Algorithms::move_forward(&second, 0) != nullptr ||
        Algorithms::move_backwards(&second, 3) != nullptr ||
        Algorithms::move_forward(&second, 3) != nullptr || Traits::get_next(&header) != &first ||
        Traits::get_next(&first) != &second) {
        return 1;
    }

    if (Algorithms::move_backwards(&second, 1) != &third || Traits::get_next(&first) != &third ||
        Traits::get_next(&third) != &second) {
        return 2;
    }
    if (Algorithms::move_forward(&second, 1) != &first || Traits::get_next(&first) != &second ||
        Traits::get_next(&second) != &third) {
        return 3;
    }

    Algorithms::link_before(&first, &spare);
    if (Traits::get_next(&header) != &spare) {
        return 4;
    }
    Algorithms::unlink(&spare);
    Algorithms::init(&spare);
    if (Traits::get_next(&header) != &first || !Algorithms::inited(&spare)) {
        return 5;
    }

    Algorithms::reverse(&header);
    if (Traits::get_next(&header) != &third || Traits::get_next(&third) != &second ||
        Traits::get_next(&second) != &first) {
        return 6;
    }

    Algorithms::StablePartitionInfo info{};
    Algorithms::stable_partition(
        &header, &header, [&second](Node *const node) noexcept -> bool { return node != &second; },
        info);
    if (info.m_num_first_partition != 2 || info.m_num_second_partition != 1 ||
        info.m_begin_second_partition != &second || info.m_new_last_node != &second ||
        Traits::get_next(&header) != &third || Traits::get_next(&third) != &first ||
        Traits::get_next(&first) != &second) {
        return 7;
    }

    Algorithms::swap_nodes(&spare, &first);
    if (!Algorithms::inited(&first) || Traits::get_next(&header) != &third ||
        Traits::get_next(&third) != &spare) {
        return 8;
    }

    Algorithms::transfer_after(&destination, &header);
    if (!Algorithms::is_empty(&header) || Algorithms::count(&destination) != 4 ||
        Traits::get_next(&destination) != &third) {
        return 9;
    }

    auto disposed = 0;
    const auto count = Algorithms::detach_and_dispose(
        &destination, [&disposed](Node *const) noexcept -> void { ++disposed; });
    if (count != 3 || disposed != 3 || !Algorithms::is_empty(&destination)) {
        return 10;
    }

    Algorithms::set_sentinel(&destination);
    if (!Algorithms::is_sentinel(&destination)) {
        return 11;
    }

    // swap_nodes() on two list headers, where an empty list is a header that
    // points at itself. The swap of `spare` and `first` above never meets
    // that case: `spare` is unlinked (a null next), not empty. Whichever
    // header was empty must come out pointing at itself, and each argument
    // order below puts the empty header on a different side.
    Node left{};
    Node right{};
    Node one{};
    Node two{};
    Algorithms::init_header(&left);
    Algorithms::init_header(&right);
    Algorithms::swap_nodes(&left, &right);
    if (!Algorithms::is_empty(&left) || !Algorithms::is_empty(&right)) {
        return 12;
    }

    Algorithms::init(&one);
    Algorithms::init(&two);
    Algorithms::link_after(&left, &one);
    Algorithms::link_after(&one, &two);
    Algorithms::swap_nodes(&left, &right);
    if (!Algorithms::is_empty(&left) || Traits::get_next(&right) != &one ||
        Traits::get_next(&one) != &two || Traits::get_next(&two) != &right) {
        return 13;
    }

    Algorithms::swap_nodes(&left, &right);
    if (!Algorithms::is_empty(&right) || Traits::get_next(&left) != &one ||
        Traits::get_next(&one) != &two || Traits::get_next(&two) != &left) {
        return 14;
    }
    return 0;
}
