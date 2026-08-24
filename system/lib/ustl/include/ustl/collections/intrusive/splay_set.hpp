/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SPLAY_SET_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SPLAY_SET_HPP 1

#include <utility>

#include <ustl/option.hpp>
#include <ustl/collections/intrusive/splay_set_hook.hpp>
#include <ustl/collections/intrusive/splaytree.hpp>

namespace ustl::collections::intrusive {
    template <typename T, typename... Options>
    class SplaySet final {
        using Tree = Splaytree<T, Options...>;

    public:
        using Element = typename Tree::Element;
        using Size = typename Tree::Size;
        using KeyCompare = typename Tree::KeyCompare;
        using IterMut = typename Tree::IterMut;
        constexpr SplaySet() noexcept = default;
        constexpr explicit SplaySet(KeyCompare compare) noexcept
            : m_tree(std::move(compare)) {}
        [[nodiscard]] constexpr auto begin() noexcept -> IterMut { return m_tree.begin(); }
        [[nodiscard]] constexpr auto end() noexcept -> IterMut { return m_tree.end(); }
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return m_tree.empty(); }
        [[nodiscard]] constexpr auto size() const noexcept -> Size { return m_tree.size(); }
        [[nodiscard]] constexpr auto insert(T &value) noexcept -> std::pair<IterMut, bool> { return m_tree.insert_unique(value); }
        constexpr auto erase(T &value) noexcept -> void { m_tree.erase(value); }
        constexpr auto clear() noexcept -> void { m_tree.clear(); }
        [[nodiscard]] constexpr auto find(T const &value) noexcept -> ustl::Option<T &> { return m_tree.find(value); }
        [[nodiscard]] constexpr auto count(T const &value) const noexcept -> Size { return m_tree.count(value); }

    private:
        Tree m_tree {};
    };

    template <typename T, typename... Options>
    class SplayMultiset final {
        using Tree = Splaytree<T, Options...>;

    public:
        using Element = typename Tree::Element;
        using Size = typename Tree::Size;
        using KeyCompare = typename Tree::KeyCompare;
        using IterMut = typename Tree::IterMut;
        constexpr SplayMultiset() noexcept = default;
        constexpr explicit SplayMultiset(KeyCompare compare) noexcept
            : m_tree(std::move(compare)) {}
        [[nodiscard]] constexpr auto begin() noexcept -> IterMut { return m_tree.begin(); }
        [[nodiscard]] constexpr auto end() noexcept -> IterMut { return m_tree.end(); }
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return m_tree.empty(); }
        [[nodiscard]] constexpr auto size() const noexcept -> Size { return m_tree.size(); }
        [[nodiscard]] constexpr auto insert(T &value) noexcept -> IterMut { return m_tree.insert_equal(value); }
        constexpr auto erase(T &value) noexcept -> void { m_tree.erase(value); }
        constexpr auto clear() noexcept -> void { m_tree.clear(); }
        [[nodiscard]] constexpr auto find(T const &value) noexcept -> ustl::Option<T &> { return m_tree.find(value); }
        [[nodiscard]] constexpr auto count(T const &value) const noexcept -> Size { return m_tree.count(value); }

    private:
        Tree m_tree {};
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_SPLAY_SET_HPP
