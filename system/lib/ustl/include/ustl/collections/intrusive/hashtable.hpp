/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_HASHTABLE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_HASHTABLE_HPP 1

#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/hashtable_node.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>
#include <ustl/collections/intrusive/unordered_set_hook.hpp>
#include <ustl/option.hpp>

#include <functional>
#include <span>
#include <type_traits>
#include <utility>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename T, typename... Options>
class Hashtable final {
    struct Defaults: HookDefaults {
        using ProtoValueTraits = UnorderedSetBaseHook<>;
        using Hash = std::hash<T>;
        using Equal = std::equal_to<T>;
    };

    using PackedOptions = typename PackOptions<Defaults, Options...>::Type;
    using Hook = typename PackedOptions::ProtoValueTraits;
    using NodeTraits = typename Hook::NodeTraits;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;

  public:
    using Element = T;
    using Size = std::size_t;
    using Hasher = typename PackedOptions::Hash;
    using KeyEqual = typename PackedOptions::Equal;
    using BucketType = detail::HashBucket<NodeTraits>;

    template <typename ValuePointer, typename TablePointer>
    class BasicIterator final {
      public:
        constexpr BasicIterator() noexcept = default;

        [[nodiscard]]
        constexpr auto operator*() const noexcept -> decltype(*std::declval<ValuePointer>()) {
            return *operator->();
        }
        [[nodiscard]]
        constexpr auto operator->() const noexcept -> ValuePointer {
            return Hashtable::to_value(m_node);
        }

        constexpr auto operator++() noexcept -> BasicIterator & {
            m_table->advance(m_bucket_index, m_node);
            return *this;
        }

        [[nodiscard]]
        friend constexpr auto operator==(BasicIterator const &left,
                                         BasicIterator const &right) noexcept -> bool {
            return left.m_table == right.m_table && left.m_bucket_index == right.m_bucket_index &&
                   left.m_node == right.m_node;
        }

      private:
        friend class Hashtable;

        constexpr BasicIterator(TablePointer const table, std::size_t const bucket_index,
                                NodePtr const node) noexcept
            : m_table(table), m_bucket_index(bucket_index), m_node(node) {}

        TablePointer m_table{};
        std::size_t m_bucket_index{};
        NodePtr m_node{};
    };

    using IterMut = BasicIterator<T *, Hashtable *>;
    using Iter = BasicIterator<T const *, Hashtable const *>;

    static_assert(std::is_nothrow_invocable_r_v<std::size_t, Hasher const &, T const &>);
    static_assert(std::is_nothrow_invocable_r_v<bool, KeyEqual const &, T const &, T const &>);

    constexpr explicit Hashtable(std::span<BucketType> buckets, Hasher hasher = {},
                                 KeyEqual equal = {}) noexcept
        : m_buckets(buckets), m_hasher(std::move(hasher)), m_equal(std::move(equal)) {
        detail::require(!m_buckets.empty());
        clear_buckets(m_buckets);
    }

    constexpr Hashtable(Hashtable const &) = delete;
    constexpr auto operator=(Hashtable const &) -> Hashtable & = delete;
    constexpr ~Hashtable() noexcept {
        clear();
    }

    [[nodiscard]]
    constexpr auto begin() noexcept -> IterMut {
        auto bucket_index = std::size_t{};
        auto node = NodePtr{};
        first_node(bucket_index, node);
        return IterMut(this, bucket_index, node);
    }

    [[nodiscard]]
    constexpr auto begin() const noexcept -> Iter {
        auto bucket_index = std::size_t{};
        auto node = NodePtr{};
        first_node(bucket_index, node);
        return Iter(this, bucket_index, node);
    }

