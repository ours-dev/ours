/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_BOX_HPP
#define USTL_BOX_HPP 1

#include <type_traits>
#include <utility>

#include <cstddef>

#include <ustl/detail/deallocate.hpp>

namespace ustl {

/// Unique-ownership smart pointer, modeled after Rust's `Box<T>`.
///
/// A `Box<T>` is the single owner of a heap-allocated `T`: it cannot be
/// copied (Rust's `Box` is not `Copy`), and the object is destroyed exactly
/// when the box is — destructor first, then storage reclaim. Reclaim goes
/// through the same contract as `RefCounted`: a nested `T::Disposer` if the
/// type defines one, otherwise `operator delete` (in the kernel this is
/// `ktl::kfree`). Types served from an object cache therefore work under
/// `Box` exactly as under `Rc` — define the `Disposer` and hand the storage
/// over with `from_raw`.
///
/// Differences from Rust's `Box`, deliberate:
///   - a default-constructed box is *empty* (Rust's box is never null). The
///     empty state behaves like `nullptr`: `operator bool` is false and every
///     accessor is a no-op or returns null. This mirrors `Rc<T>`'s shape so
///     members can be declared before their first owner exists.
///   - polymorphic ownership (`Box<Base>` holding a derived object) requires
///     `Base` to have a virtual destructor, as with any C++ pointer; Rust
///     gets this for free from the fat pointer.
///
/// `make()` allocates with the global `new`, so it pairs with the default
/// `operator delete` reclaim. A type with a nested `Disposer` must not be
/// created through `make()` — its storage comes from wherever the `Disposer`
/// returns it; adopt it with `from_raw()` instead.
template <typename T>
class Box final {
    template <typename>
    friend class Box;

  public:
    using Element = T;
    using Ptr = T const *;
    using PtrMut = T *;

    constexpr Box() noexcept = default;
    constexpr Box(std::nullptr_t) noexcept {}

    /// Allocate and construct the payload in place, forwarding the arguments
    /// to `T`'s constructor — Rust's `Box::new(v)` without the intermediate
    /// move of a complete object.
    template <typename... Args>
        requires(std::is_constructible_v<T, Args && ...>)
    [[nodiscard]]
    static auto make(Args &&...args) -> Box {
        return Box(new T(std::forward<Args>(args)...));
    }

    /// Adopt externally created storage (Rust's `Box::from_raw`). The box
    /// becomes the sole owner: it will run `~T()` and reclaim the storage
    /// when it dies. Reclaim honors `T::Disposer` when present, which is the
    /// supported path for object-cache-served objects.
    [[nodiscard]]
    static auto from_raw(T *pointer) noexcept -> Box {
        return Box(pointer);
    }

    /// `Rc<T>::adopt`'s counterpart for the unique-ownership world: take over
    /// an externally created object. Same operation as `from_raw` — unlike
    /// `Rc::adopt` there is no count to bump from 0 to 1; ownership simply
    /// changes hands. Prefer this spelling for vocabulary consistency with
    /// `Rc`/`Arc` code.
    [[nodiscard]]
    static auto adopt(T *pointer) noexcept -> Box {
        return from_raw(pointer);
    }

    explicit Box(T *pointer) noexcept : m_pointer(pointer) {}

    Box(Box const &) = delete;
    auto operator=(Box const &) -> Box & = delete;

    Box(Box &&other) noexcept : m_pointer(std::exchange(other.m_pointer, nullptr)) {}

    /// Upcast from `Box<U>` (U derived from T). Compile-time safe: only
    /// enabled when `U*` converts to `T*` without a downcast.
    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    Box(Box<U> &&other) noexcept : m_pointer(static_cast<T *>(other.m_pointer)) {
        other.m_pointer = nullptr;
    }

    ~Box() {
        reset();
    }

    auto operator=(Box &&other) noexcept -> Box & {
        if (this != &other) {
            reset();
            m_pointer = std::exchange(other.m_pointer, nullptr);
        }
        return *this;
    }

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    auto operator=(Box<U> &&other) noexcept -> Box & {
        if (m_pointer != other.m_pointer) {
            reset();
            m_pointer = static_cast<T *>(std::exchange(other.m_pointer, nullptr));
        }
        return *this;
    }

    auto operator=(std::nullptr_t) noexcept -> Box & {
        reset();
        return *this;
    }

    /// Destroy the owned object (if any) and become empty.
    auto reset() noexcept -> void {
        if (auto *pointer = std::exchange(m_pointer, nullptr)) {
            pointer->~T();
            detail::default_deallocate<T>(pointer);
        }
    }

    /// Destroy the owned object (if any) and adopt `pointer`.
    auto reset(T *pointer) noexcept -> void {
        if (pointer != m_pointer) {
            reset();
            m_pointer = pointer;
        }
    }

    auto swap(Box &other) noexcept -> void {
        std::swap(m_pointer, other.m_pointer);
    }
    friend auto swap(Box &left, Box &right) noexcept -> void {
        left.swap(right);
    }

    /// Transfer ownership to a raw pointer (Rust's `Box::into_raw`). The
    /// returned pointer must be re-wrapped with `from_raw` (or deliberately
    /// leaked); nothing else will destroy the object.
    [[nodiscard]]
    auto into_raw() noexcept -> T * {
        return std::exchange(m_pointer, nullptr);
    }

    /// Relinquish ownership and let the object outlive every box (Rust's
    /// `Box::leak`): the payload is never destroyed nor reclaimed. Returns a
    /// reference to the now-immortal object.
    [[nodiscard]]
    auto leak() noexcept -> T & {
        return *std::exchange(m_pointer, nullptr);
    }

    /// Deep copy (Rust's derived `Clone` for `Box<T: Clone>`). Only available
    /// when `T` is copy-constructible; an empty box clones to an empty box.
    [[nodiscard]]
    auto clone() const -> Box
        requires(std::is_copy_constructible_v<T>)
    {
        if (m_pointer == nullptr) {
            return {};
        }
        return make(*m_pointer);
    }

    [[nodiscard]]
    auto as_ptr() const noexcept -> T const * {
        return m_pointer;
    }
    [[nodiscard]]
    auto as_ptr_mut() noexcept -> T * {
        return m_pointer;
    }
    [[nodiscard]]
    auto as_ref() const noexcept -> T const & {
        return *m_pointer;
    }
    [[nodiscard]]
    auto as_ref_mut() noexcept -> T & {
        return *m_pointer;
    }

    [[nodiscard]]
    auto operator->() const noexcept -> T const * {
        return m_pointer;
    }
    [[nodiscard]]
    auto operator->() noexcept -> T * {
        return m_pointer;
    }
    [[nodiscard]]
    auto operator*() const noexcept -> T const & {
        return *m_pointer;
    }
    [[nodiscard]]
    auto operator*() noexcept -> T & {
        return *m_pointer;
    }

    explicit constexpr operator bool() const noexcept {
        return m_pointer != nullptr;
    }

    friend auto operator==(Box const &left, Box const &right) noexcept -> bool {
        return left.m_pointer == right.m_pointer;
    }
    friend auto operator==(Box const &left, std::nullptr_t) noexcept -> bool {
        return left.m_pointer == nullptr;
    }

  private:
    T *m_pointer = nullptr;
};

} // namespace ustl

#endif // USTL_BOX_HPP
