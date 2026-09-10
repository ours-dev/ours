/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <array>
#include <utility>

#include <cstddef>

#include <ustl/collections/intrusive/avl_set.hpp>

namespace {
struct Value final: ustl::collections::intrusive::AvlSetBaseHook<> {
    int m_key{};
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

using Avltree =
    ustl::collections::intrusive::Avltree<Value, ustl::collections::intrusive::Compare<Compare>>;
using Algorithms = ustl::collections::intrusive::AvltreeAlgorithms<Value::NodeTraits>;
using NodeTraits = Value::NodeTraits;
using NodePtr = NodeTraits::NodePtr;

[[nodiscard]]
auto is_balanced(NodePtr const node) noexcept -> std::pair<bool, std::size_t> {
    if (node == NodePtr{}) {
        return {true, 0U};
    }
    const auto [left_valid, left_height] = is_balanced(NodeTraits::get_left(node));
    const auto [right_valid, right_height] = is_balanced(NodeTraits::get_right(node));
    const auto difference =
        right_height > left_height ? right_height - left_height : left_height - right_height;
    const auto expected_balance = right_height == left_height  ? NodeTraits::zero()
                                  : right_height > left_height ? NodeTraits::positive()
                                                               : NodeTraits::negative();
    return {left_valid && right_valid && difference <= 1U &&
                NodeTraits::get_balance(node) == expected_balance,
            1U + (left_height > right_height ? left_height : right_height)};
}
} // namespace

auto main() -> int {
    std::array<Value, 31U> values{};
    Avltree tree{};
    for (std::size_t index = 0U; index < values.size(); ++index) {
        values[index].m_key = static_cast<int>(index);
        const auto [iterator, inserted] = tree.insert_unique(values[index]);
        if (!inserted || iterator->m_key != static_cast<int>(index)) {
            return 1;
        }
    }

    auto header = values[0].this_ptr();
    while (!Algorithms::is_header(header)) {
        header = NodeTraits::get_parent(header);
    }
    const auto [balanced, height] = is_balanced(NodeTraits::get_parent(header));
    if (!balanced || height > 6U || tree.count(values[20]) != 1U || !tree.find(values[20])) {
        return 2;
    }

    for (std::size_t index = 0U; index < values.size(); index += 2U) {
        tree.erase(values[index]);
    }
    const auto [balanced_after_erase, height_after_erase] =
        is_balanced(NodeTraits::get_parent(header));
    return balanced_after_erase && height_after_erase <= 5U && tree.size() == 15U ? 0 : 3;
}
