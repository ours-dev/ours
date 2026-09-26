/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// The intrusive containers reach an element through mutable nodes even when
// the container itself is const, so every const accessor below builds an
// `Option<T &>` and narrows it to `Option<T const &>` at the return. None of
// them compiled until `Option<T &>` learned that conversion, so calling each
// one on a const container is most of what this test proves. The
// static_asserts pin the narrowed type: an accessor that went back to
// handing out `Option<T &>` from a const container would pass every runtime
// check here.
//
// Bstree, RBTree, Avltree and Splaytree used to get the narrowed type another
// way, by casting the element to `T const &` before `some_ref()`; the casts
// are gone and their lookups now rely on the same conversion, so they are
// pinned here too.

#include <ustl/collections/intrusive/avltree.hpp>
#include <ustl/collections/intrusive/bstree.hpp>
#include <ustl/collections/intrusive/hashtable.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/rbtree.hpp>
#include <ustl/collections/intrusive/sgtree.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/collections/intrusive/splaytree.hpp>
#include <ustl/collections/intrusive/treap.hpp>
#include <ustl/option.hpp>

#include <array>
#include <concepts>
#include <span>
#include <utility>

#include <cassert>
#include <cstddef>

namespace {
namespace intrusive = ustl::collections::intrusive;

struct ListValue final: intrusive::ListBaseHook<> {
    int m_key{};
};

struct SlistValue final: intrusive::SlistBaseHook<> {
    int m_key{};
};

struct TreeValue final: intrusive::BsSetBaseHook<> {
    int m_key{};
};

struct HashValue final: intrusive::UnorderedSetBaseHook<> {
    int m_key{};
};

// Bstree takes the same hook as TreeValue; the other three trees each want
// their own.
struct RbValue final: intrusive::SetBaseHook<> {
    int m_key{};
};

struct AvlValue final: intrusive::AvlSetBaseHook<> {
    int m_key{};
};

struct SplayValue final: intrusive::SplaySetBaseHook<> {
    int m_key{};
};

struct KeyLess final {
    template <typename Value>
    [[nodiscard]]
    constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

// The treap insists on a priority; reusing the key keeps this test about
// lookup rather than about heap order.
struct KeyPriority final {
    using Type = int;

