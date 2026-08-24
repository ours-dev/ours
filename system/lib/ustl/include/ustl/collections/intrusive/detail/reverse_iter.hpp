/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_REVERSE_ITER_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_REVERSE_ITER_HPP 1

#include <concepts>
#include <utility>

namespace ustl::collections::intrusive::detail {
    template <typename TIter>
    class ReverseIter final {
    public:
        constexpr ReverseIter() noexcept = default;

        constexpr explicit ReverseIter(TIter base) noexcept
            : m_base(std::move(base)) {}

        template <typename TOtherIter>
            requires(std::convertible_to<TOtherIter, TIter>)
        constexpr ReverseIter(ReverseIter<TOtherIter> const &other) noexcept
            : m_base(other.base()) {}

        [[nodiscard]] constexpr auto base() const noexcept -> TIter {
            return m_base;
        }

        [[nodiscard]] constexpr auto operator*() const noexcept -> decltype(*std::declval<TIter const &>()) {
            auto current = m_base;
            return *--current;
        }

        [[nodiscard]] constexpr auto operator->() const noexcept -> decltype(std::declval<TIter const &>().operator->()) {
            auto current = m_base;
            --current;
            return current.operator->();
        }

        constexpr auto operator++() noexcept -> ReverseIter & {
            --m_base;
            return *this;
        }

        constexpr auto operator++(int) noexcept -> ReverseIter {
            const auto result = *this;
            ++(*this);
            return result;
        }

        constexpr auto operator--() noexcept -> ReverseIter & {
            ++m_base;
            return *this;
        }

        constexpr auto operator--(int) noexcept -> ReverseIter {
            const auto result = *this;
            --(*this);
            return result;
        }

    private:
        TIter m_base {};
    };

    template <typename TLeftIter, typename TRightIter>
    [[nodiscard]] constexpr auto operator==(ReverseIter<TLeftIter> const &left, ReverseIter<TRightIter> const &right) noexcept
        -> bool {
        return left.base() == right.base();
    }
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_REVERSE_ITER_HPP
