/// Copyright(C) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef USTL_COLLECTIONS_PMR_BTREE_MAP_HPP
#define USTL_COLLECTIONS_PMR_BTREE_MAP_HPP 1

#include <ustl/collections/btree.hpp>

#include <boost/container/pmr/map.hpp>

#include <functional>
#include <utility>

namespace ustl::collections::pmr {
    template <
        typename Key,
        typename T,
        typename Compare = std::less<Key>,
        typename Observer = NullBTreeObserver,
        typename Options = BTreeDefaultOptions
    >
    using BTreeMap = ustl::collections::BTreeMap<
        Key,
        T,
        Compare,
        boost::container::pmr::polymorphic_allocator<std::pair<Key const, T>>,
        Observer,
        Options
    >;
} // namespace ustl::collections::pmr

#endif // #ifndef USTL_COLLECTIONS_PMR_BTREE_MAP_HPP
