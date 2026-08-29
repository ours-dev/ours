/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/rbtree.hpp>

namespace {
struct Value final: ustl::collections::intrusive::SetBaseHook<> {
    int m_key{};
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

using RBTree =
    ustl::collections::intrusive::RBTree<Value, ustl::collections::intrusive::Compare<Compare>>;
} // namespace

auto main() -> int {
    Value one{.m_key = 1};
    Value two{.m_key = 2};
    Value duplicate{.m_key = 2};
    RBTree tree{};
    const auto [one_it, inserted_one] = tree.insert_unique(one);
    const auto [two_it, inserted_two] = tree.insert_unique(two);
    const auto [duplicate_it, inserted_duplicate] = tree.insert_unique(duplicate);
    if (!inserted_one || !inserted_two || inserted_duplicate || one_it->m_key != 1 ||
        two_it->m_key != 2 || duplicate_it->m_key != 2) {
        return 1;
    }

    const auto duplicate_it_equal = tree.insert_equal(duplicate);
    const auto found = tree.find(two);
    if (duplicate_it_equal->m_key != 2 || !found || &found.value() != &two ||
        tree.count(two) != 2U) {
        return 2;
    }

    tree.erase(duplicate);
    return tree.count(two) == 1U && tree.size() == 2U ? 0 : 3;
}
