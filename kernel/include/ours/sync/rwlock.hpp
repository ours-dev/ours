/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_RWLOCK_HPP
#define OURS_SYNC_RWLOCK_HPP 1

#include <gktl/canary.hpp>
#include <ktl/result.hpp>
#include <ours/sync/types.hpp>
#include <ours/task/types.hpp>
#include <ustl/limits.hpp>
#include <ustl/sync/atomic.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::sync {
namespace priv {
    enum class EnablePi : bool {
        No = false,
        Yes = true,
    };

    template <EnablePi>
    struct DispatchWaitQueue;

    template <>
    struct DispatchWaitQueue<EnablePi::No> {
        using Type = task::WaitQueue;
    };

    template <>
    struct DispatchWaitQueue<EnablePi::Yes> {
        using Type = task::OwnedWaitQueue;
    };

    template <EnablePi>
    struct DispatchLockState;

    template <>
    struct DispatchLockState<EnablePi::No> {
        FORCE_INLINE auto increase_reader_count(u32 count) -> void {
            m_num_reader.fetch_add(count, ustl::sync::MemoryOrder::Release);
        }

        FORCE_INLINE auto decrease_reader_count(u32 count) -> void {
            m_num_writer.fetch_sub(count, ustl::sync::MemoryOrder::Release);
        }

        ustl::sync::AtomicU32 m_num_reader;
        ustl::sync::AtomicU32 m_num_writer;
    };

    template <>
    struct DispatchLockState<EnablePi::Yes>: DispatchLockState<EnablePi::No> {
        task::Thread *m_owner;
    };

    template <EnablePi PI>
    class RwLock {
        using Self = RwLock;

      public:
        enum class Owner {
            Writer,
            Reader,
        };

        template <Owner Owner>
        class Guard;

        /// RAII object type.
        using ReaderGuard = Guard<Owner::Reader>;
        using WriterGuard = Guard<Owner::Writer>;

        auto read_acquire() -> void;
        auto write_acquire() -> void;

        auto try_read_acquire() -> bool;
        auto try_write_acquire() -> bool;

        auto read_release() -> void;
        auto write_release() -> void;

      private:
        gktl::Canary m_canary;

        using WaitQueue = typename DispatchWaitQueue<PI>::Type;
        WaitQueue m_waiters;

        using LockState = typename DispatchLockState<PI>::Type;
        LockState m_lockstate;
    };

    template <EnablePi PI>
    template <RwLock<PI>::Owner Owner>
    class RwLock<PI>::Guard<Owner> {
        using Self = Guard;

      public:
        FORCE_INLINE Guard(RwLock &lock) : m_lock(lock) {
            if constexpr (Owner == Owner::Reader) {
                m_lock.read_acquire();
            } else {
                m_lock.write_acquire();
            }
            m_guarded = 1;
        }

        FORCE_INLINE auto unguard() -> void {
            if (!m_guarded) {
                return;
            }

            if constexpr (Owner == Owner::Reader) {
                m_lock.read_release();
            } else {
                m_lock.write_release();
            }

            m_guarded = 0;
        }

        FORCE_INLINE ~Guard() {
            unguard();
        }

      private:
        USTL_NOCOPYABLE(Guard);

        usize m_guarded;
        RwLock &m_lock;
    };

} // namespace priv

/// @Usage
///
/// 1) Acquires a reader lock.
///   a) By inner guard type of lock.
///   {
///     RwLock::ReaderGuard guard{rwlock};
///     ...
///   }
///
///   b) By public interfaces RwLock::[try_]read.
///
/// 2) Acquires a writer lock, same as case 1) above.
using RwLockNoPi = priv::RwLock<priv::EnablePi::No>;
using RwLockPi = priv::RwLock<priv::EnablePi::Yes>;

template <typename Class>
using RwLock = priv::RwLock<priv::EnablePi::Yes>;

} // namespace ours::sync

#endif // #ifndef OURS_SYNC_RWLOCK_HPP