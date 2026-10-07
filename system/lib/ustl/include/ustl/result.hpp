/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_RESULT_HPP
#define USTL_RESULT_HPP 1

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace ustl {
template <typename T>
struct OkValue final {
    T m_value;
};

template <>
struct OkValue<void> final {};

template <typename E>
struct ErrValue final {
    E m_error;
};

template <typename T>
[[nodiscard]]
constexpr auto ok(T &&value) -> OkValue<std::remove_cvref_t<T>> {
    return {.m_value = std::forward<T>(value)};
}

[[nodiscard]]
constexpr auto ok() -> OkValue<void> {
    return {};
}

template <typename E>
[[nodiscard]]
constexpr auto err(E &&error) -> ErrValue<std::remove_cvref_t<E>> {
    return {.m_error = std::forward<E>(error)};
}

template <typename T, typename E>
class [[nodiscard]] Result final {
    static_assert(!std::is_void_v<T>, "Use Result<void, E> for an empty success value.");
    static_assert(!std::is_void_v<E>, "Result error type must not be void.");
    static_assert(!std::is_reference_v<T>, "Result value type must not be a reference.");
    static_assert(!std::is_reference_v<E>, "Result error type must not be a reference.");

  public:
    using Value = T;
    using Error = E;
    using value_type = T;
    using error_type = E;

    template <typename U>
        requires std::is_constructible_v<T, U const &>
    constexpr Result(OkValue<U> const &value) : m_is_ok(true) {
        std::construct_at(std::addressof(m_storage.m_value), value.m_value);
    }

    template <typename U>
        requires std::is_constructible_v<T, U &&>
    constexpr Result(OkValue<U> &&value) : m_is_ok(true) {
        std::construct_at(std::addressof(m_storage.m_value), std::move(value.m_value));
    }

    template <typename F>
        requires std::is_constructible_v<E, F const &>
    constexpr Result(ErrValue<F> const &error) : m_is_ok(false) {
        std::construct_at(std::addressof(m_storage.m_error), error.m_error);
    }

    template <typename F>
        requires std::is_constructible_v<E, F &&>
    constexpr Result(ErrValue<F> &&error) : m_is_ok(false) {
        std::construct_at(std::addressof(m_storage.m_error), std::move(error.m_error));
    }

    constexpr Result(Result const &other)
        requires(std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
        : m_is_ok(other.m_is_ok) {
        if (m_is_ok) {
            std::construct_at(std::addressof(m_storage.m_value), other.m_storage.m_value);
        } else {
            std::construct_at(std::addressof(m_storage.m_error), other.m_storage.m_error);
        }
    }

    constexpr Result(Result const &)
        requires(!std::is_copy_constructible_v<T> || !std::is_copy_constructible_v<E>)
    = delete;

    constexpr Result(Result &&other)
        requires(std::is_move_constructible_v<T> && std::is_move_constructible_v<E>)
        : m_is_ok(other.m_is_ok) {
        if (m_is_ok) {
            std::construct_at(std::addressof(m_storage.m_value),
                              std::move(other.m_storage.m_value));
        } else {
            std::construct_at(std::addressof(m_storage.m_error),
                              std::move(other.m_storage.m_error));
        }
    }

    constexpr Result(Result &&)
        requires(!std::is_move_constructible_v<T> || !std::is_move_constructible_v<E>)
    = delete;

    constexpr ~Result() {
        destroy_active();
    }

    constexpr auto operator=(Result const &other) -> Result &
        requires(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T> &&
                 std::is_copy_constructible_v<E> && std::is_copy_assignable_v<E>)
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        assign_from(other);
        return *this;
    }

    constexpr auto operator=(Result const &) -> Result &
        requires(!std::is_copy_constructible_v<T> || !std::is_copy_assignable_v<T> ||
                 !std::is_copy_constructible_v<E> || !std::is_copy_assignable_v<E>)
    = delete;

    constexpr auto operator=(Result &&other) -> Result &
        requires(std::is_move_constructible_v<T> && std::is_move_assignable_v<T> &&
                 std::is_move_constructible_v<E> && std::is_move_assignable_v<E>)
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        assign_from(std::move(other));
        return *this;
    }

    constexpr auto operator=(Result &&) -> Result &
        requires(!std::is_move_constructible_v<T> || !std::is_move_assignable_v<T> ||
                 !std::is_move_constructible_v<E> || !std::is_move_assignable_v<E>)
    = delete;

    [[nodiscard]]
    constexpr auto is_ok() const -> bool {
        return m_is_ok;
    }

    [[nodiscard]]
    constexpr auto is_err() const -> bool {
        return !m_is_ok;
    }

    [[nodiscard]]
    constexpr auto has_value() const -> bool {
        return is_ok();
    }

    [[nodiscard]]
    constexpr explicit operator bool() const {
        return is_ok();
    }

    [[nodiscard]]
    constexpr auto operator->() -> T * {
        return std::addressof(value());
    }

    [[nodiscard]]
    constexpr auto operator->() const -> T const * {
        return std::addressof(value());
    }

    [[nodiscard]]
    constexpr auto operator*() & -> T & {
        return value();
    }

    [[nodiscard]]
    constexpr auto operator*() const & -> T const & {
        return value();
    }

    [[nodiscard]]
    constexpr auto operator*() && -> T {
        return std::move(*this).value();
    }

    [[nodiscard]]
    constexpr auto value() & -> T & {
        require_ok();
        return m_storage.m_value;
    }

    [[nodiscard]]
    constexpr auto value() const & -> T const & {
        require_ok();
        return m_storage.m_value;
    }

    [[nodiscard]]
    constexpr auto value() && -> T {
        require_ok();
        return std::move(m_storage.m_value);
    }

    [[nodiscard]]
    constexpr auto error() & -> E & {
        require_err();
        return m_storage.m_error;
    }

    [[nodiscard]]
    constexpr auto error() const & -> E const & {
        require_err();
        return m_storage.m_error;
    }

    [[nodiscard]]
    constexpr auto error() && -> E {
        require_err();
        return std::move(m_storage.m_error);
    }

    [[nodiscard]]
    constexpr auto unwrap() & -> T & {
        return value();
    }

    [[nodiscard]]
    constexpr auto unwrap() const & -> T const & {
        return value();
    }

    [[nodiscard]]
    constexpr auto unwrap() && -> T {
        return std::move(*this).value();
    }

    [[nodiscard]]
    constexpr auto unwrap_err() & -> E & {
        return error();
    }

    [[nodiscard]]
    constexpr auto unwrap_err() const & -> E const & {
        return error();
    }

    [[nodiscard]]
    constexpr auto unwrap_err() && -> E {
        return std::move(*this).error();
    }

    template <typename U>
        requires std::is_copy_constructible_v<T> && std::is_convertible_v<U &&, T>
    [[nodiscard]]
    constexpr auto value_or(U &&fallback) const & -> T {
        if (is_ok()) {
            return m_storage.m_value;
        }
        return static_cast<T>(std::forward<U>(fallback));
    }

    template <typename U>
        requires std::is_move_constructible_v<T> && std::is_convertible_v<U &&, T>
    [[nodiscard]]
    constexpr auto value_or(U &&fallback) && -> T {
        if (is_ok()) {
            return std::move(m_storage.m_value);
        }
        return static_cast<T>(std::forward<U>(fallback));
    }

  private:
    union Storage {
        constexpr Storage() : m_dummy() {}

        constexpr ~Storage() {}

        char m_dummy;
        T m_value;
        E m_error;
    };

    constexpr auto destroy_active() -> void {
        if (m_is_ok) {
            std::destroy_at(std::addressof(m_storage.m_value));
        } else {
            std::destroy_at(std::addressof(m_storage.m_error));
        }
    }

    constexpr auto require_ok() const -> void {
        if (is_err()) {
            __builtin_trap();
        }
    }

    constexpr auto require_err() const -> void {
        if (is_ok()) {
            __builtin_trap();
        }
    }

    constexpr auto assign_from(Result const &other) -> void {
        if (is_ok() && other.is_ok()) {
            m_storage.m_value = other.m_storage.m_value;
        } else if (is_err() && other.is_err()) {
            m_storage.m_error = other.m_storage.m_error;
        } else if (other.is_ok()) {
            std::destroy_at(std::addressof(m_storage.m_error));
            std::construct_at(std::addressof(m_storage.m_value), other.m_storage.m_value);
            m_is_ok = true;
        } else {
            std::destroy_at(std::addressof(m_storage.m_value));
            std::construct_at(std::addressof(m_storage.m_error), other.m_storage.m_error);
            m_is_ok = false;
        }
    }

    constexpr auto assign_from(Result &&other) -> void {
        if (is_ok() && other.is_ok()) {
            m_storage.m_value = std::move(other.m_storage.m_value);
        } else if (is_err() && other.is_err()) {
            m_storage.m_error = std::move(other.m_storage.m_error);
        } else if (other.is_ok()) {
            std::destroy_at(std::addressof(m_storage.m_error));
            std::construct_at(std::addressof(m_storage.m_value),
                              std::move(other.m_storage.m_value));
            m_is_ok = true;
        } else {
            std::destroy_at(std::addressof(m_storage.m_value));
            std::construct_at(std::addressof(m_storage.m_error),
                              std::move(other.m_storage.m_error));
            m_is_ok = false;
        }
    }

    Storage m_storage;
    /// Whether m_storage holds the value or the error: the discriminant that
    /// is this type's whole representation, so it stays a bool of its own.
    bool m_is_ok;
};

