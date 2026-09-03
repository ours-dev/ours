/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SGTREE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SGTREE_HPP 1

#include <ustl/collections/intrusive/bs_set_hook.hpp>
#include <ustl/collections/intrusive/detail/key_node_ptr_compare.hpp>
#include <ustl/collections/intrusive/detail/value_traits_access.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>
#include <ustl/collections/intrusive/sgtree_algorithms.hpp>
#include <ustl/option.hpp>

#include <functional>
#include <type_traits>
#include <utility>

#include <cmath>
#include <cstddef>

namespace ustl::collections::intrusive {
template <typename T, typename... Options>
class Sgtree final {
    struct Defaults: HookDefaults {
        using ProtoValueTraits = BsSetBaseHook<>;
        using Compare = std::less<T>;
        static constexpr bool FLOATING_POINT = true;
    };

    using PackedOptions = typename PackOptions<Defaults, Options...>::Type;
    using Hook = typename PackedOptions::ProtoValueTraits;
    using ValueAccess = detail::ValueTraitsAccess<T, Hook>;
    using NodeTraits = typename Hook::NodeTraits;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using Algorithms = SgtreeAlgorithms<NodeTraits>;
    using NodeCompare = detail::KeyNodePtrCompare<typename PackedOptions::Compare, ValueAccess>;

  public:
    template <typename ValuePointer>
    class BasicIterator final {
      public:
        constexpr BasicIterator() noexcept = default;

        [[nodiscard]]
        constexpr auto operator*() const noexcept -> decltype(*std::declval<ValuePointer>()) {
            return *operator->();
        }
        [[nodiscard]]
        constexpr auto operator->() const noexcept -> ValuePointer {
            return to_value(m_node);
        }

        constexpr auto operator++() noexcept -> BasicIterator & {
            m_node = Algorithms::next_node(m_node);
            return *this;
        }

        constexpr auto operator--() noexcept -> BasicIterator & {
            m_node = Algorithms::prev_node(m_node);
            return *this;
        }

        [[nodiscard]]
        friend constexpr auto operator==(BasicIterator const &left,
                                         BasicIterator const &right) noexcept -> bool {
            return left.m_node == right.m_node;
        }

      private:
        friend class Sgtree;

        constexpr explicit BasicIterator(NodePtr const node) noexcept : m_node(node) {}

        NodePtr m_node{};
    };

    using IterMut = BasicIterator<T *>;
    using Iter = BasicIterator<T const *>;
    using Element = T;
    using Size = std::size_t;
    using KeyCompare = typename PackedOptions::Compare;
    using InsertCommitData = typename Algorithms::InsertCommitData;
    static constexpr bool FLOATING_POINT = PackedOptions::FLOATING_POINT;

    static_assert(std::is_nothrow_invocable_v<KeyCompare const &, T const &, T const &>);

    constexpr Sgtree() noexcept {
        Algorithms::init_header(&m_header);
    }

    constexpr explicit Sgtree(KeyCompare compare) noexcept : m_compare(std::move(compare)) {
        Algorithms::init_header(&m_header);
    }

    constexpr Sgtree(Sgtree const &) = delete;
    constexpr auto operator=(Sgtree const &) -> Sgtree & = delete;
    constexpr ~Sgtree() noexcept {
        clear();
    }

