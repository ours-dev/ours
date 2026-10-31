#include <ours/boot/options.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <atomic>
#include <utility>

#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours::mm {
/// Held through Arc<> by PageQueues, so it has to be reference counted.
///
/// Empty on purpose. The work splits across two threads: an MRU thread that
/// ages on a clock, and an LRU thread that retires the oldest generation when a
/// reclaimer asks to see pages older than it currently can. Only the first half
/// exists here, and the skeleton put run() on LruRunner, so that is where the
/// aging loop is for now. When a reclaimer arrives it takes the second half and
/// the loop below moves here, where it belongs by that split.
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

    explicit LruRunner(PageQueues *owner) : m_owner(owner), m_state(State::Running), m_thread() {}

    /// Starts the thread that runs run(). The runner outlives it: PageQueues
    /// holds the runner for the life of the node, and nothing stops either.
    auto start() -> Status;

    auto run() -> void;

    auto request_shutdown() -> void;

    auto state() const -> State {
        return m_state.load(std::memory_order_acquire);
    }

  private:
    PageQueues *m_owner;
    std::atomic<State> m_state;
    task::Thread *m_thread;
};

auto PageQueues::LruRunner::start() -> Status {
    // Thread::spawn() takes a plain function: this one finds the runner it
    // was handed and runs it.
    m_thread = task::Thread::spawn(
        "page-aging",
        [](void *const self) -> i32 {
            static_cast<Self *>(self)->run();
            return 0;
        },
        this, task::DEFAULT_PRIORITY);
    if (m_thread == nullptr) {
        return Status::OutOfMem;
    }
    auto _ = m_thread->detach();
    auto _ = m_thread->resume();
    return Status::Ok;
}

