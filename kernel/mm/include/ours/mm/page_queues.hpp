/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PAGE_QUEUES_HPP
#define OURS_MM_PAGE_QUEUES_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_page.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/types.hpp>
#include <ustl/rc.hpp>

#include <array>

namespace ours::mm {
class PageQueues {
    using Self = PageQueues;

  public:
    static constexpr auto NUM_RECLAIMABLE = 8_usize;

    auto set_pinned(VmPage *, VmCowPages *, PgOff) -> void;
    auto set_anonymous(VmPage *, VmCowPages *, PgOff) -> void;
    auto set_reclaimable(VmPage *, VmCowPages *, PgOff) -> void;

    auto mark_accessed(VmPage *page) -> void;

    auto start_runners() -> void;

    PageQueues();

    /// Both defined out-of-line in page_queues.cpp: m_mru_runner and
    /// m_lru_runner are Arc<> of MruRunner/LruRunner, which are only
    /// defined in that file. An implicit destructor would be emitted
    /// wherever PageQueues is destroyed -- PmNode holds one by value --
    /// and Arc's destructor needs the complete type to run ~T().
    ~PageQueues();

  private:
    enum class QueueType {
        None,
        Anonymous,
        Pinned,
        ReclaimableStart,
        ReclaimableEnd = ReclaimableStart + NUM_RECLAIMABLE - 1,
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
        return QueueType(usize(QueueType::ReclaimableStart) +
                         m_lrugen.load(ustl::sync::MemoryOrder::Acquire));
    }

    auto lrugen_to_queue_type() -> QueueType {
        return QueueType(usize(QueueType::ReclaimableStart) +
                         m_lrugen.load(ustl::sync::MemoryOrder::Acquire));
    }

    static constexpr auto NUM_QUEUES = static_cast<usize>(QueueType::NumTypes);

    mutable sync::SpinLock m_lock;
    std::array<VmPage::List, NUM_QUEUES> m_queues;
    std::array<ustl::sync::AtomicUsize, NUM_QUEUES> m_sizes;
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