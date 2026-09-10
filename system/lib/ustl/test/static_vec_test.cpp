/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/error.hpp>
#include <ustl/collections/static_vec.hpp>

#include <cassert>

auto main() -> int {
    ustl::collections::StaticVec<int, 2> values{};
    assert(values.empty());
    assert(values.front().is_none());

    assert(values.push_back(2).is_ok());
    assert(values.push_back(4).is_ok());
    assert(values.size() == 2U);
    assert(values.front().value() == 2);
    assert(values.back().value() == 4);
    assert(values.at(2).is_none());

    const auto overflow = values.push_back(6);
    assert(overflow.is_err());
    assert(overflow.error() == ustl::collections::Error::CapacityOverflow);
    assert(values.size() == 2U);

    const auto removed = values.pop_back();
    assert(removed.is_some());
    assert(removed.value() == 4);
    assert(values.back().value() == 2);
}
