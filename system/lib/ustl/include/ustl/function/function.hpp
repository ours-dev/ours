/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_FUNCTION_FUNCTION_HPP
#define USTL_FUNCTION_FUNCTION_HPP 1

#include <array>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <cstddef>

namespace ustl::function {

namespace detail {
    /// What a target of type T must satisfy to be stored in |Size| bytes aligned
    /// to |Align|. A concept, not a static_assert and not a bool variable, for two
    /// reasons: "does not fit" then takes part in overload resolution --
    /// std::is_constructible_v says false, which the tests rely on -- and clang
    /// breaks a failed concept down to the clause that failed, so an oversized
    /// lambda is reported as `sizeof(T) <= Size` evaluating to false.
    ///
    /// Nothrow move construction is required because moving a Function moves its
    /// target, and Function's own move is noexcept.
    template <typename T, std::size_t Size, std::size_t Align>
    concept FitsInline =
        sizeof(T) <= Size && alignof(T) <= Align && std::is_nothrow_move_constructible_v<T>;
} // namespace detail

template <typename Signature>
class Function;

/// A type-erased callable that never allocates: the target lives inside the
/// Function itself, in INLINE_SIZE bytes, and a target that does not fit is a
/// compile error rather than a silent trip to the heap.
///
/// Why not std::function (which ustl::function::Fn still aliases): it moves a
/// target larger than its small buffer to the heap, behind the caller's back,
/// and the heap is exactly what code running under a spin lock or with
/// interrupts off may not touch. Here the size is part of the contract. A
/// capture list that grows past the limit stops compiling at the line that
/// grew it, and the fix -- capture a pointer to a struct instead of the
/// struct -- is the caller's to make on purpose.
///
/// Why 24 bytes: three pointers. That covers the shapes that actually get
/// passed around -- a function pointer, a lambda capturing `this` plus a
/// buffer and a length -- and keeps the whole object at 32 bytes, half a
/// cache line, cheap to pass by value.
///
/// Move-only, like Zircon's fit::function and C++23's std::move_only_function
/// and unlike std::function: copying would demand that every target be
/// copyable, and a callable owning a resource is not. Moving relocates the
/// target into the destination's buffer, so the source is left empty.
///
/// Calling goes through one indirect call to a per-target-type thunk. The
/// empty state is not a null pointer but a table whose thunk traps, so the
/// call path has no branch and calling an empty Function stops at once
/// instead of jumping through null.
///
/// Zircon counterpart: fit::inline_function<Signature, inline_target_size>
/// (sdk/lib/fit/include/lib/fit/function.h), which also static_asserts that
/// the target fits. Here the size is fixed, not a parameter, so every
/// Function<R(Args...)> is the same type whatever it holds.
template <typename R, typename... Args>
class Function<R(Args...)> final {
  public:
    /// The most bytes a target may occupy, and the strictest alignment it may
    /// ask for.
    constexpr static std::size_t INLINE_SIZE = 24;
    constexpr static std::size_t INLINE_ALIGN = alignof(void *);

  private:
    /// Everything but the size, spelled once for both places a callable is
    /// taken. The size is detail::FitsInline, kept a separate concept so the
    /// diagnostic says which of the two was not met.
    template <typename F>
    constexpr static bool INVOCABLE = !std::is_same_v<std::remove_cvref_t<F>, Function> &&
                                      std::is_invocable_r_v<R, std::decay_t<F> &, Args...>;

  public:
    constexpr Function() noexcept = default;

    constexpr Function(std::nullptr_t) noexcept {}

    /// Stores |callable| in place. Implicit on purpose, as std::function's is:
    /// a lambda passed where a Function parameter is expected should convert
    /// without the caller naming the type.
    ///
    /// A null function pointer or member pointer gives an empty Function, as
    /// it does for std::function, instead of a Function that jumps to zero.
    template <typename F>
        requires INVOCABLE<F> && detail::FitsInline<std::decay_t<F>, INLINE_SIZE, INLINE_ALIGN>
    Function(F &&callable) noexcept {
        using Target = std::decay_t<F>;
        if constexpr (std::is_pointer_v<Target> || std::is_member_pointer_v<Target>) {
            if (callable == nullptr) {
                return;
            }
        }
        std::construct_at(static_cast<Target *>(storage()), std::forward<F>(callable));
        m_ops = &OPS<Target>;
    }

