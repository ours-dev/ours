/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_SYNC_ATOMIC_HPP
#define USTL_SYNC_ATOMIC_HPP 1

#include <cstddef>
#include <cstdint>

/// The ours kernel no longer uses anything in this header. It moved to
/// std::atomic, which is freestanding-safe and is the spelling the repository
/// prefers wherever the standard one works, and lockdep, the last user
/// there, followed. The wrappers stay because ustl is a standalone library
/// whose headers need not serve only this kernel. New code should use
/// std::atomic and std::memory_order rather than copy these.
namespace ustl::sync {

struct MemoryOrder {
    static constexpr auto Relaxed = __ATOMIC_RELAXED;
    static constexpr auto Consume = __ATOMIC_CONSUME;
    static constexpr auto Acquire = __ATOMIC_ACQUIRE;
    static constexpr auto Release = __ATOMIC_RELEASE;
    static constexpr auto AcqRel = __ATOMIC_ACQ_REL;
    static constexpr auto SeqCst = __ATOMIC_SEQ_CST;
};

template <typename T>
class Atomic {
  public:
    constexpr Atomic() noexcept = default;
    constexpr Atomic(T value) noexcept : value_(value) {}

    Atomic(Atomic const &) = delete;
    auto operator=(Atomic const &) -> Atomic & = delete;

    auto operator=(T value) noexcept -> Atomic & {
        store(value);
        return *this;
    }

    operator T() const noexcept {
        return load();
    }

    auto load(int order = MemoryOrder::SeqCst) const noexcept -> T {
        T value;
        __atomic_load(&value_, &value, order);
        return value;
    }

    auto store(T value, int order = MemoryOrder::SeqCst) noexcept -> void {
        __atomic_store(&value_, &value, order);
    }

    auto exchange(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        T previous;
        __atomic_exchange(&value_, &value, &previous, order);
        return previous;
    }

    auto compare_exchange_weak(T &expected, T desired, int success,
                               int failure = MemoryOrder::SeqCst) noexcept -> bool {
        return __atomic_compare_exchange(&value_, &expected, &desired, true, success, failure);
    }

    auto compare_exchange_strong(T &expected, T desired, int success,
                                 int failure = MemoryOrder::SeqCst) noexcept -> bool {
        return __atomic_compare_exchange(&value_, &expected, &desired, false, success, failure);
    }

    auto fetch_add(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_add(&value_, value, order);
    }

    auto fetch_sub(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_sub(&value_, value, order);
    }

    auto fetch_or(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_or(&value_, value, order);
    }

    auto fetch_and(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_and(&value_, value, order);
    }

    auto fetch_xor(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_xor(&value_, value, order);
    }

    auto operator+=(T value) noexcept -> Atomic & {
        static_cast<void>(fetch_add(value));
        return *this;
    }

    auto operator-=(T value) noexcept -> Atomic & {
        static_cast<void>(fetch_sub(value));
        return *this;
    }

  private:
    alignas(T) T value_{};
};

template <typename T>
class AtomicRef {
  public:
    explicit AtomicRef(T &value) noexcept : value_(&value) {}

    AtomicRef(AtomicRef const &) = default;
    auto operator=(AtomicRef const &) -> AtomicRef & = delete;

    operator T() const noexcept {
        return load();
    }

    auto load(int order = MemoryOrder::SeqCst) const noexcept -> T {
        T value;
        __atomic_load(value_, &value, order);
        return value;
    }

    auto store(T value, int order = MemoryOrder::SeqCst) noexcept -> void {
        __atomic_store(value_, &value, order);
    }

    auto exchange(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        T previous;
        __atomic_exchange(value_, &value, &previous, order);
        return previous;
    }

    auto compare_exchange_weak(T &expected, T desired, int success,
                               int failure = MemoryOrder::SeqCst) noexcept -> bool {
        return __atomic_compare_exchange(value_, &expected, &desired, true, success, failure);
    }

    auto compare_exchange_strong(T &expected, T desired, int success,
                                 int failure = MemoryOrder::SeqCst) noexcept -> bool {
        return __atomic_compare_exchange(value_, &expected, &desired, false, success, failure);
    }

    auto fetch_add(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_add(value_, value, order);
    }

    auto fetch_sub(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_sub(value_, value, order);
    }

    auto fetch_or(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_or(value_, value, order);
    }

    auto fetch_and(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_and(value_, value, order);
    }

    auto fetch_xor(T value, int order = MemoryOrder::SeqCst) noexcept -> T {
        return __atomic_fetch_xor(value_, value, order);
    }

  private:
    T *value_;
};

using AtomicU8 = Atomic<std::uint8_t>;
using AtomicU16 = Atomic<std::uint16_t>;
using AtomicU32 = Atomic<std::uint32_t>;
using AtomicU64 = Atomic<std::uint64_t>;
using AtomicUsize = Atomic<std::size_t>;
using AtomicI8 = Atomic<std::int8_t>;
using AtomicI16 = Atomic<std::int16_t>;
using AtomicI32 = Atomic<std::int32_t>;
using AtomicI64 = Atomic<std::int64_t>;
using AtomicIsize = Atomic<std::ptrdiff_t>;

inline auto atomic_signal_fence(int order) noexcept -> void {
    __atomic_signal_fence(order);
}

inline auto atomic_thread_fence(int order) noexcept -> void {
    __atomic_thread_fence(order);
}

} // namespace ustl::sync

#endif // USTL_SYNC_ATOMIC_HPP
