/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_RECLAIMER_HPP
#define OURS_MM_MEMORY_RECLAIMER_HPP 1

#include <ours/status.hpp>
#include <ours/sync/event.hpp>
#include <ours/types.hpp>

#include <atomic>

namespace ours::mm {

/// Gives memory back when the memory watchdog says it runs short: the pages
/// that all-zero anonymous memory holds, then the clean pages a page source
/// supplied (PageQueues::dedup_zero_pages() and PageQueues::reclaim()), on
/// every node.
///
/// Zero pages go first because giving one back costs nothing later: a read
/// finds a marker, a write commits a page. A pager page given back is asked
/// of its source again when it is needed.
///
/// Called two ways, both by the watchdog (D3 of .agents/tasks/page-reclaim):
/// request() at Warning and Critical, which wakes a thread of its own and
/// returns, and reclaim() at OutOfMemory, which the watchdog's thread calls
/// and waits for before it refuses the allocations that could wait. Nothing
/// reclaims in the background, or when the watchdog has not asked.
class MemoryReclaimer {
    using Self = MemoryReclaimer;

  public:
    /// The one reclaimer.
    static auto get() -> Self &;

    MemoryReclaimer(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// Starts its thread. OutOfMem when there is none for it.
    auto start() -> Status;

    /// Asks the thread to give back |frames| frames, and returns. A request
    /// made while one is being served is added to it as the larger of the two,
    /// not their sum: the watchdog asks again each time it looks, with the
    /// distance to where it wants free memory, which has not changed.
    auto request(usize frames) -> void;

    /// Gives back up to |frames| frames now, on the calling thread: zero
    /// pages on every node first, then pager pages on every node. How many
    /// frames it gave back, 0 while a Pause lives. Takes locks that sleep.
    auto reclaim(usize frames) -> usize;

    /// While one is alive nothing is reclaimed, request() or reclaim(): for a
    /// test that drives the watchdog to pressures free memory is not at, and
    /// wants to see it decide and not what reclaiming does to the answer.
    class Pause {
      public:
        Pause() {
            get().m_pauses.fetch_add(1, std::memory_order_acq_rel);
        }
        ~Pause() {
            get().m_pauses.fetch_sub(1, std::memory_order_acq_rel);
        }

        Pause(Pause const &) = delete;
        auto operator=(Pause const &) -> Pause & = delete;
    };

  private:
    MemoryReclaimer() = default;

    /// The thread: sleeps until a request, then reclaims what was asked.
    auto run() -> i32;

    [[nodiscard]]
    auto paused() const -> bool {
        return m_pauses.load(std::memory_order_acquire) != 0;
    }

    /// What wakes the thread. Auto-unsignal, so each wakes it once.
    sync::Event m_signal{sync::Event::Options::AutoUnsignal};

    /// What has been asked and not yet taken by the thread.
    std::atomic<usize> m_wanted{0};

    std::atomic<usize> m_pauses{0};
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMORY_RECLAIMER_HPP
