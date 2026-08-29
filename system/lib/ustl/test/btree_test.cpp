/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/btree.hpp>

#include <cassert>

auto main() -> int {
    ustl::collections::BTreeSet<int> tree{};

    const auto four = tree.insert(4);
    assert(four.is_ok());
    assert(four.value().second);
    assert(*four.value().first == 4);

    const auto two = tree.insert(2);
    const auto six = tree.insert(6);
    assert(two.is_ok());
    assert(six.is_ok());

    const auto duplicate = tree.insert(4);
    assert(duplicate.is_ok());
    assert(!duplicate.value().second);
    assert(*duplicate.value().first == 4);

    const auto found = tree.find(4);
    const auto missing = tree.find(5);
    assert(found.is_some());
    assert(*found.value() == 4);
    assert(missing.is_none());

    const auto lower = tree.lower_bound(5);
    const auto upper = tree.upper_bound(4);
    assert(lower.is_some());
    assert(*lower.value() == 6);
    assert(upper.is_some());
    assert(*upper.value() == 6);

    const auto after_last = tree.upper_bound(6);
    assert(after_last.is_none());
    assert(tree.size() == 3U);
}
