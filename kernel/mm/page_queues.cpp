#include <ours/lockdep.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <atomic>
#include <utility>

#include <ustl/rc.hpp>

namespace ours::mm {
namespace {
    /// How often aging runs, when something runs it.
    ///
    /// Placeholders, and knowingly so: the right numbers come from watching a
    /// reclaimer decide badly because the generations were too coarse or too
    /// fine, and there is no reclaimer to watch. These are Zircon's defaults
    /// (kDefaultMinMruRotateTime / kDefaultMaxMruRotateTime), which is the
    /// only evidence available. The pair exists because aging wants a floor
    /// and a ceiling rather than a period: rotate no more often than the floor
    /// however much pressure there is, and no less often than the ceiling
    /// however little.
    constexpr auto DEFAULT_MIN_ROTATE_TIME = 10_s;
    constexpr auto DEFAULT_MAX_ROTATE_TIME = 60_s;
} // namespace

/// Held through Arc<> by PageQueues, so it has to be reference counted.
///
/// Empty on purpose. Zircon splits this work across two threads: an MRU thread
/// that ages on a clock, and an LRU thread that retires the oldest generation
/// when a reclaimer asks to see pages older than it currently can. Only the
/// first half exists here, and the skeleton put run() on LruRunner, so that is
/// where the aging loop is for now. When a reclaimer arrives it takes the
/// second half and the loop below moves here, where it belongs by that split.
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
    /// The three states a shutdown handshake needs, and no fewer.
    ///
    /// The pair this replaces was RequestShutdown and Shutdown, with nothing
    /// that meant "running" -- so whichever way the loop condition was written,
    /// one of the two states could never be the state the loop was actually in.
    /// Telling "someone has asked us to stop" from "the loop has left and is
    /// touching nothing" is the entire point of the handshake: only the second
    /// is safe to tear the queues down after.
    enum class State {
        Running,
        ShutdownRequested,
        Shutdown,
    };

    explicit LruRunner(PageQueues *owner) : m_owner(owner), m_state(State::Running) {}

    auto run() -> void;

    auto request_shutdown() -> void;

    auto state() const -> State {
        return m_state.load(std::memory_order_acquire);
    }

  private:
    PageQueues *m_owner;
    std::atomic<State> m_state;
    [[maybe_unused]]
    task::Thread *m_thread;
};

auto PageQueues::LruRunner::run() -> void {
    // A worker loop runs *until* it is shut down. What stood here --
    // `while (State::Shutdown == m_state.load(Relaxed)) {}` -- spun while the
    // runner was already shut down and left the instant it was not, which is
    // the condition inverted; and being an empty body it was not a loop that
    // did anything either way, just a CPU held at 100%.
    while (State::Running == m_state.load(std::memory_order_acquire)) {
        // Sleep to an instant rather than for an interval. The next rotation
        // is due one minimum period after the last one actually happened, so
        // this cadence does not drift by however long a rotation took, and a
        // manual rotation pushes the next automatic one out instead of the
        // two landing on top of each other.
        auto const next = m_owner->m_last_age_time + m_owner->m_min_rotate_time;
        // Whether it slept the whole way or was woken is read from m_state
        // below, not from the answer.
        auto _ =
            task::Thread::Current::sleep_until(time::Deadline::at(next), task::Interruptible::Yes);

        // Re-read rather than falling through to the loop condition: an
        // interruptible sleep is how a shutdown request cuts the wait short,
        // and one more rotation on the way out would be work against queues
        // the caller is already tearing down.
        if (State::Running != m_state.load(std::memory_order_acquire)) {
            break;
        }

        m_owner->rotate_reclaimable_queues(AgeReason::Timeout);
    }

    // Release, paired with the Acquire in state(): everything this loop did to
    // the queues happens-before an observer sees Shutdown, which is what makes
    // that state mean "nothing is still touching them".
    m_state.store(State::Shutdown, std::memory_order_release);
}

auto PageQueues::LruRunner::request_shutdown() -> void {
    // Only Running may be asked out of. Storing unconditionally would let a
    // second request overwrite the Shutdown the loop has already published,
    // un-telling a waiter something it has already been told.
    auto expected = State::Running;
    m_state.compare_exchange_strong(expected, State::ShutdownRequested, std::memory_order_acq_rel,
                                    std::memory_order_acquire);
}

auto PageQueues::start_runners() -> void {
    // Deliberately starts nothing, and the queues do not need it to: every
    // operation here is synchronous, and rotate_reclaimable_queues() can be
    // driven by whoever wants aging to happen.
    //
    // A thread would age a ring that is always empty. Nothing files a page on
    // these queues yet -- that waits on VmCowPages becoming a real page store
    // -- and nothing reads a generation back, because there is no reclaimer:
    // no swap, no pager, no compression, so there is nowhere for a reclaimed
    // page to go. Aging exists to rank pages for a reclaimer to choose from,
    // and a ranking with no consumer is a periodic wakeup for nothing.
    //
    // What has to be true before this starts the loop above is written down in
    // docs/repairs/260903/mm.md.
}

/// The two generation counters have to start equal: `mru - lru + 1` is the
/// number of occupied slots, and every decision about the ring is made from
/// it, so they are spelled out here rather than left to the default.
///
/// This used to be load-bearing in a stronger sense: ustl::sync::Atomic's
/// default constructor was `= default`, which left its storage indeterminate,
/// and a PageQueues outside .bss would have started out claiming an arbitrary
/// number of live generations. std::atomic value-initialises since C++20, so
/// the explicit zeros now say what the code means rather than repairing the
/// type.
PageQueues::PageQueues()
    : m_lrugen(0)
    , m_mrugen(0)
    ,
    // Value-initialised, i.e. the clock's epoch, not "now": this runs from
    // PmNode's constructor, which is far too early to read a clock that the
    // platform has not brought up. The effect is that the first rotation
    // deadline is already in the past, so a runner that does start rotates
    // once straight away and settles into the cadence from there.
    m_last_age_time()
    , m_last_age_reason(AgeReason::Manual)
    , m_min_rotate_time(duration_cast<Duration>(DEFAULT_MIN_ROTATE_TIME))
    , m_max_rotate_time(duration_cast<Duration>(DEFAULT_MAX_ROTATE_TIME)) {}

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
    m_sizes[index].fetch_add(1, std::memory_order_relaxed);
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
    m_sizes[queue_index(type)].fetch_sub(1, std::memory_order_relaxed);
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
    auto const gen = m_lrugen.load(std::memory_order_relaxed);
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

    m_sizes[to].fetch_add(moved, std::memory_order_relaxed);
    m_sizes[from].fetch_sub(moved, std::memory_order_relaxed);
    m_lrugen.store(gen + 1, std::memory_order_relaxed);
}

auto PageQueues::rotate_reclaimable_queues(AgeReason reason) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    auto const mru = m_mrugen.load(std::memory_order_relaxed);
    auto const lru = m_lrugen.load(std::memory_order_relaxed);

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

    m_mrugen.store(mru + 1, std::memory_order_relaxed);

    m_last_age_time = time::SteadyClock::now();
    m_last_age_reason = reason;
}

} // namespace ours::mm