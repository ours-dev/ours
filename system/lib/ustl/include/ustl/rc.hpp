/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_RC_HPP
#define USTL_RC_HPP 1

#include <ustl/detail/deallocate.hpp>

#include <atomic>
#include <type_traits>
#include <utility>

#include <cstddef>
#include <cstdint>

namespace ustl {

namespace sync {
    /// Threading policy: reference counting uses `std::atomic` counters with
    /// release/acquire synchronization. Safe to share across CPUs.
    struct ThreadSafePolicy {};

    /// Threading policy: reference counting uses a plain non-atomic integer
    /// counter (no `<atomic>` operations at all). Must not be shared between
    /// threads; zero atomic overhead. This is the default (`Rc<T>`).
    struct SingleThreadedPolicy {};
} // namespace sync

namespace detail {
    /// Whether the policy requires atomic counting. `RefCounted` dispatches on
    /// this at compile time (`if constexpr`): the thread-safe policy counts
    /// with `std::atomic`, the single-threaded policy with a plain integer.
    template <typename Policy>
    struct IsThreadSafe: std::bool_constant<std::is_same_v<Policy, sync::ThreadSafePolicy>> {};

    /// Per-type runtime tag used by the checked `downcast`. A class opts in by
    /// defining `static constexpr std::uintptr_t TYPE_TAG` and overriding
    /// `RefCounted::rc_type_tag()` to return it.
    template <typename U, typename = void>
    struct RcTypeTag {
        static constexpr auto VALUE = std::uintptr_t{0};
    };
    template <typename U>
    struct RcTypeTag<U, std::void_t<decltype(U::TYPE_TAG)>> {
        static constexpr auto VALUE = U::TYPE_TAG;
    };
    template <typename U>
    concept HasRcTypeTag = RcTypeTag<U>::VALUE != 0;

    /// The threading policy of a `RefCounted`-derived type, or `void` when T is
    /// not ref-counted. Inherited by derived classes through the base alias.
    template <typename T, typename = void>
    struct ThreadPolicyOf {
        using Type = void;
    };
    template <typename T>
    struct ThreadPolicyOf<T, std::void_t<typename T::ThreadPolicy>> {
        using Type = typename T::ThreadPolicy;
    };
} // namespace detail

template <typename T, typename Policy>
class Weak;

template <typename T, typename Policy>
class Rc;

/// Intrusive reference counter embedded in the object.
///
/// The object carries BOTH a strong count (owners of type `Rc<T>`) and a weak
/// count (observers of type `Weak<T>`), so payload destruction and storage
/// reclamation are decoupled:
///
///   1. when the last strong reference is released, the payload is destroyed
///      (`~T()`, dispatched virtually to the most-derived type);
///   2. only when the last weak reference is also released is the storage
///      reclaimed through the deallocator (default: nested `T::Disposer` or
///      `operator delete`).
///
/// A `Weak<T>` therefore keeps the storage (and the counters) alive, which is
/// what makes `Weak::lock()` race-free: it atomically re-acquires a strong
/// reference iff the strong count is non-zero.
///
/// Counters are selected by the threading policy via compile-time tag dispatch
/// (`if constexpr` on `detail::IsThreadSafe<Policy>::value`): `ThreadSafePolicy`
/// counts with `std::atomic<std::size_t>`, `SingleThreadedPolicy` counts with
/// a plain `std::size_t` and performs no atomic operations at all. With
/// `ThreadSafePolicy`: increments use `std::memory_order_relaxed` (the caller
/// already holds a reference that keeps the object alive); decrements that may
/// reach zero use `acq_rel` (release the object's state, then acquire before
/// destruction or reclamation); `lock_strong()` uses an acquire CAS so a
/// successful upgrade sees the object's state published by the previous owner's
/// release. With `SingleThreadedPolicy` a single thread is naturally ordered.
///
/// Note: after the payload is destroyed, the counters and the deallocator are
/// accessed through the still-allocated storage. This is the standard
/// "reference counting with deferred reclamation" idiom: the counter fields are
/// trivially destructible and the storage is guaranteed to be alive by the
/// invariant `weak_count == 0 ⟹ no Rc and no Weak`.
template <typename T, typename Policy = sync::SingleThreadedPolicy>
class RefCounted {
    using Count = std::conditional_t<detail::IsThreadSafe<Policy>::value, std::atomic<std::size_t>,
                                     std::size_t>;

