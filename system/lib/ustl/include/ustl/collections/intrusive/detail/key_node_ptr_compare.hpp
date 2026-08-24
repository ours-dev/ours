/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_KEY_NODE_PTR_COMPARE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_KEY_NODE_PTR_COMPARE_HPP 1

#include <concepts>
#include <functional>
#include <type_traits>

namespace ustl::collections::intrusive::detail {
    // C++23 port of Boost.Intrusive's detail::KeyNodeptrComp.
    template <typename KeyCompare, typename ValueAccess, typename KeyOfElement = std::identity>
    class KeyNodePtrCompare final {
        using NodePtr = typename ValueAccess::NodePtr;
        using ConstNodePtr = typename ValueAccess::ConstNodePtr;

        template <typename Candidate>
        static constexpr bool IS_NODE_PTR = std::same_as<std::remove_cvref_t<Candidate>, NodePtr>
                                         || std::same_as<std::remove_cvref_t<Candidate>, ConstNodePtr>
                                         || std::convertible_to<std::remove_cvref_t<Candidate>, ConstNodePtr>;

    public:
        constexpr explicit KeyNodePtrCompare(KeyCompare const *const compare) noexcept
            : m_compare(compare) {}

        template <typename Candidate>
            requires(IS_NODE_PTR<Candidate>)
        [[nodiscard]] constexpr auto operator()(Candidate const &candidate) const noexcept -> bool {
            return (*m_compare)(key(candidate));
        }

        template <typename Left, typename Right>
        [[nodiscard]] constexpr auto operator()(Left const &left, Right const &right) const noexcept -> bool {
            return (*m_compare)(key(left), key(right));
        }

    private:
        template <typename Candidate>
        [[nodiscard]] static constexpr decltype(auto) key(Candidate const &candidate) noexcept {
            if constexpr (IS_NODE_PTR<Candidate>) {
                return std::invoke(KeyOfElement {}, *ValueAccess::to_value(candidate));
            } else {
                return (candidate);
            }
        }

        KeyCompare const *m_compare {};
    };
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_KEY_NODE_PTR_COMPARE_HPP
