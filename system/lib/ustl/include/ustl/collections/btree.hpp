/// Copyright(C) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef USTL_COLLECTIONS_BTREE_HPP
#define USTL_COLLECTIONS_BTREE_HPP 1

#include <ustl/collections/detail/btree.hpp>

#include <functional>
#include <memory>
#include <tuple>
#include <utility>

namespace ustl::collections {
    using BTreeNodeView = detail::BTreeNodeView;
    using NullBTreeObserver = detail::NullBTreeObserver;
    using BTreeDefaultOptions = detail::BTreeDefaultOptions;

    template <
        typename Key,
        typename Value,
        typename KeyOfValue,
        typename Compare,
        typename Allocator,
        typename Observer = NullBTreeObserver,
        typename Options = BTreeDefaultOptions
    >
    using BTree = detail::BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options>;

    template <
        typename Key,
        typename T,
        typename Compare = std::less<Key>,
        typename Allocator = std::allocator<std::pair<Key const, T>>,
        typename Observer = NullBTreeObserver,
        typename Options = BTreeDefaultOptions
    >
    class BTreeMap {
        using Self = BTreeMap;
        using KeyOfValue = detail::SelectFirstKeyOfValue<std::pair<Key const, T>>;
        using Impl = BTree<Key, std::pair<Key const, T>, KeyOfValue, Compare, Allocator, Observer, Options>;

      public:
        using key_type = Key;
        using mapped_type = T;
        using value_type = std::pair<Key const, T>;
        using key_compare = Compare;
        using allocator_type = Allocator;
        using observer_type = Observer;
        using options_type = Options;

        using size_type = typename Impl::size_type;
        using difference_type = typename Impl::difference_type;
        using reference = typename Impl::reference;
        using const_reference = typename Impl::const_reference;
        using pointer = typename Impl::pointer;
        using const_pointer = typename Impl::const_pointer;
        using IterMut = typename Impl::iterator;
        using Iter = typename Impl::const_iterator;
        using iterator = typename Impl::iterator;
        using const_iterator = typename Impl::const_iterator;
        using reverse_iterator = typename Impl::reverse_iterator;
        using const_reverse_iterator = typename Impl::const_reverse_iterator;
        using InsertResult = typename Impl::InsertResult;

        BTreeMap() = default;
        explicit BTreeMap(key_compare const &compare)
            : m_tree(compare) {}

        explicit BTreeMap(allocator_type const &allocator)
            : m_tree(allocator) {}

        BTreeMap(key_compare const &compare, allocator_type const &allocator)
            : m_tree(compare, allocator) {}

        BTreeMap(Self &&) noexcept = default;
        auto operator=(Self &&) noexcept -> Self & = default;