  public:
    using ThreadPolicy = Policy;

    RefCounted() noexcept = default;
    RefCounted(RefCounted const &) = delete;
    auto operator=(RefCounted const &) -> RefCounted & = delete;

    /// Take one strong reference. Every strong reference also holds one weak
    /// reference, so the storage stays alive as long as the object is owned.
    auto retain() noexcept -> void {
        strong_add();
        weak_add();
    }

    /// Drop one strong reference. The payload is destroyed when the count
    /// reaches zero; the storage is reclaimed only when the weak count also
    /// reaches zero (see `release_weak`).
    auto release() noexcept -> void {
        if (strong_sub()) {
            destroy();
            if (weak_sub()) {
                reclaim();
            }
        } else {
            static_cast<void>(weak_sub());
        }
    }

    auto retain_weak() noexcept -> void {
        weak_add();
    }

    /// Drop one weak reference. Reclaims the storage when both counts are zero.
    auto release_weak() noexcept -> void {
        if (weak_sub()) {
            reclaim();
        }
    }

    /// Upgrade a weak reference: acquire a strong reference if and only if the
    /// object is still alive (strong count > 0). The caller must hold a weak
    /// reference, which guarantees the storage stays alive during the
    /// operation. On success the caller owns one strong reference and must
    /// pair it with `release()`.
    [[nodiscard]]
    auto lock_strong() noexcept -> bool {
        if constexpr (detail::IsThreadSafe<Policy>::value) {
            auto expected = m_strong.load(std::memory_order_relaxed);
            for (;;) {
                if (expected == 0) {
                    return false;
                }
                if (m_strong.compare_exchange_weak(expected, expected + 1,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
                    break;
                }
            }
            weak_add();
            return true;
        } else {
            if (m_strong == 0) {
                return false;
            }
            ++m_strong;
            ++m_weak;
            return true;
        }
    }

    [[nodiscard]]
    auto strong_count() const noexcept -> std::size_t {
        if constexpr (detail::IsThreadSafe<Policy>::value) {
            return m_strong.load(std::memory_order_acquire);
        }
        return m_strong;
    }

    [[nodiscard]]
    auto weak_count() const noexcept -> std::size_t {
        if constexpr (detail::IsThreadSafe<Policy>::value) {
            return m_weak.load(std::memory_order_acquire);
        }
        return m_weak;
    }

    /// Runtime type tag used by the checked `downcast`. Returns 0 unless a
    /// class opts in by defining `static constexpr std::uintptr_t TYPE_TAG`
    /// and overriding this function to return it.
    [[nodiscard]]
    virtual auto rc_type_tag() const noexcept -> std::uintptr_t {
        return 0;
    }

  protected:
    /// Virtual destructor: makes payload destruction polymorphic (a derived
    /// object held through a base `Rc` is destroyed as its most-derived type)
    /// and keeps plain `delete` on `T*` safe. The counting machinery never
    /// calls `delete`; storage is reclaimed via the deallocator.
    virtual ~RefCounted() noexcept = default;

    /// Replace the storage-reclaim strategy. Useful for per-instance reclaim
    /// (e.g. objects served from an object cache, or boot singletons that must
    /// never be freed). Must be called before the object escapes into `Rc`.
    auto set_deallocate(void (*fn)(void *) noexcept) noexcept -> void {
        m_deallocate = fn;
    }

  private:
    auto strong_add() noexcept -> void {
        if constexpr (detail::IsThreadSafe<Policy>::value) {
            m_strong.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++m_strong;
        }
    }

    /// true when the strong count became zero.
    auto strong_sub() noexcept -> bool {
        if constexpr (detail::IsThreadSafe<Policy>::value) {
            return m_strong.fetch_sub(1, std::memory_order_acq_rel) == 1;
        }
        return --m_strong == 0;
    }

    auto weak_add() noexcept -> void {
        if constexpr (detail::IsThreadSafe<Policy>::value) {
            m_weak.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++m_weak;
        }
    }

    /// true when the weak count became zero.
    auto weak_sub() noexcept -> bool {
        if constexpr (detail::IsThreadSafe<Policy>::value) {
            return m_weak.fetch_sub(1, std::memory_order_acq_rel) == 1;
        }
        return --m_weak == 0;
    }

    auto destroy() noexcept -> void {
        static_cast<T *>(this)->~T();
    }

    auto reclaim() noexcept -> void {
        m_deallocate(this);
    }

    Count m_strong{};
    Count m_weak{};
    void (*m_deallocate)(void *) noexcept = &detail::default_deallocate<T>;
};

/// Intrusive reference-counted smart pointer (strong ownership).
///
/// `Policy` is carried for API symmetry with `RefCounted` and to express intent
/// (`Rc<T, sync::SingleThreadedPolicy>`); the authoritative policy is the one
/// the object's `RefCounted` base was instantiated with, because the counters
/// live in the object itself.
template <typename T, typename Policy = sync::SingleThreadedPolicy>
class Rc final {
    template <typename, typename>
    friend class Rc;
    template <typename, typename>
    friend class Weak;