    [[nodiscard]]
    constexpr auto begin() noexcept -> IterMut {
        return IterMut(Algorithms::begin_node(&m_header));
    }
    [[nodiscard]]
    constexpr auto begin() const noexcept -> Iter {
        return Iter(Algorithms::begin_node(&m_header));
    }
    [[nodiscard]]
    constexpr auto end() noexcept -> IterMut {
        return IterMut(Algorithms::end_node(&m_header));
    }
    [[nodiscard]]
    constexpr auto end() const noexcept -> Iter {
        return Iter(Algorithms::end_node(&m_header));
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
    constexpr auto balance_factor() const noexcept -> float {
        return m_alpha;
    }

    constexpr auto balance_factor(float const alpha) noexcept -> void
        requires(FLOATING_POINT)
    {
        if (alpha <= 0.5F || alpha >= 1.0F) {
            return;
        }
        const auto old_alpha = m_alpha;
        m_alpha = alpha;
        if (alpha < old_alpha) {
            m_max_tree_size = m_size;
            Algorithms::rebalance(&m_header, m_size);
        }
    }

    [[nodiscard]]
    constexpr auto insert_unique(T &value) noexcept -> std::pair<IterMut, bool> {
        InsertCommitData commit_data{};
        const auto [existing, inserted] = Algorithms::insert_unique_check(
            &m_header, to_node(value), NodeCompare{&m_compare}, commit_data);
        if (!inserted) {
            return {IterMut(existing), false};
        }
        Algorithms::insert_unique_commit(
            &m_header, to_node(value), commit_data, m_size,
            [this](std::size_t const size) noexcept { return height_limit(size); },
            m_max_tree_size);
        ++m_size;
        return {IterMut(to_node(value)), true};
    }

    [[nodiscard]]
    constexpr auto insert_equal(T &value) noexcept -> IterMut {
        const auto node = to_node(value);
        Algorithms::insert_equal_upper_bound(
            &m_header, node, NodeCompare{&m_compare}, m_size,
            [this](std::size_t const size) noexcept { return height_limit(size); },
            m_max_tree_size);
        ++m_size;
        return IterMut(node);
    }

    constexpr auto erase(T &value) noexcept -> void {
        const auto node = to_node(value);
        if (!Algorithms::unique(node)) {
            static_cast<void>(Algorithms::erase(&m_header, node, m_size, m_max_tree_size,
                                                [this](std::size_t const maximum_size) noexcept {
                                                    return deletion_threshold(maximum_size);
                                                }));
            --m_size;
        }
    }

    constexpr auto clear() noexcept -> void {
        while (!empty()) {
            erase(*begin());
        }
        m_max_tree_size = 0U;
    }

    [[nodiscard]]
    constexpr auto find(T const &value) noexcept -> ustl::Option<T &> {
        const auto node = Algorithms::find(&m_header, value, NodeCompare{&m_compare});
        return node == Algorithms::end_node(&m_header) ? ustl::NONE : ustl::some_ref(*to_value(node));
    }

    [[nodiscard]]
    constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
        const auto node = Algorithms::find(&m_header, value, NodeCompare{&m_compare});
        return node == Algorithms::end_node(&m_header) ? ustl::NONE : ustl::some_ref(*to_value(node));
    }

    [[nodiscard]]
    constexpr auto count(T const &value) const noexcept -> Size {
        return Algorithms::count(&m_header, value, NodeCompare{&m_compare});
    }

  private:
    [[nodiscard]]
    constexpr auto height_limit(std::size_t const tree_size) const noexcept -> std::size_t {
        if constexpr (FLOATING_POINT) {
            return static_cast<std::size_t>(std::log(static_cast<double>(tree_size)) /
                                            -std::log(static_cast<double>(m_alpha)));
        } else {
            std::size_t result{};
            auto remaining = tree_size;
            while (remaining > 1U) {
                remaining >>= 1U;
                result += 2U;
            }
            return result;
        }
    }

    [[nodiscard]]
    constexpr auto deletion_threshold(std::size_t const maximum_size) const noexcept
        -> std::size_t {
        if constexpr (FLOATING_POINT) {
            return static_cast<std::size_t>(static_cast<double>(maximum_size) *
                                            static_cast<double>(m_alpha));
        } else {
            return maximum_size / 4U * 3U;
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

    Node m_header{};
    KeyCompare m_compare{};
    float m_alpha{0.70710677F};
    Size m_max_tree_size{};
    Size m_size{};
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_SGTREE_HPP
