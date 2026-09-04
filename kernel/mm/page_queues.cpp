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

PageQueues::PageQueues() = default;

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

auto PageQueues::rotate_reclaimable_queues(AgeReason reason) -> void {
    {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};
        m_mrugen.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);

        m_last_age_time = time::SteadyClock::now();
        m_last_age_reason = reason;
    }
}

} // namespace ours::mm