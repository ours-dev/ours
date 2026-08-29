/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_COLLECTIONS_PMR_BTREE_SET_HPP
#define USTL_COLLECTIONS_PMR_BTREE_SET_HPP 1

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/btree.hpp>

namespace ustl::collections::pmr {
template <typename Key, typename Compare = ustl::collections::detail::DefaultCompare<Key>,
          typename Observer = ustl::collections::NullBTreeObserver,
          typename Options = ustl::collections::BTreeDefaultOptions>
using BTreeSet =
    ustl::collections::BTreeSet<Key, Compare, PolymorphicAllocator<Key>, Observer, Options>;
} // namespace ustl::collections::pmr

#endif // USTL_COLLECTIONS_PMR_BTREE_SET_HPP
