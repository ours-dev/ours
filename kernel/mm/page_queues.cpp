#include <ours/lockdep.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ustl/rc.hpp>
#include <ustl/sync/atomic.hpp>

#include <utility>

using ustl::sync::Atomic;
using ustl::sync::MemoryOrder;

namespace ours::mm {
/// Held through Arc<> by PageQueues, so it has to be reference counted.
class PageQueues::MruRunner: public ustl::RefCounted<MruRunner, ustl::sync::ThreadSafePolicy> {
    using Self = PageQueues::MruRunner;

  public:
  private:
    [[maybe_unused]]
    task::Thread *m_thread;
};

/// Held through Arc<> by PageQueues, so it has to be reference counted.
class PageQueues::LruRunner: public ustl::RefCounted<LruRunner, ustl::sync::ThreadSafePolicy> {
    using Self = PageQueues::LruRunner;

  public:
    enum class State {
        RequestShutdown,
        Shutdown,
    };

    auto run() -> void;

  private:
    Atomic<State> m_state;
    [[maybe_unused]]
    task::Thread *m_thread;
};

auto PageQueues::LruRunner::run() -> void {
    while (State::Shutdown == m_state.load(MemoryOrder::Relaxed)) {
    }
}

/// The two generation counters have to start equal: `mru - lru + 1` is the
/// number of occupied slots, and every decision about the ring is made from
/// it. ustl::sync::Atomic's default constructor is `= default`, which leaves
/// its storage indeterminate, so a PageQueues that is not in .bss would start
/// out claiming an arbitrary number of live generations.
PageQueues::PageQueues() : m_lrugen(0), m_mrugen(0), m_last_age_reason(AgeReason::Manual) {}

PageQueues::~PageQueues() = default;

auto PageQueues::set_pinned(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return set_queue_backlink_locked(page, object, pgoff, QueueType::Pinned);
}

auto PageQueues::set_anonymous(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return set_queue_backlink_locked(page, object, pgoff, QueueType::Anonymous);
}

auto PageQueues::set_reclaimable(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return set_queue_backlink_locked(page, object, pgoff, mru_queue());
}

auto PageQueues::remove(VmPage *page) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    if (QueueType::None == QueueType(page->page_queue())) {
        return;
    }

    dequeue_locked(page);

    // Drop the backlink as well, which the setters installed. A frame on its
    // way back to the buddy allocator keeps this payload until the next
    // become(), so leaving the owner pointer behind would hand the next reader
    // of the descriptor a VmCowPages that may already be gone.
    page->set_object(nullptr);
    page->set_pgoff(0);
}

auto PageQueues::set_queue_backlink_locked(VmPage *page, VmCowPages *object, PgOff pgoff,
                                           QueueType type) -> void {
    // Vacate the hook first. A frame carries one list hook for all of its
    // roles (see page::VmmBody in pm_frame.hpp), so "put this page on queue X"
    // is always a move: push_back() on a node that is still linked elsewhere
    // splices the old queue's neighbours into the new one, and both lists are
    // then circular through the wrong nodes.
    //
    // Re-filing a page that is already queued is the normal case, not the
    // exceptional one -- it is what aging and mark_accessed() do -- so this
    // has to be cheap and unconditional rather than an assertion.
    dequeue_locked(page);

    page->set_pgoff(pgoff);
    page->set_object(object);
    page->set_page_queue(std::to_underlying(type));

    auto const index = queue_index(type);
    m_queues[index].push_back(*page);
    m_sizes[index].fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
}

auto PageQueues::dequeue_locked(VmPage *page) -> void {
    auto const type = QueueType(page->page_queue());
    if (QueueType::None == type) {
        // Either never filed, or filed and taken off again. It may still be
        // linked -- the buddy allocator threads free frames through the same
        // hook -- so this early return is what keeps us from stealing a page
        // out of a list that is not ours.
        return;
    }

    // AutoUnlink, so the node detaches itself without the list header; that is
    // exactly why the size has to be maintained by hand here. m_queues is
    // declared ConstantTimeSize<false> in pm_frame.hpp because the buddy
    // allocator wants splices to stay O(1), and Boost-style intrusive
    // containers refuse to combine auto-unlink with a cached size for this
    // reason: an unlink cannot reach the header to decrement it.
    page->unlink();
    page->set_page_queue(std::to_underlying(QueueType::None));
    m_sizes[queue_index(type)].fetch_sub(1, ustl::sync::MemoryOrder::Relaxed);
}

