/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/sg_set.hpp>

#include <array>

namespace {
using Hook = ustl::collections::intrusive::BsSetBaseHook<>;
using Node = Hook::NodeTraits::Node;
using NodePtr = Hook::NodeTraits::NodePtr;
using NodeTraits = Hook::NodeTraits;

struct Value final: Hook {
    int m_key{};
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

using Sgtree =
    ustl::collections::intrusive::Sgtree<Value, ustl::collections::intrusive::Compare<Compare>>;
using SgSet =
    ustl::collections::intrusive::SgSet<Value, ustl::collections::intrusive::Compare<Compare>>;
using SgMultiset =
    ustl::collections::intrusive::SgMultiset<Value, ustl::collections::intrusive::Compare<Compare>>;

[[nodiscard]]
constexpr auto height(NodePtr const node) noexcept -> std::size_t {
    if (node == nullptr) {
        return 0U;
    }
    const auto left_height = height(NodeTraits::get_left(node));
    const auto right_height = height(NodeTraits::get_right(node));
    return 1U + (left_height > right_height ? left_height : right_height);
}

[[nodiscard]]
constexpr auto is_sorted(Sgtree const &tree) noexcept -> bool {
    int previous{};
    bool is_first = true;
    for (auto iterator = tree.begin(); iterator != tree.end(); ++iterator) {
        if (!is_first && iterator->m_key <= previous) {
            return false;
        }
        previous = iterator->m_key;
        is_first = false;
    }
    return true;
}
} // namespace

auto main() -> int {
    std::array<Value, 63> values{};
    Sgtree tree{};
    for (std::size_t index = 0U; index < values.size(); ++index) {
        values[index].m_key = static_cast<int>(index);
        const auto [iterator, inserted] = tree.insert_unique(values[index]);
        if (!inserted || iterator->m_key != static_cast<int>(index)) {
            return 1;
        }
    }

    const auto header = ustl::collections::intrusive::SgtreeAlgorithms<NodeTraits>::get_header(
        static_cast<Hook &>(values.front()).this_ptr());
    if (!is_sorted(tree) || height(NodeTraits::get_parent(header)) > 14U ||
        tree.balance_factor() <= 0.5F) {
        return 2;
    }

    tree.balance_factor(0.6F);
    if (tree.balance_factor() != 0.6F || height(NodeTraits::get_parent(header)) > 10U) {
        return 3;
    }

    for (std::size_t index = 0U; index < 48U; ++index) {
        tree.erase(values[index]);
    }
    if (!is_sorted(tree) || tree.size() != 15U || height(NodeTraits::get_parent(header)) > 6U) {
        return 4;
    }

    Value set_value{.m_key = 100};
    Value set_duplicate{.m_key = 100};
    SgSet set{};
    const auto [set_iterator, inserted_set] = set.insert(set_value);
    const auto [duplicate_iterator, inserted_duplicate] = set.insert(set_duplicate);
    if (!inserted_set || inserted_duplicate || set_iterator->m_key != 100 ||
        duplicate_iterator->m_key != 100) {
        return 5;
    }

    Value multi_one{.m_key = 101};
    Value multi_two{.m_key = 101};
    SgMultiset multiset{};
    auto _ = multiset.insert(multi_one);
    auto _ = multiset.insert(multi_two);
    return multiset.count(multi_one) == 2U ? 0 : 6;
}
