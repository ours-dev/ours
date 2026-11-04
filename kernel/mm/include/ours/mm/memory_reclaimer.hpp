/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_RECLAIMER_HPP
#define OURS_MM_MEMORY_RECLAIMER_HPP 1

#include <ours/node_mask.hpp>
#include <ours/status.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/types.hpp>

#include <array>
#include <atomic>

namespace ours::mm {
class PmZone;

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
/// reclaims synchronously at OutOfMemory. An eligible zone short of frames can
/// also request background work on its own node. One reclaim walk runs at a time.
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

    /// Queue a shortage observed by an allocation. The worker samples the
    /// zone again and walks only its node's page queues.
    auto request(PmZone const &zone) -> void;

    /// The ask the thread has not taken, or 0. The thread walks what this
    /// returns. A synchronous reclaim calls it and drops the result: that
    /// walk has its own count, and an ask left from an earlier look is not
    /// walked after it.
    [[nodiscard]]
    auto take() -> usize;

    /// Gives back up to |frames| frames now, on the calling thread: zero
    /// pages on every node first, then pager pages on every node. How many
    /// frames it gave back, 0 while a Pause lives or |frames| is 0. A walk
    /// already running is waited out first. Takes locks that sleep.
    auto reclaim(usize frames) -> usize;

    /// While one is alive nothing is reclaimed, request() or reclaim(): for a
    /// test that drives the watchdog to pressures free memory is not at, and
    /// wants to see it decide and not what reclaiming does to the answer.
    class Pause {
      public:
        explicit Pause(MemoryReclaimer &reclaimer) : m_reclaimer(reclaimer) {
            m_reclaimer.m_pauses.fetch_add(1, std::memory_order_acq_rel);
        }
        ~Pause() {
            m_reclaimer.m_pauses.fetch_sub(1, std::memory_order_acq_rel);
        }

        Pause(Pause const &) = delete;
        auto operator=(Pause const &) -> Pause & = delete;

      private:
        MemoryReclaimer &m_reclaimer;
    };

  private:
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

    /// One bit per zone type on each node, coalesced until the worker samples it.
    std::array<std::atomic<usize>, MAX_NODES> m_pending_zones{};

    std::atomic<bool> m_started{false};

    std::atomic<usize> m_pauses{0};

    /// Held for one reclaim() and nothing else. request() does not take it:
    /// the watchdog calls request() while holding its own mutex, and a walk
    /// sleeps in the locks it takes, so taking this there would make every
    /// look wait the walk out.
    sync::Mutex m_reclaim_lock;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMORY_RECLAIMER_HPP