  public:
    using Element = T;
    using Ptr = T const *;
    using PtrMut = T *;

    constexpr Rc() noexcept = default;
    constexpr Rc(std::nullptr_t) noexcept {}

    /// Adopt an externally allocated object. The object must have been created
    /// with a zero strong count (the normal case: allocated with `new (gaf) T`
    /// or from an object cache, or a raw singleton), and this call turns it
    /// into one strong owner.
    explicit Rc(T *pointer) noexcept : m_pointer(pointer) {
        static_assert(std::is_same_v<typename detail::ThreadPolicyOf<T>::Type, Policy>,
                      "policy mismatch: single-threaded objects use Rc<T>, "
                      "thread-safe (cross-CPU) objects use Arc<T> (= Rc<T, ThreadSafePolicy>)");
        retain();
    }

    /// Named form of the adopting constructor.
    static auto adopt(T *pointer) noexcept -> Rc {
        return Rc(pointer);
    }

    Rc(Rc const &other) noexcept : m_pointer(other.m_pointer) {
        retain();
    }
    Rc(Rc &&other) noexcept : m_pointer(std::exchange(other.m_pointer, nullptr)) {}

    /// Upcast from `Rc<U>` (U derived from T). Compile-time safe: only enabled
    /// when `U*` converts to `T*` without a downcast.
    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    Rc(Rc<U, Policy> const &other) noexcept : m_pointer(static_cast<T *>(other.m_pointer)) {
        retain();
    }

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    Rc(Rc<U, Policy> &&other) noexcept : m_pointer(static_cast<T *>(other.m_pointer)) {
        other.m_pointer = nullptr;
    }

    ~Rc() {
        release();
    }

    auto operator=(Rc const &other) noexcept -> Rc & {
        if (this != &other) {
            if (other.m_pointer) {
                other.m_pointer->retain();
            }
            if (m_pointer) {
                m_pointer->release();
            }
            m_pointer = other.m_pointer;
        }
        return *this;
    }

    auto operator=(Rc &&other) noexcept -> Rc & {
        if (this != &other) {
            if (m_pointer) {
                m_pointer->release();
            }
            m_pointer = std::exchange(other.m_pointer, nullptr);
        }
        return *this;
    }

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    auto operator=(Rc<U, Policy> const &other) noexcept -> Rc & {
        if (other.m_pointer) {
            static_cast<T *>(other.m_pointer)->retain();
        }
        if (m_pointer) {
            m_pointer->release();
        }
        m_pointer = static_cast<T *>(other.m_pointer);
        return *this;
    }

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    auto operator=(Rc<U, Policy> &&other) noexcept -> Rc & {
        if (m_pointer) {
            m_pointer->release();
        }
        m_pointer = static_cast<T *>(other.m_pointer);
        other.m_pointer = nullptr;
        return *this;
    }

    auto operator=(std::nullptr_t) noexcept -> Rc & {
        reset();
        return *this;
    }

    auto reset() noexcept -> void {
        release();
    }

    template <typename U>
    auto reset(U *pointer) noexcept -> void {
        if (pointer) {
            static_cast<T *>(pointer)->retain();
        }
        if (m_pointer) {
            m_pointer->release();
        }
        m_pointer = static_cast<T *>(pointer);
    }

    /// Transfer the strong reference to a raw pointer without touching the
    /// counter: the object stays alive until the transferred reference is
    /// re-wrapped (with `adopt`, which increments) or deliberately leaked.
    [[nodiscard]]
    auto into_raw() noexcept -> T * {
        return std::exchange(m_pointer, nullptr);
    }

