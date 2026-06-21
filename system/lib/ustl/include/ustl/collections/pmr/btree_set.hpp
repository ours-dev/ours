/// Copyright(C) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef USTL_COLLECTIONS_PMR_BTREE_SET_HPP
#define USTL_COLLECTIONS_PMR_BTREE_SET_HPP 1

#include <ustl/collections/btree.hpp>

#include <boost/container/pmr/set.hpp>

#include <functional>

namespace ustl::collections::pmr {
    template <
        typename Key,
        typename Compare = std::less<Key>,
        typename Observer = NullBTreeObserver,
        typename Options = BTreeDefaultOptions
    >
    using BTreeSet = ustl::collections::BTreeSet<
        Key,
        Compare,
        boost::container::pmr::polymorphic_allocator<Key>,
        Observer,
        Options
    >;
} // namespace ustl::collections::pmr

#endif // #ifndef USTL_COLLECTIONS_PMR_BTREE_SET_HPP
