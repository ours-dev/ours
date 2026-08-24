/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_BS_SET_HPP
#define USTL_COLLECTIONS_INTRUSIVE_BS_SET_HPP 1

#include <utility>

#include <ustl/option.hpp>
#include <ustl/collections/intrusive/bs_set_hook.hpp>
#include <ustl/collections/intrusive/bstree.hpp>

namespace ustl::collections::intrusive {
    template <typename T, typename... Options>
    class BsSet final {
        using Tree = Bstree<T, Options...>;

    public:
        using Element = typename Tree::Element;
        using Size = typename Tree::Size;
        using KeyCompare = typename Tree::KeyCompare;
        using IterMut = typename Tree::IterMut;

        constexpr BsSet() noexcept = default;
        constexpr explicit BsSet(KeyCompare compare) noexcept
            : m_tree(std::move(compare)) {}

        constexpr BsSet(BsSet const &) = delete;
        constexpr auto operator=(BsSet const &) -> BsSet & = delete;

        [[nodiscard]] constexpr auto begin() noexcept -> IterMut { return m_tree.begin(); }
        [[nodiscard]] constexpr auto end() noexcept -> IterMut { return m_tree.end(); }
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return m_tree.empty(); }
        [[nodiscard]] constexpr auto size() const noexcept -> Size { return m_tree.size(); }
        [[nodiscard]] constexpr auto insert(T &value) noexcept -> std::pair<IterMut, bool> { return m_tree.insert_unique(value); }
        constexpr auto erase(T &value) noexcept -> void { m_tree.erase(value); }
        constexpr auto clear() noexcept -> void { m_tree.clear(); }
        [[nodiscard]] constexpr auto find(T const &value) noexcept -> ustl::Option<T &> { return m_tree.find(value); }
        [[nodiscard]] constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
            return m_tree.find(value);
        }
        [[nodiscard]] constexpr auto count(T const &value) const noexcept -> Size { return m_tree.count(value); }

    private:
        Tree m_tree {};
    };

    template <typename T, typename... Options>
    class BsMultiset final {
        using Tree = Bstree<T, Options...>;

    public:
        using Element = typename Tree::Element;
        using Size = typename Tree::Size;
        using KeyCompare = typename Tree::KeyCompare;
        using IterMut = typename Tree::IterMut;

        constexpr BsMultiset() noexcept = default;
        constexpr explicit BsMultiset(KeyCompare compare) noexcept
            : m_tree(std::move(compare)) {}

        constexpr BsMultiset(BsMultiset const &) = delete;
        constexpr auto operator=(BsMultiset const &) -> BsMultiset & = delete;

        [[nodiscard]] constexpr auto begin() noexcept -> IterMut { return m_tree.begin(); }
        [[nodiscard]] constexpr auto end() noexcept -> IterMut { return m_tree.end(); }
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return m_tree.empty(); }
        [[nodiscard]] constexpr auto size() const noexcept -> Size { return m_tree.size(); }
        [[nodiscard]] constexpr auto insert(T &value) noexcept -> IterMut { return m_tree.insert_equal(value); }
        constexpr auto erase(T &value) noexcept -> void { m_tree.erase(value); }
        constexpr auto clear() noexcept -> void { m_tree.clear(); }
        [[nodiscard]] constexpr auto find(T const &value) noexcept -> ustl::Option<T &> { return m_tree.find(value); }
        [[nodiscard]] constexpr auto find(T const &value) const noexcept -> ustl::Option<T const &> {
            return m_tree.find(value);
        }
        [[nodiscard]] constexpr auto count(T const &value) const noexcept -> Size { return m_tree.count(value); }

    private:
        Tree m_tree {};
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_BS_SET_HPP
