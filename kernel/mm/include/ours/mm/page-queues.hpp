/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PAGE_QUEUES_HPP
#define OURS_MM_PAGE_QUEUES_HPP 1

#include <ours/mm/vm_page.hpp>
#include <ours/mm/types.hpp>

#include <ours/sync/spin-lock.hpp>
#include <ours/task/types.hpp>
#include <ours/syscall/time.hpp>

#include <ustl/rc.hpp>
#include <ustl/array.hpp>

namespace ours::mm {
    class PageQueues {
        using Self = PageQueues;
      public:
        static constexpr auto kNumReclaimable = 8_usize;

        auto set_pinned(VmPage *, VmCowPages *, PgOff) -> void;
        auto set_anonymous(VmPage *, VmCowPages *, PgOff) -> void;
        auto set_reclaimable(VmPage *, VmCowPages *, PgOff) -> void;

        auto mark_accessed(VmPage *page) -> void;

        auto start_runners() -> void;
      private:
        enum class QueueType {
            None,
            Anonymous,
            Pinned,
            ReclaimableStart,
            ReclaimableEnd = ReclaimableStart + kNumReclaimable - 1,
            NumTypes,
        };

        class MruRunner;
        class LruRunner;

        auto set_queue_backlink_locked(VmPage *, VmCowPages *, PgOff, QueueType) -> void;

        enum class AgeReason {
            Timeout,
            Manual,
        };
        friend auto to_string(AgeReason) -> char const *;

        auto rotate_reclaimable_queues(AgeReason reason) -> void;

        auto get_lock() -> sync::SpinLock & {
            return m_lock;
        }

        auto mrugen_to_queue_type() -> QueueType {
            return QueueType(usize(QueueType::ReclaimableStart) + m_lrugen.load(ustl::sync::MemoryOrder::Acquire));
        }

        auto lrugen_to_queue_type() -> QueueType {
            return QueueType(usize(QueueType::ReclaimableStart) + m_lrugen.load(ustl::sync::MemoryOrder::Acquire));
        }

        static constexpr auto kNumQueues = static_cast<usize>(QueueType::NumTypes);

        mutable sync::SpinLock m_lock;
        ustl::Array<VmPage::List, kNumQueues> m_queues;
        ustl::Array<ustl::sync::AtomicUsize, kNumQueues> m_sizes;
        ustl::sync::AtomicU32 m_lrugen;
        ustl::sync::AtomicU32 m_mrugen;

        TimePoint<> m_last_age_time;
        AgeReason m_last_age_reason;

        ustl::Arc<MruRunner> m_mru_runner;
        ustl::Arc<LruRunner> m_lru_runner;
        Duration m_min_rotate_time;
        Duration m_max_rotate_time;
    };

} // namespace ours::mm

#endif // #ifndef OURS_MM_PAGE_QUEUES_HPP