    Function(Function &&other) noexcept {
        take(other);
    }

    auto operator=(Function &&other) noexcept -> Function & {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    auto operator=(std::nullptr_t) noexcept -> Function & {
        reset();
        return *this;
    }

    /// Replaces the target. Built as a temporary Function first so that the
    /// constraint and the null-pointer rule stay in one place.
    template <typename F>
        requires INVOCABLE<F> && detail::FitsInline<std::decay_t<F>, INLINE_SIZE, INLINE_ALIGN>
    auto operator=(F &&callable) noexcept -> Function & {
        return *this = Function(std::forward<F>(callable));
    }

    Function(Function const &) = delete;
    auto operator=(Function const &) -> Function & = delete;

    ~Function() {
        m_ops->destroy(storage());
    }

    /// Invokes the target. const as std::function's is, and for the same
    /// reason: a Function held by const reference is still callable, and the
    /// target -- a mutable lambda, say -- is invoked as the non-const object
    /// it is. The constness guards the Function's identity (which target it
    /// holds), not the target's state; hence `mutable` on the storage.
    auto operator()(Args... args) const -> R {
        return m_ops->invoke(storage(), std::forward<Args>(args)...);
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return m_ops != &EMPTY_OPS;
    }

    friend auto operator==(Function const &function, std::nullptr_t) noexcept -> bool {
        return !function;
    }

  private:
    /// One table per target type, built at compile time: how to call it, move
    /// it into another buffer, and destroy it. A pointer to this is the only
    /// thing that records which type the storage holds.
    struct Ops {
        auto (*invoke)(void *target, Args &&...args) -> R;

        /// Move-constructs the target at |dst| from |src|, then destroys |src|.
        auto (*relocate)(void *dst, void *src) noexcept -> void;

        auto (*destroy)(void *target) noexcept -> void;
    };

    /// The storage holds a T constructed by std::construct_at; std::launder
    /// is what turns the storage's address back into a pointer to that object
    /// rather than to the bytes under it.
    template <typename T>
    static auto target(void *storage) noexcept -> T * {
        return std::launder(static_cast<T *>(storage));
    }

    template <typename T>
    static auto invoke_target(void *storage, Args &&...args) -> R {
        return std::invoke_r<R>(*target<T>(storage), std::forward<Args>(args)...);
    }

    template <typename T>
    static auto relocate_target(void *dst, void *src) noexcept -> void {
        std::construct_at(static_cast<T *>(dst), std::move(*target<T>(src)));
        std::destroy_at(target<T>(src));
    }

    template <typename T>
    static auto destroy_target(void *storage) noexcept -> void {
        std::destroy_at(target<T>(storage));
    }

    template <typename T>
    constexpr static Ops OPS = {
        .invoke = &invoke_target<T>,
        .relocate = &relocate_target<T>,
        .destroy = &destroy_target<T>,
    };

    /// The empty state. Relocating or destroying nothing is a no-op; invoking
    /// it traps, which is the whole of the empty check.
    [[noreturn]]
    static auto invoke_empty(void * /*storage*/, Args &&.../*args*/) -> R {
        __builtin_trap();
    }

    static auto relocate_empty(void * /*dst*/, void * /*src*/) noexcept -> void {}

    static auto destroy_empty(void * /*storage*/) noexcept -> void {}

    constexpr static Ops EMPTY_OPS = {
        .invoke = &invoke_empty,
        .relocate = &relocate_empty,
        .destroy = &destroy_empty,
    };

    /// Moves |other|'s target into this Function, which must be empty, and
    /// leaves |other| empty.
    auto take(Function &other) noexcept -> void {
        m_ops = other.m_ops;
        m_ops->relocate(storage(), other.storage());
        other.m_ops = &EMPTY_OPS;
    }

    auto reset() noexcept -> void {
        m_ops->destroy(storage());
        m_ops = &EMPTY_OPS;
    }

    auto storage() const noexcept -> void * {
        return m_storage.data();
    }

    Ops const *m_ops = &EMPTY_OPS;

    /// Left uninitialised: only a constructed target is ever read from it, and
    /// zeroing 24 bytes on every construction buys nothing.
    alignas(INLINE_ALIGN) mutable std::array<std::byte, INLINE_SIZE> m_storage;
};

} // namespace ustl::function

#endif // USTL_FUNCTION_FUNCTION_HPP
