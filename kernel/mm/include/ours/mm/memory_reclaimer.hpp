/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_RECLAIMER_HPP
#define OURS_MM_MEMORY_RECLAIMER_HPP 1

#include <ours/mm/types.hpp>
#include <ours/node_mask.hpp>
#include <ours/status.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/types.hpp>

#include <atomic>

namespace ours::mm {
class PmNode;

/// Gives memory back when the memory watchdog says it runs short: the pages
/// that all-zero anonymous memory holds, then the clean pages a page source
/// supplied (PageQueues::dedup_zero_pages() and PageQueues::reclaim()), on
/// every node.
///
/// Zero pages go first because giving one back costs nothing later: a read
/// finds a marker, a write commits a page. A pager page given back is asked
/// of its source again when it is needed.
///
/// The watchdog requests global background work at Warning and Critical and
/// reclaims synchronously at OutOfMemory. A zone an allocation finds short is
/// recorded on its node by the PMM, which wakes the thread through the event
/// the reclaimer registers (set_reclaim_signal()); the thread then walks that
/// node alone. One reclaim walk runs at a time.
class MemoryReclaimer {
    using Self = MemoryReclaimer;

  public:
    /// The one reclaimer.
    static auto get() -> Self &;

    MemoryReclaimer(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// Starts its thread. OutOfMem when there is none for it.
    auto start() -> Status;

    /// Asks the thread to give back |frames| frames, and returns. Two asks
    /// the thread has not taken keep the larger. Each is a distance from the
    /// free memory just read, and the thread has not run between them.
    auto request(usize frames) -> void;

    /// Gives back up to |frames| frames now, on the calling thread: zero
    /// pages on every node first, then pager pages on every node. How many
    /// frames it gave back, 0 while a Pause lives or |frames| is 0. A walk
    /// already running is waited out first. Takes locks that sleep.
    auto reclaim(usize frames) -> usize;

    /// reclaim(), in place of the ask the thread has not taken: the ask is
    /// dropped first. Each is a distance from the free memory read when it was
    /// made, so the ask left from an earlier look is stale once this walk has
    /// its own count, and walking it afterwards would reclaim after free
    /// memory is back. The watchdog's walk at OutOfMemory.
    auto reclaim_superseding(usize frames) -> usize;

    /// The thread's take of |node|'s record of short zones
    /// (PmNode::take_reclaim_target()), refused while a Pause lives: then 0,
    /// and the record left as it is. Decided under m_take_lock, which a Pause
    /// takes as it begins, so no take starts once a Pause has begun.
    [[nodiscard]]
    auto take_node_target(PmNode &node) -> usize;

    /// While one is alive nothing new is reclaimed: request() is dropped,
    /// reclaim() refuses, and the thread takes no node's record of short
    /// zones, so a test can read one undisturbed. A zone found short meanwhile
    /// is still recorded on its node (the PMM marks it); the record waits, and
    /// the last Pause to end wakes the thread, which takes it. For a test that
    /// drives the watchdog to pressures free memory is not at, and wants to
    /// see it decide and not what reclaiming does to the answer.
    ///
    /// What had already started runs to its end: a walk under way, and the
    /// walk of a record the thread took before the Pause began (a record is
    /// never taken and then dropped).
    class Pause {
      public:
        explicit Pause(MemoryReclaimer &reclaimer);
        ~Pause();

        Pause(Pause const &) = delete;
        auto operator=(Pause const &) -> Pause & = delete;

      private:
        MemoryReclaimer &m_reclaimer;
    };

  private:
    /// The ask the thread has not taken, or 0. The thread walks what this
    /// returns; reclaim_superseding() drops it.
    [[nodiscard]]
    auto take() -> usize;

    MemoryReclaimer() = default;

    /// The thread: sleeps until a request, then reclaims what was asked.
    auto run() -> i32;

    auto reclaim(usize frames, NodeMask const &nodes) -> usize;

    auto reclaim_node_requests() -> void;

    [[nodiscard]]
    auto paused() const -> bool {
        return m_pauses.load(std::memory_order_acquire) != 0;
    }

    /// What wakes the thread. Auto-unsignal, so each wakes it once.
    sync::Event m_signal{sync::Event::Options::AutoUnsignal};

    /// What has been asked and not yet taken by the thread.
    std::atomic<usize> m_wanted{0};

    std::atomic<usize> m_pauses{0};

    /// Held for one walk, reclaim()'s or the walk of a node record the thread
    /// took (reclaim_node_requests()), and nothing else. request() does not take it:
    /// the watchdog calls request() while holding its own mutex, and a walk
    /// sleeps in the locks it takes, so taking this there would make every
    /// look wait the walk out.
    sync::Mutex m_reclaim_lock;

    /// Orders the thread's take of a node record against the start of a
    /// Pause: take_node_target() asks paused() and takes under it, and a
    /// Pause takes it once after counting itself in. Held for one node's
    /// take, which reads counters and never sleeps, so a Pause waits out at
    /// most that, never a walk.
    sync::SpinLock m_take_lock;

    /// The walk itself, zero pages and then pager pages on |nodes|, up to
    /// |frames|: what reclaim() does once it has decided to. Declared after
    /// the lock it requires, which the annotation names.
    TA_REQUIRES(m_reclaim_lock)
    auto walk(usize frames, NodeMask const &nodes) -> usize;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMORY_RECLAIMER_HPP
