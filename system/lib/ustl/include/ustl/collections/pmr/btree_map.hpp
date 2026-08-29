/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_COLLECTIONS_PMR_BTREE_MAP_HPP
#define USTL_COLLECTIONS_PMR_BTREE_MAP_HPP 1

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/btree.hpp>

#include <utility>

namespace ustl::collections::pmr {
template <typename Key, typename T,
          typename Compare = ustl::collections::detail::DefaultCompare<Key>,
          typename Observer = ustl::collections::NullBTreeObserver,
          typename Options = ustl::collections::BTreeDefaultOptions>
using BTreeMap =
    ustl::collections::BTreeMap<Key, T, Compare, PolymorphicAllocator<std::pair<Key const, T>>,
                                Observer, Options>;
} // namespace ustl::collections::pmr

#endif // USTL_COLLECTIONS_PMR_BTREE_MAP_HPP