template <typename E>
class [[nodiscard]]
Result<void, E>
    final {
    static_assert(!std::is_void_v<E>, "Result error type must not be void.");
    static_assert(!std::is_reference_v<E>, "Result error type must not be a reference.");

  public:
    using Value = void;
    using Error = E;
    using value_type = void;
    using error_type = E;

    constexpr Result(OkValue<void>) : m_is_ok(true) {
        std::construct_at(std::addressof(m_storage.m_dummy));
    }

    template <typename F>
        requires std::is_constructible_v<E, F const &>
    constexpr Result(ErrValue<F> const &error) : m_is_ok(false) {
        std::construct_at(std::addressof(m_storage.m_error), error.m_error);
    }

    template <typename F>
        requires std::is_constructible_v<E, F &&>
    constexpr Result(ErrValue<F> &&error) : m_is_ok(false) {
        std::construct_at(std::addressof(m_storage.m_error), std::move(error.m_error));
    }

    constexpr Result(Result const &other)
        requires std::is_copy_constructible_v<E>
        : m_is_ok(other.m_is_ok) {
        if (is_err()) {
            std::construct_at(std::addressof(m_storage.m_error), other.m_storage.m_error);
        } else {
            std::construct_at(std::addressof(m_storage.m_dummy));
        }
    }

    constexpr Result(Result const &)
        requires(!std::is_copy_constructible_v<E>)
    = delete;

    constexpr Result(Result &&other)
        requires std::is_move_constructible_v<E>
        : m_is_ok(other.m_is_ok) {
        if (is_err()) {
            std::construct_at(std::addressof(m_storage.m_error),
                              std::move(other.m_storage.m_error));
        } else {
            std::construct_at(std::addressof(m_storage.m_dummy));
        }
    }

    constexpr Result(Result &&)
        requires(!std::is_move_constructible_v<E>)
    = delete;

    constexpr ~Result() {
        destroy_active();
    }

    constexpr auto operator=(Result const &other) -> Result &
        requires(std::is_copy_constructible_v<E> && std::is_copy_assignable_v<E>)
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        assign_from(other);
        return *this;
    }

    constexpr auto operator=(Result const &) -> Result &
        requires(!std::is_copy_constructible_v<E> || !std::is_copy_assignable_v<E>)
    = delete;

    constexpr auto operator=(Result &&other) -> Result &
        requires(std::is_move_constructible_v<E> && std::is_move_assignable_v<E>)
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        assign_from(std::move(other));
        return *this;
    }

    constexpr auto operator=(Result &&) -> Result &
        requires(!std::is_move_constructible_v<E> || !std::is_move_assignable_v<E>)
    = delete;

    [[nodiscard]]
    constexpr auto is_ok() const -> bool {
        return m_is_ok;
    }

    [[nodiscard]]
    constexpr auto is_err() const -> bool {
        return !m_is_ok;
    }

    [[nodiscard]]
    constexpr auto has_value() const -> bool {
        return is_ok();
    }

    [[nodiscard]]
    constexpr explicit operator bool() const {
        return is_ok();
    }

    constexpr auto value() const -> void {
        require_ok();
    }

    constexpr auto unwrap() const -> void {
        value();
    }

    [[nodiscard]]
    constexpr auto error() & -> E & {
        require_err();
        return m_storage.m_error;
    }

    [[nodiscard]]
    constexpr auto error() const & -> E const & {
        require_err();
        return m_storage.m_error;
    }

    [[nodiscard]]
    constexpr auto error() && -> E {
        require_err();
        return std::move(m_storage.m_error);
    }

    [[nodiscard]]
    constexpr auto unwrap_err() & -> E & {
        return error();
    }

    [[nodiscard]]
    constexpr auto unwrap_err() const & -> E const & {
        return error();
    }

    [[nodiscard]]
    constexpr auto unwrap_err() && -> E {
        return std::move(*this).error();
    }

  private:
    union Storage {
        constexpr Storage() : m_dummy() {}

        constexpr ~Storage() {}

        char m_dummy;
        E m_error;
    };

    constexpr auto destroy_active() -> void {
        if (is_err()) {
            std::destroy_at(std::addressof(m_storage.m_error));
        }
    }

    constexpr auto require_ok() const -> void {
        if (is_err()) {
            __builtin_trap();
        }
    }

    constexpr auto require_err() const -> void {
        if (is_ok()) {
            __builtin_trap();
        }
    }

    constexpr auto assign_from(Result const &other) -> void {
        if (is_err() && other.is_err()) {
            m_storage.m_error = other.m_storage.m_error;
        } else if (other.is_ok()) {
            destroy_active();
            std::construct_at(std::addressof(m_storage.m_dummy));
            m_is_ok = true;
        } else {
            std::construct_at(std::addressof(m_storage.m_error), other.m_storage.m_error);
            m_is_ok = false;
        }
    }

    constexpr auto assign_from(Result &&other) -> void {
        if (is_err() && other.is_err()) {
            m_storage.m_error = std::move(other.m_storage.m_error);
        } else if (other.is_ok()) {
            destroy_active();
            std::construct_at(std::addressof(m_storage.m_dummy));
            m_is_ok = true;
        } else {
            std::construct_at(std::addressof(m_storage.m_error),
                              std::move(other.m_storage.m_error));
            m_is_ok = false;
        }
    }

    Storage m_storage;
    /// Whether m_storage holds the value or the error: the discriminant that
    /// is this type's whole representation, so it stays a bool of its own.
    bool m_is_ok;
};

} // namespace ustl

#endif // USTL_RESULT_HPP
