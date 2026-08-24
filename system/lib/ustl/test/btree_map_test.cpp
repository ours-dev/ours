/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cassert>

#include <ustl/collections/btree.hpp>

auto main() -> int {
    ustl::collections::BTreeMap<int, int> map {};

    const auto first = map.try_emplace(4, 40);
    assert(first.is_ok());
    assert(first.value().second);
    assert(first.value().first->second == 40);

    const auto duplicate = map.try_emplace(4, 400);
    assert(duplicate.is_ok());
    assert(!duplicate.value().second);
    assert(duplicate.value().first->second == 40);

    const auto assigned = map.insert_or_assign(4, 42);
    assert(assigned.is_ok());
    assert(!assigned.value().second);

    const auto found = map.find(4);
    assert(found.is_some());
    assert(found.value()->second == 42);
    assert(map.find(2).is_none());
}
