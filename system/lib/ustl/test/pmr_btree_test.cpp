/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cassert>

#include <ustl/collections/pmr/btree_map.hpp>
#include <ustl/collections/pmr/btree_set.hpp>

auto main() -> int {
    ustl::collections::pmr::BTreeSet<int> set {};
    const auto set_insert = set.insert(2);
    assert(set_insert.is_ok());
    assert(set.find(2).is_some());

    ustl::collections::pmr::BTreeMap<int, int> map {};
    const auto map_insert = map.try_emplace(4, 40);
    assert(map_insert.is_ok());
    assert(map_insert.value().first->second == 40);
}
