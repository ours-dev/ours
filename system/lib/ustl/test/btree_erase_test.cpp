/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/btree.hpp>

#include <cassert>

auto main() -> int {
    ustl::collections::BTreeSet<int> tree{};
    for (auto value = 1; value <= 20; ++value) {
        const auto inserted = tree.insert(value);
        assert(inserted.is_ok());
        assert(inserted.value().second);
    }

    auto next = tree.erase(tree.begin());
    assert(next != tree.end());
    assert(*next == 2);
    assert(tree.find(1).is_none());
    assert(tree.size() == 19U);

    assert(tree.erase(10) == 1U);
    assert(tree.find(10).is_none());
    assert(tree.size() == 18U);
}