auto PageQueues::LruRunner::run() -> void {
    // A worker loop runs *until* it is shut down. What stood here --
    // `while (State::Shutdown == m_state.load(Relaxed)) {}` -- spun while the
    // runner was already shut down and left the instant it was not, which is
    // the condition inverted; and being an empty body it was not a loop that
    // did anything either way, just a CPU held at 100%.
    while (State::Running == m_state.load(std::memory_order_acquire)) {
        // Sleep to an instant rather than for an interval. The next rotation
        // is due one period after the last one actually happened, so this
        // cadence does not drift by however long a rotation took, and a
        // manual rotation pushes the next automatic one out instead of the
        // two landing on top of each other.
        auto const next = m_owner->next_rotation_due();
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

        // A manual rotation may have moved the deadline while this slept.
        if (time::SteadyClock::now() < m_owner->next_rotation_due()) {
            continue;
        }
        // Harvested first, so that a page used through a mapping in the
        // period that ends is in the newest generation when it becomes one
        // older, as one the kernel used is.
        auto _ = m_owner->harvest_accessed();
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

auto PageQueues::start_runners(Duration const period) -> Status {
    DEBUG_ASSERT(!m_lru_runner, "the page queues' thread started twice");
    {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};
        m_aging_period = period;
        // The first rotation one period from now, not at once: a page filed
        // during boot is as young as the clock says.
        m_last_age_time = time::SteadyClock::now();
    }

    auto *const runner = new (GAF_KERNEL) LruRunner(this);
    if (runner == nullptr) {
        return Status::OutOfMem;
    }
    m_lru_runner = ustl::Arc<LruRunner>::adopt(runner);
    return m_lru_runner->start();
}

auto PageQueues::rotate() -> void {
    auto _ = harvest_accessed();
    rotate_reclaimable_queues(AgeReason::Manual);
}

struct PageQueues::Candidate {
    ustl::Arc<VmCowPages> owner;
    PgOff index = 0;
    VmPage *page = nullptr;
};

auto PageQueues::adopt_first_locked(VmPage::List &list, Candidate *const out) -> bool {
    for (auto &page: list) {
        auto adopted =
            ustl::Arc<VmCowPages>::try_adopt(static_cast<VmCowPages *>(page.get_object()));
        if (!adopted) {
            continue;
        }
        out->owner = ustl::move(adopted);
        out->index = page.get_pgoff();
        out->page = &page;
        return true;
    }
    return false;
}

auto PageQueues::oldest_reclaimable(Candidate *const out) -> bool {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    // Generations [lru, mru] are live, the oldest first, and in each the page
    // filed first is first.
    auto const lru = m_lrugen.load(std::memory_order_relaxed);
    auto const mru = m_mrugen.load(std::memory_order_relaxed);
    for (auto gen = lru; gen != mru + 1; ++gen) {
        if (adopt_first_locked(m_queues[queue_index(reclaimable_queue(gen))], out)) {
            return true;
        }
    }
    return false;
}

auto PageQueues::oldest_zero_fork(Candidate *const out) -> bool {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return adopt_first_locked(m_queues[queue_index(QueueType::AnonymousZeroFork)], out);
}

auto PageQueues::reclaim(usize const target_frames) -> usize {
    usize freed = 0;
    // Each page is looked at once at most. What stays is not on the ring, bar
    // a page that should never be there, which reclaim_page() files as just
    // used; the bound does not rest on it.
    auto looks = count(PageKind::Reclaimable);
    while (freed < target_frames && looks != 0) {
        looks -= 1;
        Candidate candidate;
        if (!oldest_reclaimable(&candidate)) {
            break;
        }
        if (VmCowPages::ReclaimResult::Evicted ==
            candidate.owner->reclaim_page(candidate.index, candidate.page)) {
            freed += 1;
        }
        // |candidate| goes here, with no lock of this node's held: its owner
        // may be dying, and its destructor takes the queue lock.
    }
    return freed;
}

auto PageQueues::dedup_zero_pages(usize const target_frames) -> usize {
    usize freed = 0;
    // Each page once at most: one found not to be zero leaves the queue.
    auto looks = count(PageKind::AnonymousZeroFork);
    while (freed < target_frames && looks != 0) {
        looks -= 1;
        Candidate candidate;
        if (!oldest_zero_fork(&candidate)) {
            break;
        }
        if (VmCowPages::ReclaimResult::Evicted ==
            candidate.owner->dedup_zero_page(candidate.index, candidate.page)) {
            freed += 1;
        }
    }
    return freed;
}

auto PageQueues::harvest_accessed() -> usize {
    // The harvest takes a plain function: this one finds the queues it was
    // handed and asks them.
    auto const cleared = VmAspace::harvest_all_user_accessed(
        [](void *const self, PhysAddr const pa, usize const size) -> bool {
            return static_cast<Self *>(self)->take_accessed(pa, size);
        },
        this);
    log::trace("mm: page queues harvested {} accessed bits", cleared);
    return cleared;
}

/// Under the page table's lock, with the leaf present: the page cannot be
/// freed meanwhile, since its object unmaps it, under that lock, before it
/// frees it. So the descriptor is the page's, and mark_accessed() may read it.
///
/// A 2 MiB leaf maps one block, contiguous, so its pages are on one node: the
/// first says whose they are.
auto PageQueues::take_accessed(PhysAddr const pa, usize const size) -> bool {
    auto *const first = phys_to_frame(pa);
    if (first == get_zero_page()) {
        return false;
    }
    if (&PmNode::node(first->nid())->page_queues() != this) {
        return false;
    }
    for (usize offset = 0; offset < size; offset += PAGE_SIZE) {
        mark_accessed(phys_to_frame(pa + offset));
    }
    return true;
}

auto PageQueues::age_of(VmPage const &page) const -> usize {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};
    auto const type = QueueType(page.page_queue());
    DEBUG_ASSERT(type >= QueueType::ReclaimableStart && type <= QueueType::ReclaimableEnd,
                 "the age of a page that is not reclaimable");
    auto const slot = queue_index(type) - queue_index(QueueType::ReclaimableStart);
    auto const newest = m_mrugen.load(std::memory_order_relaxed) % NUM_RECLAIMABLE;
    return (newest + NUM_RECLAIMABLE - slot) % NUM_RECLAIMABLE;
}

auto PageQueues::next_rotation_due() const -> TimePoint<> {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};
    return m_last_age_time + m_aging_period;
}

