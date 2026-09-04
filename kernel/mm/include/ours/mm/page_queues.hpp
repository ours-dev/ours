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
#include <utility>

namespace ours::mm {
/// The set of lists a VMM page can sit on while it is owned by a VMO.
///
/// Membership is exclusive: a page is on exactly one of these, or on none.
/// That is not a policy choice but a consequence of the frame descriptor
/// carrying a single list hook (see page::VmmBody in pm_frame.hpp) -- the same
/// hook the buddy allocator threads free frames through. Every entry point
/// here therefore *moves* a page rather than inserting it, and the queue a
/// page currently sits on is recorded in the descriptor so the move can find
/// the list it has to leave.
class PageQueues {
    using Self = PageQueues;

  public:
    /// How many generations the reclaimable ring holds. Aging advances the
    /// most-recently-used generation into the next slot, so this is also the
    /// number of distinct ages a page can be told apart by before the oldest
    /// generation has to be folded forward.
    static constexpr auto NUM_RECLAIMABLE = 8_usize;

    auto set_pinned(VmPage *, VmCowPages *, PgOff) -> void;
    auto set_anonymous(VmPage *, VmCowPages *, PgOff) -> void;
    auto set_reclaimable(VmPage *, VmCowPages *, PgOff) -> void;

    /// Takes |page| off whatever queue holds it and drops its backlink.
    ///
    /// This is the other half of the three setters above, and it is not
    /// optional: the setters record a raw `VmCowPages *` in the descriptor,
    /// so a page handed back to the buddy allocator while still queued would
    /// leave both a stale owner pointer and a list node pointing into a frame
    /// that has since been reused for something else.
    auto remove(VmPage *page) -> void;

    /// Re-files |page| into the youngest generation, if it is reclaimable and
    /// not already there. This is the only input the generational scheme has:
    /// without it the generations record when a page was first filed, not when
    /// it was last touched, and aging degenerates into a FIFO.
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
    /// Narrow on purpose: the value is mirrored into a byte of the frame
    /// descriptor's VMM payload, whose budget is counted in pm_frame.hpp.
    enum class QueueType : u8 {
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

    /// Detaches |page| from its queue and marks it unqueued. Leaves the
    /// backlink alone: the setters call this to vacate the hook before
    /// re-inserting, and they are about to overwrite the backlink anyway.
    auto dequeue_locked(VmPage *page) -> void;

    enum class AgeReason {
        Timeout,
        Manual,
    };
    friend auto to_string(AgeReason) -> char const *;

    /// Ages every reclaimable page by one generation.
    ///
    /// Nothing moves. A page's age is its distance from the most-recently-used
    /// generation, so advancing that generation ages every page already filed,
    /// in constant time and without touching a single descriptor. That is the
    /// whole point of a generational LRU, and the counterpart of Linux's
    /// inc_max_seq().
    auto rotate_reclaimable_queues(AgeReason reason) -> void;

    /// Empties the oldest generation into the next one and retires its slot.
    ///
    /// Called only when the ring is full, because it is the one operation here
    /// that is not O(1): every page it hands over has to be re-tagged. It is
    /// also where reclaim would go if this kernel had anywhere to reclaim to
    /// -- see the comment on the definition.
    auto advance_lru_locked() -> void;

    auto get_lock() -> sync::SpinLock & {
        return m_lock;
    }

    static constexpr auto queue_index(QueueType type) -> usize {
        return static_cast<usize>(std::to_underlying(type));
    }

    /// The ring slot holding generation |gen|.
    ///
    /// The generation counters are free-running u32s while the ring has only
    /// NUM_RECLAIMABLE slots, so the mapping has to be modular. Without the
    /// wrap the index walks straight past ReclaimableEnd on the eighth
    /// rotation and starts writing off the end of m_queues -- which is what
    /// the previous `ReclaimableStart + m_mrugen` did.
    static constexpr auto reclaimable_queue(u32 gen) -> QueueType {
        return QueueType(std::to_underlying(QueueType::ReclaimableStart) + gen % NUM_RECLAIMABLE);
    }

    /// The generation new and freshly-accessed pages are filed into.
    auto mru_queue() const -> QueueType {
        return reclaimable_queue(m_mrugen.load(ustl::sync::MemoryOrder::Relaxed));
    }

    /// The oldest generation still held, i.e. the one reclaim would look at
    /// first.
    auto lru_queue() const -> QueueType {
        return reclaimable_queue(m_lrugen.load(ustl::sync::MemoryOrder::Relaxed));
    }

    /// Spelled out rather than routed through queue_index(): an inline member
    /// function's body is only compiled at the closing brace of the class, so
    /// it cannot be constant-evaluated by a static data member initializer
    /// that appears in the same class.
    static constexpr auto NUM_QUEUES = static_cast<usize>(std::to_underlying(QueueType::NumTypes));

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
