/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_UNORDERED_SET_HPP
#define USTL_COLLECTIONS_INTRUSIVE_UNORDERED_SET_HPP 1

#include <span>
#include <utility>

#include <ustl/option.hpp>
#include <ustl/collections/intrusive/hashtable.hpp>
#include <ustl/collections/intrusive/unordered_set_hook.hpp>

namespace ustl::collections::intrusive {
    template <typename T, typename... Options>
    class UnorderedSet final {
        using Table = Hashtable<T, Options...>;

    public:
        using Element = typename Table::Element;
        using Size = typename Table::Size;
        using Hasher = typename Table::Hasher;
        using KeyEqual = typename Table::KeyEqual;
        using BucketType = typename Table::BucketType;
        using IterMut = typename Table::IterMut;
        using Iter = typename Table::Iter;

        constexpr explicit UnorderedSet(std::span<BucketType> buckets, Hasher hasher = {}, KeyEqual equal = {}) noexcept
            : m_table(buckets, std::move(hasher), std::move(equal)) {}

        constexpr UnorderedSet(UnorderedSet const &) = delete;
        constexpr auto operator=(UnorderedSet const &) -> UnorderedSet & = delete;

        [[nodiscard]] constexpr auto begin() noexcept -> IterMut { return m_table.begin(); }
        [[nodiscard]] constexpr auto begin() const noexcept -> Iter { return m_table.begin(); }
        [[nodiscard]] constexpr auto end() noexcept -> IterMut { return m_table.end(); }
        [[nodiscard]] constexpr auto end() const noexcept -> Iter { return m_table.end(); }
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return m_table.empty(); }
        [[nodiscard]] constexpr auto size() const noexcept -> Size { return m_table.size(); }
        [[nodiscard]] constexpr auto bucket_count() const noexcept -> Size { return m_table.bucket_count(); }
        [[nodiscard]] constexpr auto insert(T &value) noexcept -> std::pair<IterMut, bool> { return m_table.insert_unique(value); }
        constexpr auto erase(T &value) noexcept -> void { m_table.erase(value); }
        constexpr auto clear() noexcept -> void { m_table.clear(); }
        constexpr auto rehash(std::span<BucketType> buckets) noexcept -> void { m_table.rehash(buckets); }
        [[nodiscard]] constexpr auto find(T const &value) noexcept -> ustl::Option<T &> { return m_table.find(value); }
        [[nodiscard]] constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
            return m_table.find(value);
        }
        [[nodiscard]] constexpr auto count(T const &value) const noexcept -> Size { return m_table.count(value); }

    private:
        Table m_table;
    };

    template <typename T, typename... Options>
    class UnorderedMultiset final {
        using Table = Hashtable<T, Options...>;

    public:
        using Element = typename Table::Element;
        using Size = typename Table::Size;
        using Hasher = typename Table::Hasher;
        using KeyEqual = typename Table::KeyEqual;
        using BucketType = typename Table::BucketType;
        using IterMut = typename Table::IterMut;
        using Iter = typename Table::Iter;

        constexpr explicit UnorderedMultiset(std::span<BucketType> buckets, Hasher hasher = {}, KeyEqual equal = {}) noexcept
            : m_table(buckets, std::move(hasher), std::move(equal)) {}

        constexpr UnorderedMultiset(UnorderedMultiset const &) = delete;
        constexpr auto operator=(UnorderedMultiset const &) -> UnorderedMultiset & = delete;

        [[nodiscard]] constexpr auto begin() noexcept -> IterMut { return m_table.begin(); }
        [[nodiscard]] constexpr auto begin() const noexcept -> Iter { return m_table.begin(); }
        [[nodiscard]] constexpr auto end() noexcept -> IterMut { return m_table.end(); }
        [[nodiscard]] constexpr auto end() const noexcept -> Iter { return m_table.end(); }
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return m_table.empty(); }
        [[nodiscard]] constexpr auto size() const noexcept -> Size { return m_table.size(); }
        [[nodiscard]] constexpr auto bucket_count() const noexcept -> Size { return m_table.bucket_count(); }
        [[nodiscard]] constexpr auto insert(T &value) noexcept -> IterMut { return m_table.insert_equal(value); }
        constexpr auto erase(T &value) noexcept -> void { m_table.erase(value); }
        constexpr auto clear() noexcept -> void { m_table.clear(); }
        constexpr auto rehash(std::span<BucketType> buckets) noexcept -> void { m_table.rehash(buckets); }
        [[nodiscard]] constexpr auto find(T const &value) noexcept -> ustl::Option<T &> { return m_table.find(value); }
        [[nodiscard]] constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
            return m_table.find(value);
        }
        [[nodiscard]] constexpr auto count(T const &value) const noexcept -> Size { return m_table.count(value); }

    private:
        Table m_table;
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_UNORDERED_SET_HPP