    /// Create a non-owning weak reference to the same object.
    [[nodiscard]]
    auto downgrade() const noexcept -> Weak<T, Policy>;

    [[nodiscard]]
    auto use_count() const noexcept -> std::size_t {
        if (m_pointer) {
            return m_pointer->strong_count();
        }
        return 0;
    }

    [[nodiscard]]
    auto is_unique() const noexcept -> bool {
        return use_count() == 1;
    }

    /// Mutable access only when this is the sole owner; otherwise nullptr.
    /// Unlike a bare `is_unique()` + `as_ptr_mut()` check, this cannot be
    /// raced by a copy of this very handle on the same thread.
    [[nodiscard]]
    auto try_get_mut() noexcept -> T * {
        if (is_unique()) {
            return m_pointer;
        }
        return nullptr;
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

    /// Checked downcast to a derived type. Only available when the derived type
    /// opts in with a `TYPE_TAG`; returns an empty `Rc` on type mismatch
    /// instead of undefined behavior.
    template <typename U>
        requires(std::is_base_of_v<T, U> && detail::HasRcTypeTag<U>)
    [[nodiscard]]
    auto downcast() const noexcept -> Rc<U, Policy> {
        if (m_pointer && m_pointer->rc_type_tag() == U::TYPE_TAG) {
            return Rc<U, Policy>(static_cast<U *>(m_pointer));
        }
        return {};
    }

    friend auto operator==(Rc const &left, Rc const &right) noexcept -> bool {
        return left.m_pointer == right.m_pointer;
    }
    friend auto operator==(Rc const &left, std::nullptr_t) noexcept -> bool {
        return left.m_pointer == nullptr;
    }

  private:
    static auto from_locked(T *pointer) noexcept -> Rc {
        Rc rc;
        rc.m_pointer = pointer;
        return rc;
    }

    auto retain() const noexcept -> void {
        if (m_pointer) {
            m_pointer->retain();
        }
    }

    auto release() noexcept -> void {
        if (auto *pointer = std::exchange(m_pointer, nullptr)) {
            pointer->release();
        }
    }

    T *m_pointer = nullptr;
};

/// Non-owning observer. The object's storage is kept alive by the weak count,
/// so `lock()`/`upgrade()` are race-free.
template <typename T, typename Policy = sync::SingleThreadedPolicy>
class Weak final {
    template <typename, typename>
    friend class Weak;
    template <typename, typename>
    friend class Rc;

  public:
    using Element = T;
    using Ptr = T const *;

    constexpr Weak() noexcept = default;
    constexpr Weak(std::nullptr_t) noexcept {}

    Weak(Weak const &other) noexcept : m_pointer(other.m_pointer) {
        retain_weak();
    }
    Weak(Weak &&other) noexcept : m_pointer(std::exchange(other.m_pointer, nullptr)) {}

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    Weak(Weak<U, Policy> const &other) noexcept : m_pointer(static_cast<T *>(other.m_pointer)) {
        retain_weak();
    }

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    Weak(Weak<U, Policy> &&other) noexcept : m_pointer(static_cast<T *>(other.m_pointer)) {
        other.m_pointer = nullptr;
    }

    ~Weak() {
        release_weak();
    }

    auto operator=(Weak const &other) noexcept -> Weak & {
        if (this != &other) {
            if (other.m_pointer) {
                other.m_pointer->retain_weak();
            }
            if (m_pointer) {
                m_pointer->release_weak();
            }
            m_pointer = other.m_pointer;
        }
        return *this;
    }

    auto operator=(Weak &&other) noexcept -> Weak & {
        if (this != &other) {
            if (m_pointer) {
                m_pointer->release_weak();
            }
            m_pointer = std::exchange(other.m_pointer, nullptr);
        }
        return *this;
    }

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    auto operator=(Weak<U, Policy> const &other) noexcept -> Weak & {
        if (other.m_pointer) {
            static_cast<T *>(other.m_pointer)->retain_weak();
        }
        if (m_pointer) {
            m_pointer->release_weak();
        }
        m_pointer = static_cast<T *>(other.m_pointer);
        return *this;
    }

    template <typename U>
        requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
    auto operator=(Weak<U, Policy> &&other) noexcept -> Weak & {
        if (m_pointer) {
            m_pointer->release_weak();
        }
        m_pointer = static_cast<T *>(other.m_pointer);
        other.m_pointer = nullptr;
        return *this;
    }

