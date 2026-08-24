/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_OPTION_HPP
#define USTL_OPTION_HPP 1

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ustl {
    struct NoneValue final {};

    inline constexpr NoneValue NONE {};

    constexpr auto none() noexcept -> NoneValue {
        return NONE;
    }

    template <typename T>
    class [[nodiscard]] Option final {
        static_assert(!std::is_void_v<T>, "Option value type must not be void.");
        static_assert(!std::is_reference_v<T>, "Use Option<T&> for references.");
        static_assert(std::is_nothrow_destructible_v<T>, "Option value type must be nothrow destructible.");

      public:
        constexpr Option() noexcept = default;

        constexpr Option(NoneValue) noexcept {}

        template <typename U>
            requires(!std::same_as<std::remove_cvref_t<U>, NoneValue>
                     && std::constructible_from<T, U &&> && std::is_nothrow_constructible_v<T, U &&>)
        constexpr explicit(!std::is_convertible_v<U &&, T>) Option(U &&value) noexcept
            : m_is_some(true) {
            std::construct_at(std::addressof(m_storage.m_value), std::forward<U>(value));
        }

        constexpr Option(Option const &other) noexcept
            requires std::is_nothrow_copy_constructible_v<T>
            : m_is_some(other.m_is_some) {
            if (m_is_some) {
                std::construct_at(std::addressof(m_storage.m_value), other.m_storage.m_value);
            }
        }

        constexpr Option(Option const &)
            requires(!std::is_nothrow_copy_constructible_v<T>)
            = delete;

        constexpr Option(Option &&other) noexcept
            requires std::is_nothrow_move_constructible_v<T>
            : m_is_some(other.m_is_some) {
            if (m_is_some) {
                std::construct_at(std::addressof(m_storage.m_value), std::move(other.m_storage.m_value));
            }
        }

        constexpr Option(Option &&)
            requires(!std::is_nothrow_move_constructible_v<T>)
            = delete;

        constexpr ~Option() noexcept {
            reset();
        }

        constexpr auto operator=(Option const &other) noexcept -> Option &
            requires(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T>) {
            if (this == std::addressof(other)) {
                return *this;
            }

            if (m_is_some && other.m_is_some) {
                m_storage.m_value = other.m_storage.m_value;
            } else if (other.m_is_some) {
                std::construct_at(std::addressof(m_storage.m_value), other.m_storage.m_value);
                m_is_some = true;
            } else {
                reset();
            }
            return *this;
        }

        constexpr auto operator=(Option const &) -> Option &
            requires(!std::is_nothrow_copy_constructible_v<T> || !std::is_nothrow_copy_assignable_v<T>)
            = delete;

        constexpr auto operator=(Option &&other) noexcept -> Option &
            requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) {
            if (this == std::addressof(other)) {
                return *this;
            }

            if (m_is_some && other.m_is_some) {
                m_storage.m_value = std::move(other.m_storage.m_value);
            } else if (other.m_is_some) {
                std::construct_at(std::addressof(m_storage.m_value), std::move(other.m_storage.m_value));
                m_is_some = true;
            } else {
                reset();
            }
            return *this;
        }

        constexpr auto operator=(Option &&) -> Option &
            requires(!std::is_nothrow_move_constructible_v<T> || !std::is_nothrow_move_assignable_v<T>)
            = delete;

        constexpr auto operator=(NoneValue) noexcept -> Option & {
            reset();
            return *this;
        }

        [[nodiscard]]
        constexpr auto is_some() const noexcept -> bool {
            return m_is_some;
        }

        [[nodiscard]]
        constexpr auto is_none() const noexcept -> bool {
            return !is_some();
        }

        [[nodiscard]]
        constexpr explicit operator bool() const noexcept {
            return is_some();
        }

        [[nodiscard]]
        constexpr auto operator->() noexcept -> T * {
            return std::addressof(value());
        }

        [[nodiscard]]
        constexpr auto operator->() const noexcept -> T const * {
            return std::addressof(value());
        }

        [[nodiscard]]
        constexpr auto operator*() & noexcept -> T & {
            return value();
        }

        [[nodiscard]]
        constexpr auto operator*() const & noexcept -> T const & {
            return value();
        }

        [[nodiscard]]
        constexpr auto operator*() && noexcept -> T
            requires std::is_nothrow_move_constructible_v<T> {
            return std::move(*this).value();
        }

        [[nodiscard]]
        constexpr auto value() & noexcept -> T & {
            require_some();
            return m_storage.m_value;
        }

        [[nodiscard]]
        constexpr auto value() const & noexcept -> T const & {
            require_some();
            return m_storage.m_value;
        }

        [[nodiscard]]
        constexpr auto value() && noexcept -> T
            requires std::is_nothrow_move_constructible_v<T> {
            require_some();
            return std::move(m_storage.m_value);
        }

        constexpr auto reset() noexcept -> void {
            if (m_is_some) {
                std::destroy_at(std::addressof(m_storage.m_value));
                m_is_some = false;
            }
        }

        [[nodiscard]]
        constexpr auto take() noexcept -> Option
            requires std::is_nothrow_move_constructible_v<T> {
            if (is_none()) {
                return NONE;
            }

            auto result = Option(std::move(m_storage.m_value));
            reset();
            return result;
        }

        template <typename F>
            requires std::invocable<F, T const &>
                     && std::is_nothrow_invocable_v<F, T const &>
                     && std::is_nothrow_constructible_v<std::remove_cvref_t<std::invoke_result_t<F, T const &>>,
                                                        std::invoke_result_t<F, T const &>>
        [[nodiscard]]
        constexpr auto map(F &&function) const & noexcept
            -> Option<std::remove_cvref_t<std::invoke_result_t<F, T const &>>> {
            using Mapped = std::remove_cvref_t<std::invoke_result_t<F, T const &>>;

            static_assert(std::is_object_v<Mapped>, "Option map result must be an object type.");
            if (is_none()) {
                return NONE;
            }
            return Option<Mapped>(std::invoke(std::forward<F>(function), m_storage.m_value));
        }

      private:
        union Storage {
            constexpr Storage() noexcept
                : m_dummy() {}

            constexpr ~Storage() {}

            unsigned char m_dummy;
            T m_value;
        };

        constexpr auto require_some() const noexcept -> void {
            if (is_none()) {
                __builtin_trap();
            }
        }

        Storage m_storage {};
        bool m_is_some {false};
    };

    template <typename T>
    class [[nodiscard]] Option<T &> final {
      public:
        constexpr Option() noexcept = default;

        constexpr Option(NoneValue) noexcept {}

        constexpr explicit Option(T &value) noexcept
            : m_value(std::addressof(value)) {}

        constexpr auto operator=(NoneValue) noexcept -> Option & {
            m_value = nullptr;
            return *this;
        }

        [[nodiscard]]
        constexpr auto is_some() const noexcept -> bool {
            return m_value != nullptr;
        }

        [[nodiscard]]
        constexpr auto is_none() const noexcept -> bool {
            return !is_some();
        }

        [[nodiscard]]
        constexpr explicit operator bool() const noexcept {
            return is_some();
        }

        [[nodiscard]]
        constexpr auto operator->() const noexcept -> T * {
            return std::addressof(value());
        }

        [[nodiscard]]
        constexpr auto operator*() const noexcept -> T & {
            return value();
        }

        [[nodiscard]]
        constexpr auto value() const noexcept -> T & {
            require_some();
            return *m_value;
        }

        constexpr auto reset() noexcept -> void {
            m_value = nullptr;
        }

        [[nodiscard]]
        constexpr auto take() noexcept -> Option {
            auto result = Option(m_value == nullptr ? NONE : Option(*m_value));
            reset();
            return result;
        }

        template <typename F>
            requires std::invocable<F, T &>
                     && std::is_nothrow_invocable_v<F, T &>
                     && std::is_nothrow_constructible_v<std::remove_cvref_t<std::invoke_result_t<F, T &>>,
                                                        std::invoke_result_t<F, T &>>
        [[nodiscard]]
        constexpr auto map(F &&function) const noexcept -> Option<std::remove_cvref_t<std::invoke_result_t<F, T &>>> {
            using Mapped = std::remove_cvref_t<std::invoke_result_t<F, T &>>;

            static_assert(std::is_object_v<Mapped>, "Option map result must be an object type.");
            if (is_none()) {
                return NONE;
            }
            return Option<Mapped>(std::invoke(std::forward<F>(function), *m_value));
        }

      private:
        constexpr auto require_some() const noexcept -> void {
            if (is_none()) {
                __builtin_trap();
            }
        }

        T *m_value {nullptr};
    };

    template <typename T>
    [[nodiscard]]
    constexpr auto some(T &value) noexcept -> Option<T &> {
        return Option<T &>(value);
    }

    template <typename T>
        requires(!std::is_lvalue_reference_v<T>)
    [[nodiscard]]
    constexpr auto some(T &&value) noexcept -> Option<std::remove_cvref_t<T>> {
        return Option<std::remove_cvref_t<T>>(std::forward<T>(value));
    }
} // namespace ustl

#endif // USTL_OPTION_HPP