        BTreeMap(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;

        auto get_allocator() const -> allocator_type {
            return m_tree.get_allocator();
        }

        auto key_comp() const -> key_compare {
            return m_tree.key_comp();
        }

        auto observer() -> observer_type & {
            return m_tree.observer();
        }

        auto observer() const -> observer_type const & {
            return m_tree.observer();
        }

        auto begin() -> iterator { return m_tree.begin(); }
        auto begin() const -> const_iterator { return m_tree.begin(); }
        auto cbegin() const -> const_iterator { return m_tree.cbegin(); }

        auto end() -> iterator { return m_tree.end(); }
        auto end() const -> const_iterator { return m_tree.end(); }
        auto cend() const -> const_iterator { return m_tree.cend(); }

        auto rbegin() -> reverse_iterator { return m_tree.rbegin(); }
        auto rbegin() const -> const_reverse_iterator { return m_tree.rbegin(); }
        auto crbegin() const -> const_reverse_iterator { return m_tree.crbegin(); }

        auto rend() -> reverse_iterator { return m_tree.rend(); }
        auto rend() const -> const_reverse_iterator { return m_tree.rend(); }
        auto crend() const -> const_reverse_iterator { return m_tree.crend(); }

        constexpr auto empty() const -> bool { return m_tree.empty(); }
        constexpr auto size() const -> size_type { return m_tree.size(); }
        auto max_size() const -> size_type { return m_tree.max_size(); }

        auto clear() -> void { m_tree.clear(); }

        auto find(key_type const &key) -> iterator { return m_tree.find(key); }
        auto find(key_type const &key) const -> const_iterator { return m_tree.find(key); }

        auto lower_bound(key_type const &key) -> iterator { return m_tree.lower_bound(key); }
        auto lower_bound(key_type const &key) const -> const_iterator { return m_tree.lower_bound(key); }

        auto upper_bound(key_type const &key) -> iterator { return m_tree.upper_bound(key); }
        auto upper_bound(key_type const &key) const -> const_iterator { return m_tree.upper_bound(key); }

        auto equal_range(key_type const &key) -> std::pair<iterator, iterator> {
            return m_tree.equal_range(key);
        }

        auto equal_range(key_type const &key) const -> std::pair<const_iterator, const_iterator> {
            return m_tree.equal_range(key);
        }

        auto contains(key_type const &key) const -> bool { return m_tree.contains(key); }
        auto count(key_type const &key) const -> size_type { return m_tree.count(key); }

        auto insert(value_type const &value) -> InsertResult { return m_tree.insert(value); }
        auto insert(value_type &&value) -> InsertResult { return m_tree.insert(std::move(value)); }

        auto insert_or_assign(key_type const &key, mapped_type const &value) -> InsertResult {
            auto iter = find(key);
            if (iter != end()) {
                iter->second = value;
                return InsertResult {
                    .iter = iter,
                    .inserted = false,
                    .status = ours::Status::Ok,
                };
            }
            return insert(value_type(key, value));
        }

        auto insert_or_assign(key_type const &key, mapped_type &&value) -> InsertResult {
            auto iter = find(key);
            if (iter != end()) {
                iter->second = std::move(value);
                return InsertResult {
                    .iter = iter,
                    .inserted = false,
                    .status = ours::Status::Ok,
                };
            }
            return insert(value_type(key, std::move(value)));
        }

        template <typename... Args>
        auto try_emplace(key_type const &key, Args &&...args) -> InsertResult {
            auto iter = find(key);
            if (iter != end()) {
                return InsertResult {
                    .iter = iter,
                    .inserted = false,
                    .status = ours::Status::AlreadyExists,
                };
            }
            return m_tree.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(key),
                std::forward_as_tuple(std::forward<Args>(args)...)
            );
        }

        auto erase(iterator pos) -> iterator { return m_tree.erase(pos); }
        auto erase(iterator first, iterator last) -> iterator { return m_tree.erase(first, last); }
        auto erase(key_type const &key) -> size_type { return m_tree.erase(key); }

        auto swap(Self &other) noexcept -> void {
            m_tree.swap(other.m_tree);
        }

      private:
        Impl m_tree {};
    };

    template <
        typename Key,
        typename Compare = std::less<Key>,
        typename Allocator = std::allocator<Key>,
        typename Observer = NullBTreeObserver,
        typename Options = BTreeDefaultOptions
    >
    class BTreeSet {
        using Self = BTreeSet;
        using KeyOfValue = detail::IdentityKeyOfValue<Key>;
        using Impl = BTree<Key, Key, KeyOfValue, Compare, Allocator, Observer, Options>;

      public:
        using key_type = Key;
        using value_type = Key;
        using key_compare = Compare;
        using allocator_type = Allocator;
        using observer_type = Observer;
        using options_type = Options;

        using size_type = typename Impl::size_type;
        using difference_type = typename Impl::difference_type;
        using reference = typename Impl::reference;
        using const_reference = typename Impl::const_reference;
        using pointer = typename Impl::pointer;
        using const_pointer = typename Impl::const_pointer;
        using IterMut = typename Impl::iterator;
        using Iter = typename Impl::const_iterator;
        using iterator = typename Impl::iterator;
        using const_iterator = typename Impl::const_iterator;
        using reverse_iterator = typename Impl::reverse_iterator;
        using const_reverse_iterator = typename Impl::const_reverse_iterator;
        using InsertResult = typename Impl::InsertResult;