    [[nodiscard]]
    constexpr auto operator()(TreeValue const &value) const noexcept -> int {
        return value.m_key;
    }
};

struct KeyHash final {
    [[nodiscard]]
    constexpr auto operator()(HashValue const &value) const noexcept -> std::size_t {
        return static_cast<std::size_t>(value.m_key);
    }
};

struct KeyEqual final {
    [[nodiscard]]
    constexpr auto operator()(HashValue const &left, HashValue const &right) const noexcept
        -> bool {
        return left.m_key == right.m_key;
    }
};

using List = intrusive::List<ListValue>;
using Slist = intrusive::Slist<SlistValue>;
using Sgtree = intrusive::Sgtree<TreeValue, intrusive::Compare<KeyLess>>;
using Treap = intrusive::Treap<TreeValue, intrusive::Compare<KeyLess>,
                               intrusive::PriorityOfValue<KeyPriority>>;
using Hashtable =
    intrusive::Hashtable<HashValue, intrusive::Hash<KeyHash>, intrusive::Equal<KeyEqual>>;
using Bstree = intrusive::Bstree<TreeValue, intrusive::Compare<KeyLess>>;
using RBTree = intrusive::RBTree<RbValue, intrusive::Compare<KeyLess>>;
using Avltree = intrusive::Avltree<AvlValue, intrusive::Compare<KeyLess>>;
using Splaytree = intrusive::Splaytree<SplayValue, intrusive::Compare<KeyLess>>;

static_assert(
    std::same_as<decltype(std::declval<List const &>().front()), ustl::Option<ListValue const &>>);
static_assert(
    std::same_as<decltype(std::declval<List const &>().back()), ustl::Option<ListValue const &>>);
static_assert(std::same_as<decltype(std::declval<Slist const &>().front()),
                           ustl::Option<SlistValue const &>>);
static_assert(
    std::same_as<decltype(std::declval<Sgtree const &>().find(std::declval<TreeValue const &>())),
                 ustl::Option<TreeValue const &>>);
static_assert(
    std::same_as<decltype(std::declval<Treap const &>().find(std::declval<TreeValue const &>())),
                 ustl::Option<TreeValue const &>>);
static_assert(std::same_as<
              decltype(std::declval<Hashtable const &>().find(std::declval<HashValue const &>())),
              ustl::Option<HashValue const &>>);

// find(), lower_bound() and upper_bound() on a const tree all hand out a const
// element.
template <typename Tree, typename Value>
constexpr auto CONST_LOOKUPS =
    std::same_as<decltype(std::declval<Tree const &>().find(std::declval<Value const &>())),
                 ustl::Option<Value const &>> &&
    std::same_as<decltype(std::declval<Tree const &>().lower_bound(std::declval<Value const &>())),
                 ustl::Option<Value const &>> &&
    std::same_as<decltype(std::declval<Tree const &>().upper_bound(std::declval<Value const &>())),
                 ustl::Option<Value const &>>;

static_assert(CONST_LOOKUPS<Bstree, TreeValue>);
static_assert(CONST_LOOKUPS<RBTree, RbValue>);
static_assert(CONST_LOOKUPS<Avltree, AvlValue>);
static_assert(CONST_LOOKUPS<Splaytree, SplayValue>);

/// One member keyed `key` in an otherwise empty tree, looked up through a
/// const reference: each lookup must find the member itself, or nothing.
template <typename Tree, typename Value>
auto check_const_lookups(int const key) -> void {
    Value member{.m_key = key};
    Value const below{.m_key = key - 1};
    Tree tree{};
    Tree const &const_tree = tree;
    static_cast<void>(tree.insert_unique(member));
    assert(&const_tree.find(member).value() == &member);
    assert(const_tree.find(below).is_none());
    assert(&const_tree.lower_bound(below).value() == &member);
    assert(&const_tree.lower_bound(member).value() == &member);
    assert(&const_tree.upper_bound(below).value() == &member);
    assert(const_tree.upper_bound(member).is_none());
}
} // namespace

auto main() -> int {
    // Each container is declared after the values it links, so it is
    // destroyed, and unlinks them, before they are.
    ListValue list_first{.m_key = 1};
    ListValue list_last{.m_key = 2};
    List list{};
    List const &const_list = list;
    assert(const_list.front().is_none());
    assert(const_list.back().is_none());
    list.push_back(list_first);
    list.push_back(list_last);
    assert(&const_list.front().value() == &list_first);
    assert(&const_list.back().value() == &list_last);

    SlistValue slist_first{.m_key = 3};
    Slist slist{};
    Slist const &const_slist = slist;
    assert(const_slist.front().is_none());
    slist.push_front(slist_first);
    assert(&const_slist.front().value() == &slist_first);

    // An insert that failed would surface as a find that comes back empty,
    // so the insert results are not checked separately.
    TreeValue sgtree_member{.m_key = 4};
    TreeValue const sgtree_absent{.m_key = 5};
    Sgtree sgtree{};
    Sgtree const &const_sgtree = sgtree;
    static_cast<void>(sgtree.insert_unique(sgtree_member));
    assert(&const_sgtree.find(sgtree_member).value() == &sgtree_member);
    assert(const_sgtree.find(sgtree_absent).is_none());

    TreeValue treap_member{.m_key = 6};
    TreeValue const treap_absent{.m_key = 7};
    Treap treap{};
    Treap const &const_treap = treap;
    static_cast<void>(treap.insert_unique(treap_member));
    assert(&const_treap.find(treap_member).value() == &treap_member);
    assert(const_treap.find(treap_absent).is_none());

    HashValue hash_member{.m_key = 8};
    HashValue const hash_absent{.m_key = 9};
    std::array<Hashtable::BucketType, 4> buckets{};
    Hashtable table{std::span{buckets}};
    Hashtable const &const_table = table;
    static_cast<void>(table.insert_unique(hash_member));
    assert(&const_table.find(hash_member).value() == &hash_member);
    assert(const_table.find(hash_absent).is_none());

    check_const_lookups<Bstree, TreeValue>(10);
    check_const_lookups<RBTree, RbValue>(20);
    check_const_lookups<Avltree, AvlValue>(30);
    check_const_lookups<Splaytree, SplayValue>(40);
}