    [[nodiscard]]
    constexpr auto end() noexcept -> IterMut {
        return IterMut(this, m_buckets.size(), NodePtr{});
    }
    [[nodiscard]]
    constexpr auto end() const noexcept -> Iter {
        return Iter(this, m_buckets.size(), NodePtr{});
    }
    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_size == 0U;
    }
    [[nodiscard]]
    constexpr auto size() const noexcept -> Size {
        return m_size;
    }
    [[nodiscard]]
    constexpr auto bucket_count() const noexcept -> Size {
        return m_buckets.size();
    }

    [[nodiscard]]
    constexpr auto insert_unique(T &value) noexcept -> std::pair<IterMut, bool> {
        const auto hash = m_hasher(value);
        const auto bucket_index = bucket_for_hash(hash);
        if (const auto node = find_node(value, hash, bucket_index); node != NodePtr{}) {
            return {IterMut(this, bucket_index, node), false};
        }
        const auto node = to_node(value);
        detail::require(!NodeTraits::is_linked(node));
        link_node(node, hash, bucket_index);
        return {IterMut(this, bucket_index, node), true};
    }

    [[nodiscard]]
    constexpr auto insert_equal(T &value) noexcept -> IterMut {
        const auto hash = m_hasher(value);
        const auto bucket_index = bucket_for_hash(hash);
        const auto node = to_node(value);
        detail::require(!NodeTraits::is_linked(node));
        link_node(node, hash, bucket_index);
        return IterMut(this, bucket_index, node);
    }

    constexpr auto erase(T &value) noexcept -> void {
        const auto node = to_node(value);
        if (NodeTraits::is_linked(node)) {
            unlink_node(node, NodeTraits::get_hash(node));
        }
    }

    constexpr auto clear() noexcept -> void {
        for (auto &bucket: m_buckets) {
            auto node = bucket.m_head;
            bucket.m_head = NodePtr{};
            while (node != NodePtr{}) {
                const auto next = NodeTraits::get_next(node);
                NodeTraits::init(node);
                node = next;
            }
        }
        m_size = 0U;
    }

    [[nodiscard]]
    constexpr auto find(T const &value) noexcept -> ustl::Option<T &> {
        const auto hash = m_hasher(value);
        const auto node = find_node(value, hash, bucket_for_hash(hash));
        return node == NodePtr{} ? ustl::NONE : ustl::some(*to_value(node));
    }

    [[nodiscard]]
    constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
        const auto hash = m_hasher(value);
        const auto node = find_node(value, hash, bucket_for_hash(hash));
        return node == NodePtr{} ? ustl::NONE : ustl::some(*to_value(node));
    }

    [[nodiscard]]
    constexpr auto count(T const &value) const noexcept -> Size {
        const auto hash = m_hasher(value);
        auto node = m_buckets[bucket_for_hash(hash)].m_head;
        auto result = Size{};
        while (node != NodePtr{}) {
            if (NodeTraits::get_hash(node) == hash && m_equal(*to_value(node), value)) {
                ++result;
            }
            node = NodeTraits::get_next(node);
        }
        return result;
    }

    constexpr auto rehash(std::span<BucketType> buckets) noexcept -> void {
        detail::require(!buckets.empty());
        if (buckets.data() == m_buckets.data() && buckets.size() == m_buckets.size()) {
            return;
        }
        detail::require(buckets.data() != m_buckets.data());
        clear_buckets(buckets);
        const auto old_buckets = m_buckets;
        m_buckets = buckets;
        for (auto &bucket: old_buckets) {
            auto node = bucket.m_head;
            bucket.m_head = NodePtr{};
            while (node != NodePtr{}) {
                const auto next = NodeTraits::get_next(node);
                const auto bucket_index = bucket_for_hash(NodeTraits::get_hash(node));
                NodeTraits::set_next(node, m_buckets[bucket_index].m_head);
                m_buckets[bucket_index].m_head = node;
                node = next;
            }
        }
    }

  private:
    [[nodiscard]]
    constexpr auto bucket_for_hash(std::size_t const hash) const noexcept -> std::size_t {
        return hash % m_buckets.size();
    }

    static constexpr auto clear_buckets(std::span<BucketType> buckets) noexcept -> void {
        for (auto &bucket: buckets) {
            bucket.m_head = NodePtr{};
        }
    }

    constexpr auto first_node(std::size_t &bucket_index, NodePtr &node) const noexcept -> void {
        while (bucket_index < m_buckets.size()) {
            node = m_buckets[bucket_index].m_head;
            if (node != NodePtr{}) {
                return;
            }
            ++bucket_index;
        }
    }

    constexpr auto advance(std::size_t &bucket_index, NodePtr &node) const noexcept -> void {
        node = NodeTraits::get_next(node);
        if (node != NodePtr{}) {
            return;
        }
        ++bucket_index;
        first_node(bucket_index, node);
    }

    [[nodiscard]]
    constexpr auto find_node(T const &value, std::size_t const hash,
                             std::size_t const bucket_index) const noexcept -> NodePtr {
        auto node = m_buckets[bucket_index].m_head;
        while (node != NodePtr{}) {
            if (NodeTraits::get_hash(node) == hash && m_equal(*to_value(node), value)) {
                return node;
            }
            node = NodeTraits::get_next(node);
        }
        return NodePtr{};
    }

    constexpr auto link_node(NodePtr const node, std::size_t const hash,
                             std::size_t const bucket_index) noexcept -> void {
        NodeTraits::set_hash(node, hash);
        NodeTraits::set_next(node, m_buckets[bucket_index].m_head);
        NodeTraits::set_linked(node, true);
        m_buckets[bucket_index].m_head = node;
        ++m_size;
    }

    constexpr auto unlink_node(NodePtr const node, std::size_t const hash) noexcept -> void {
        const auto bucket_index = bucket_for_hash(hash);
        auto previous = NodePtr{};
        auto current = m_buckets[bucket_index].m_head;
        while (current != NodePtr{}) {
            if (current == node) {
                const auto next = NodeTraits::get_next(node);
                if (previous == NodePtr{}) {
                    m_buckets[bucket_index].m_head = next;
                } else {
                    NodeTraits::set_next(previous, next);
                }
                NodeTraits::init(node);
                --m_size;
                return;
            }
            previous = current;
            current = NodeTraits::get_next(current);
        }
    }

    [[nodiscard]]
    static constexpr auto to_node(T &value) noexcept -> NodePtr {
        if constexpr (requires { Hook::to_node_ptr(value); }) {
            return Hook::to_node_ptr(value);
        } else {
            return static_cast<Hook &>(value).this_ptr();
        }
    }

    [[nodiscard]]
    static constexpr auto to_value(NodePtr const node) noexcept -> T * {
        if constexpr (requires { Hook::to_value_ptr(node); }) {
            return PointerTraits<decltype(Hook::to_value_ptr(node))>::to_raw_pointer(
                Hook::to_value_ptr(node));
        } else {
            auto *const hook = static_cast<Hook *>(static_cast<Node *>(node));
            return static_cast<T *>(hook);
        }
    }

    std::span<BucketType> m_buckets;
    [[no_unique_address]]
    Hasher m_hasher{};
    [[no_unique_address]]
    KeyEqual m_equal{};
    Size m_size{};
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_HASHTABLE_HPP