        BTreeSet() = default;
        explicit BTreeSet(key_compare const &compare)
            : m_tree(compare) {}

        explicit BTreeSet(allocator_type const &allocator)
            : m_tree(allocator) {}

        BTreeSet(key_compare const &compare, allocator_type const &allocator)
            : m_tree(compare, allocator) {}

        BTreeSet(Self &&) noexcept = default;
        auto operator=(Self &&) noexcept -> Self & = default;

        BTreeSet(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;

        auto get_allocator() const -> allocator_type {
            return m_tree.get_allocator();
        }

        auto key_comp() const -> key_compare {
            return m_tree.key_comp();
        }

        auto observer() -> observer_type & {
            return m_tree.observer();
        }

        auto observer() const -> observer_type const & {
            return m_tree.observer();
        }

        auto begin() -> iterator { return m_tree.begin(); }
        auto begin() const -> const_iterator { return m_tree.begin(); }
        auto cbegin() const -> const_iterator { return m_tree.cbegin(); }

        auto end() -> iterator { return m_tree.end(); }
        auto end() const -> const_iterator { return m_tree.end(); }
        auto cend() const -> const_iterator { return m_tree.cend(); }

        auto rbegin() -> reverse_iterator { return m_tree.rbegin(); }
        auto rbegin() const -> const_reverse_iterator { return m_tree.rbegin(); }
        auto crbegin() const -> const_reverse_iterator { return m_tree.crbegin(); }

        auto rend() -> reverse_iterator { return m_tree.rend(); }
        auto rend() const -> const_reverse_iterator { return m_tree.rend(); }
        auto crend() const -> const_reverse_iterator { return m_tree.crend(); }

        constexpr auto empty() const -> bool { return m_tree.empty(); }
        constexpr auto size() const -> size_type { return m_tree.size(); }
        auto max_size() const -> size_type { return m_tree.max_size(); }

        auto clear() -> void { m_tree.clear(); }

        auto find(key_type const &key) -> iterator { return m_tree.find(key); }
        auto find(key_type const &key) const -> const_iterator { return m_tree.find(key); }

        auto lower_bound(key_type const &key) -> iterator { return m_tree.lower_bound(key); }
        auto lower_bound(key_type const &key) const -> const_iterator { return m_tree.lower_bound(key); }

        auto upper_bound(key_type const &key) -> iterator { return m_tree.upper_bound(key); }
        auto upper_bound(key_type const &key) const -> const_iterator { return m_tree.upper_bound(key); }

        auto equal_range(key_type const &key) -> std::pair<iterator, iterator> {
            return m_tree.equal_range(key);
        }

        auto equal_range(key_type const &key) const -> std::pair<const_iterator, const_iterator> {
            return m_tree.equal_range(key);
        }

        auto contains(key_type const &key) const -> bool { return m_tree.contains(key); }
        auto count(key_type const &key) const -> size_type { return m_tree.count(key); }

        auto insert(value_type const &value) -> InsertResult { return m_tree.insert(value); }
        auto insert(value_type &&value) -> InsertResult { return m_tree.insert(std::move(value)); }

        template <typename... Args>
        auto emplace(Args &&...args) -> InsertResult {
            return m_tree.emplace(std::forward<Args>(args)...);
        }

        auto erase(iterator pos) -> iterator { return m_tree.erase(pos); }
        auto erase(iterator first, iterator last) -> iterator { return m_tree.erase(first, last); }
        auto erase(key_type const &key) -> size_type { return m_tree.erase(key); }

        auto swap(Self &other) noexcept -> void {
            m_tree.swap(other.m_tree);
        }

      private:
        Impl m_tree {};
    };
} // namespace ustl::collections

#endif // #ifndef USTL_COLLECTIONS_BTREE_HPP
