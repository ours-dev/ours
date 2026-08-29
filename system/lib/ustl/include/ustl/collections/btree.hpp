/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_COLLECTIONS_BTREE_HPP
#define USTL_COLLECTIONS_BTREE_HPP 1

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/detail/btree.hpp>

#include <functional>
#include <utility>

namespace ustl::collections {
using BTreeNodeView = detail::BTreeNodeView;
using NullBTreeObserver = detail::NullBTreeObserver;
using BTreeDefaultOptions = detail::BTreeDefaultOptions;

template <typename Key, typename Value, typename KeyOfValue,
          typename Compare = detail::DefaultCompare<Key>, typename AllocatorType = Allocator<Value>,
          typename Observer = NullBTreeObserver, typename Options = BTreeDefaultOptions>
using BTree = detail::BTree<Key, Value, KeyOfValue, Compare, AllocatorType, Observer, Options>;

template <typename Key, typename T, typename Compare = detail::DefaultCompare<Key>,
          typename AllocatorType = Allocator<std::pair<Key const, T>>,
          typename Observer = NullBTreeObserver, typename Options = BTreeDefaultOptions>
using BTreeMap =
    BTree<Key, std::pair<Key const, T>, detail::SelectFirstKeyOfValue<std::pair<Key const, T>>,
          Compare, AllocatorType, Observer, Options>;

template <typename Key, typename Compare = detail::DefaultCompare<Key>,
          typename AllocatorType = Allocator<Key>, typename Observer = NullBTreeObserver,
          typename Options = BTreeDefaultOptions>
using BTreeSet =
    BTree<Key, Key, detail::IdentityKeyOfValue<Key>, Compare, AllocatorType, Observer, Options>;
} // namespace ustl::collections

#endif // USTL_COLLECTIONS_BTREE_HPP