    /// Try to obtain a strong reference; empty when the object is gone.
    [[nodiscard]]
    auto lock() const noexcept -> Rc<T, Policy> {
        if (m_pointer && m_pointer->lock_strong()) {
            return Rc<T, Policy>::from_locked(m_pointer);
        }
        return {};
    }

    [[nodiscard]]
    auto upgrade() const noexcept -> Rc<T, Policy> {
        return lock();
    }

    [[nodiscard]]
    auto expired() const noexcept -> bool {
        return m_pointer == nullptr || m_pointer->strong_count() == 0;
    }

    [[nodiscard]]
    auto use_count() const noexcept -> std::size_t {
        if (m_pointer) {
            return m_pointer->strong_count();
        }
        return 0;
    }

    [[nodiscard]]
    auto as_ptr() const noexcept -> T const * {
        return m_pointer;
    }

    friend auto operator==(Weak const &left, Weak const &right) noexcept -> bool {
        return left.m_pointer == right.m_pointer;
    }
    friend auto operator==(Weak const &left, std::nullptr_t) noexcept -> bool {
        return left.m_pointer == nullptr;
    }

  private:
    explicit Weak(T *pointer) noexcept : m_pointer(pointer) {
        static_assert(
            std::is_same_v<typename detail::ThreadPolicyOf<T>::Type, Policy>,
            "policy mismatch: use Weak<T> for Rc<T>, Weak<T, ThreadSafePolicy> for Arc<T>");
        retain_weak();
    }

    auto retain_weak() noexcept -> void {
        if (m_pointer) {
            m_pointer->retain_weak();
        }
    }

    auto release_weak() noexcept -> void {
        if (auto *pointer = std::exchange(m_pointer, nullptr)) {
            pointer->release_weak();
        }
    }

    T *m_pointer = nullptr;
};

/// Thread-safe reference-counted pointer (Rust's `Arc`): atomic counting,
/// safe to share across CPUs. The default `Rc<T>` is the single-threaded
/// counterpart (Rust's `Rc`); pick `Arc` explicitly when the object is shared
/// between threads.
template <typename T>
using Arc = Rc<T, sync::ThreadSafePolicy>;

template <typename T>
using Arc = Rc<T, sync::ThreadSafePolicy>;

/// Adopt a raw pointer (incrementing re-wrap). `T` is explicit; `Policy` is
/// optional (defaults to the single-threaded `Rc`); `U` is deduced.
template <typename T, typename Policy = sync::SingleThreadedPolicy, typename U>
[[nodiscard]]
auto adopt(U *pointer) noexcept -> Rc<T, Policy> {
    return Rc<T, Policy>(static_cast<T *>(pointer));
}

template <typename T, typename U, typename Policy>
[[nodiscard]]
auto adopt(Rc<U, Policy> const &pointer) noexcept -> Rc<T, Policy> {
    return Rc<T, Policy>(pointer);
}

template <typename T, typename U, typename Policy>
[[nodiscard]]
auto adopt(Rc<U, Policy> &&pointer) noexcept -> Rc<T, Policy> {
    return Rc<T, Policy>(std::move(pointer));
}

/// Compile-time safe upcast.
template <typename T, typename U, typename Policy>
[[nodiscard]]
auto upcast(Rc<U, Policy> const &pointer) noexcept -> Rc<T, Policy> {
    return Rc<T, Policy>(pointer);
}

template <typename T, typename U, typename Policy>
[[nodiscard]]
auto upcast(Rc<U, Policy> &&pointer) noexcept -> Rc<T, Policy> {
    return Rc<T, Policy>(std::move(pointer));
}

/// Checked downcast: empty `Rc` on type mismatch, undefined behavior-free.
template <typename T, typename U, typename Policy>
    requires(std::is_base_of_v<U, T> && detail::HasRcTypeTag<T>)
[[nodiscard]]
auto downcast(Rc<U, Policy> const &pointer) noexcept -> Rc<T, Policy> {
    return pointer.template downcast<T>();
}

template <typename T, typename Policy>
auto Rc<T, Policy>::downgrade() const noexcept -> Weak<T, Policy> {
    return Weak<T, Policy>(m_pointer);
}

} // namespace ustl

#endif // USTL_RC_HPP