auto PageQueues::mark_accessed(VmPage *page) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    auto const current = QueueType(page->page_queue());
    if (current < QueueType::ReclaimableStart) {
        // Anonymous and pinned pages have no age to refresh, and an unqueued
        // page is not ours to file -- whoever owns it decides which queue it
        // belongs on.
        return;
    }

    auto const mru = mru_queue();
    if (current == mru) {
        return;
    }

    // A real list move, not just a re-tag. Zircon's MarkAccessed writes the
    // new generation into the descriptor with a compare-exchange and leaves
    // the page on the old list, letting ProcessLruQueue reconcile the two
    // later; that buys a lock-free fast path at the price of a descriptor
    // whose queue byte deliberately disagrees with the list holding it.
    // Everything here already runs under m_lock, so there is nothing to buy,
    // and the invariant that the byte names the list is worth more -- it is
    // what dequeue_locked() trusts to decrement the right counter.
    set_queue_backlink_locked(page, static_cast<VmCowPages *>(page->get_object()),
                              page->get_pgoff(), mru);
}

auto PageQueues::advance_lru_locked() -> void {
    auto const gen = m_lrugen.load(ustl::sync::MemoryOrder::Relaxed);
    auto const older = reclaimable_queue(gen);
    auto const newer = reclaimable_queue(gen + 1);
    auto const from = queue_index(older);
    auto const to = queue_index(newer);

    auto &source = m_queues[from];
    auto &target = m_queues[to];

    // The relink itself is one O(1) splice; the walk exists only to re-tag.
    // That byte is not decoration -- dequeue_locked() reads it to decide which
    // counter to decrement, so a bulk splice that left the old generation
    // written in every descriptor would silently corrupt m_sizes on the next
    // removal. Zircon pays more here: ProcessLruQueue re-queues page by page.
    usize moved = 0;
    for (auto &page: source) {
        page.set_page_queue(std::to_underlying(newer));
        moved += 1;
    }
    target.splice(target.end(), source);

    m_sizes[to].fetch_add(moved, ustl::sync::MemoryOrder::Relaxed);
    m_sizes[from].fetch_sub(moved, ustl::sync::MemoryOrder::Relaxed);
    m_lrugen.store(gen + 1, ustl::sync::MemoryOrder::Relaxed);
}

auto PageQueues::rotate_reclaimable_queues(AgeReason reason) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    auto const mru = m_mrugen.load(ustl::sync::MemoryOrder::Relaxed);
    auto const lru = m_lrugen.load(ustl::sync::MemoryOrder::Relaxed);

    // Generations [lru, mru] are live, so this is how many slots they occupy.
    // The subtraction is deliberately done on the free-running counters and
    // not on the wrapped slot indices: u32 arithmetic keeps the difference
    // right across the counters' own wrap, while two slot indices could not
    // tell "full" from "empty" apart at all.
    if (usize(mru - lru) + 1 == NUM_RECLAIMABLE) {
        // The ring is full, so the new generation has nowhere to go until the
        // oldest one is retired. In a kernel with somewhere to put pages this
        // is where reclaim happens -- Linux's inc_min_seq() evicts what it can
        // and only folds the rest forward. There is no swap, no pager and no
        // compression here, so everything folds forward: the oldest pages get
        // one generation younger than they deserve, which costs accuracy at
        // the far end of the ring and nothing else.
        advance_lru_locked();
    }

    m_mrugen.store(mru + 1, ustl::sync::MemoryOrder::Relaxed);

    m_last_age_time = time::SteadyClock::now();
    m_last_age_reason = reason;
}

} // namespace ours::mm