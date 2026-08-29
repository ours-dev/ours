/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/avltree.hpp>

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

using Tree =
    ustl::collections::intrusive::Avltree<Value, ustl::collections::intrusive::Compare<Compare>>;
} // namespace

auto main() -> int {
    Value one{.m_key = 1};
    Value two{.m_key = 2};
    Value replacement{.m_key = 2};
    Tree tree{};
    static_cast<void>(tree.insert_equal(one));
    static_cast<void>(tree.insert_before(tree.end(), two));
    tree.replace_node(tree.iterator_to(two), replacement);
    const auto after = tree.erase(tree.iterator_to(one));
    return after->m_key == 2 && !one.is_linked() && !two.is_linked() && replacement.is_linked() ? 0
                                                                                                : 1;
}