auto to_string(PageQueues::AgeReason const reason) -> char const * {
    switch (reason) {
        case PageQueues::AgeReason::Timeout:
            return "timeout";
        case PageQueues::AgeReason::Manual:
            return "manual";
    }
    return "?";
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
    // platform has not brought up. start_runners() sets it when the clock is
    // there.
    m_last_age_time()
    , m_aging_period(Duration::zero()) {}

PageQueues::~PageQueues() = default;

auto PageQueues::set_pinned(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return set_queue_backlink_locked(page, object, pgoff, QueueType::Pinned);
}

auto PageQueues::set_anonymous(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return set_queue_backlink_locked(page, object, pgoff, QueueType::Anonymous);
}

auto PageQueues::set_anonymous_zero_fork(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return set_queue_backlink_locked(page, object, pgoff, QueueType::AnonymousZeroFork);
}

auto PageQueues::set_reclaimable(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    return set_queue_backlink_locked(page, object, pgoff, mru_queue());
}

auto PageQueues::set_owner(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};

    if (QueueType::None == QueueType(page->page_queue())) {
        return;
    }
    // Under the lock a reclaimer reads the backlink with: it must see the old
    // owner or the new one, never half of each.
    page->set_object(object);
    page->set_pgoff(pgoff);
}

auto PageQueues::kind_of(VmPage const &page) const -> PageKind {
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};
    auto const type = QueueType(page.page_queue());
    switch (type) {
        case QueueType::None:
            return PageKind::None;
        case QueueType::Anonymous:
            return PageKind::Anonymous;
        case QueueType::AnonymousZeroFork:
            return PageKind::AnonymousZeroFork;
        case QueueType::Pinned:
            return PageKind::Pinned;
        default:
            break;
    }
    DEBUG_ASSERT(type >= QueueType::ReclaimableStart && type <= QueueType::ReclaimableEnd,
                 "a page on queue {}, which does not exist", std::to_underlying(type));
    return PageKind::Reclaimable;
}

auto PageQueues::count(PageKind const kind) const -> usize {
    auto const size = [this](QueueType const type) {
        return m_sizes[queue_index(type)].load(std::memory_order_relaxed);
    };
    switch (kind) {
        case PageKind::None:
            return 0;
        case PageKind::Pinned:
            return size(QueueType::Pinned);
        case PageKind::Anonymous:
            return size(QueueType::Anonymous);
        case PageKind::AnonymousZeroFork:
            return size(QueueType::AnonymousZeroFork);
        case PageKind::Reclaimable:
            break;
    }
    usize total = 0;
    for (auto i = queue_index(QueueType::ReclaimableStart);
         i <= queue_index(QueueType::ReclaimableEnd); ++i) {
        total += m_sizes[i].load(std::memory_order_relaxed);
    }
    return total;
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

    // A real list move, not just a re-tag. The alternative writes the new
    // generation into the descriptor with a compare-exchange and leaves the
    // page on the old list, for the aging pass to reconcile the two later; that
    // buys a lock-free fast path at the price of a descriptor whose queue byte
    // deliberately disagrees with the list holding it. Everything here already
    // runs under m_lock, so there is nothing to buy, and the invariant that the
    // byte names the list is worth more -- it is what dequeue_locked() trusts
    // to decrement the right counter.
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
    // removal.
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
        // oldest one is retired. Once there is somewhere to put pages, this
        // is where reclaim happens: retiring a generation evicts what it can
        // and only folds the rest forward. There is no swap, no pager and no
        // compression here, so everything folds forward: the oldest pages get
        // one generation younger than they deserve, which costs accuracy at
        // the far end of the ring and nothing else.
        advance_lru_locked();
    }

    m_mrugen.store(mru + 1, std::memory_order_relaxed);

    m_last_age_time = time::SteadyClock::now();
    log::trace("mm: page queues aged ({}), generations {}..{}", to_string(reason),
               m_lrugen.load(std::memory_order_relaxed), mru + 1);
}

namespace {
    /// Starts every node's aging thread, once threads can be made.
    INIT_CODE
    auto start_page_aging() -> void {
        auto const seconds = boot::options().page_aging_seconds();
        auto const period = duration_cast<Duration>(Seconds(seconds));
        PmNode::for_each_node(PmNode::State::Online, [seconds, period](PmNode &node) {
            if (auto const status = node.page_queues().start_runners(period);
                Status::Ok != status) {
                log::error("mm: no page aging thread for node {}: {}", node.nid(),
                           to_string(status));
                return;
            }
            log::info("mm: node {} ages its pages every {} s", node.nid(), seconds);
        });
    }
    GKTL_INIT_HOOK(PageAging, start_page_aging, gktl::InitLevel::Threading + 1);
} // namespace

} // namespace ours::mm