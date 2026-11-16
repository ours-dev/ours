#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/priv/alloc_failure.hpp>
#include <ours/mm/priv/pmm.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/timer.hpp>

#include <algorithm>
#include <array>
#include <atomic>

#include <gktl/counter.hpp>
#include <gktl/defer.hpp>
#include <logz4/log.hpp>
#include <numa/topology.hpp>

namespace ours::mm {
namespace priv {
    // These carry the names pmm.hpp declares and init_pmm.cpp assigns. They
    // were left behind under their old g_-prefixed spelling when the header
    // was renamed, so the definitions the whole tree refers to did not exist.
    extern "C" {
    PhysAddr MAX_PHYS_ADDR;
    PhysAddr MIN_PHYS_ADDR;
    }
} // namespace priv

auto approx_free_frame_count() -> usize {
    usize total = 0;
    PmZone::for_each([&total](PmZone &zone) { total += zone.num_free_frames(); });
    return total;
}

auto zone_floor_frame_count() -> usize {
    // A zone refusing a frame holds at most its floor, and never more than it
    // has: a small zone whose reserve is above its size (the Dma zone of a
    // large machine) refuses with every frame it has free. Present frames,
    // not managed: managed grows after boot as reserved frames are given back
    // (the .init sections, the boot image), and a sum taken from it at boot
    // would fall short of the one later. Present is fixed and above both.
    usize total = 0;
    PmZone::for_each([&total](PmZone const &zone) {
        total += std::min(zone.normal_low_floor(), zone.present_frames());
    });
    return total;
}

namespace {
    /// The gate in front of the allocations that can wait (alloc_frames()
    /// with Gaf::CanWait), and the signal for whoever watches free memory
    /// (set_free_memory_signal()). The mm:: functions own admission, and
    /// observe_pressure() below samples the watcher's bounds once at the end
    /// of each of their allocations or returns. PmNode and PmZone never reach
    /// it.
    ///
    /// Every allocation and free asks it something, so what they read takes
    /// no lock: atomics, each loaded on its own. The event that wakes waiters
    /// is touched only by waiters and by the watcher, and the watcher's lock
    /// by the watcher and by the rare allocation that shuts the gate.
    class FreeMemoryGate {
      public:
        /// Whether a CanWait allocation may go ahead: Ok, ShouldWait while the
        /// gate is shut, or OutOfMem while delayed allocations are refused.
        auto admit() -> Status;

        /// After every allocation and free: signals the watcher's event, once,
        /// when free memory has left its bounds. One load when nothing is armed.
        auto check_signal() -> void;

        /// Sleeps until the gate opens, or turns to refusing: Ok, or the
        /// status of a sleep that ended otherwise.
        auto wait_for_opening(time::Deadline const &deadline, task::Interruptible interruptible)
            -> Status;

        /// An opening, or a refusal, passed on to the next waiter.
        auto pass_on() -> void {
            m_may_allocate.signal();
        }

        /// What set_free_memory_signal() does; see there.
        auto set(usize lower, usize upper, usize delay_level, sync::Event *event,
                 DelayedAllocations delayed) -> bool;

      private:
        /// admit()'s slow path, for an allocation that found free memory
        /// below the level with the gate open: decides again under the
        /// watcher's lock, and shuts the gate if that still holds.
        auto shut() -> Status;

        enum class State : u8 {
            /// A CanWait allocation that finds free memory below the delay
            /// level shuts the gate.
            Open,
            /// Every CanWait allocation answers ShouldWait, whatever free
            /// memory is now, until the watcher opens the gate.
            Shut,
            /// A CanWait allocation that finds free memory below the delay
            /// level fails, and a waiter wakes with OutOfMem.
            Refusing,
        };

        std::atomic<State> m_state{State::Open};
        std::atomic<usize> m_delay_level{0};

        /// The watcher's bounds, in frames, and its event, published last: an
        /// event read with acquire comes with bounds at least as new as the
        /// ones it was armed with. Null when nothing is armed, which is also
        /// after it has been signalled once.
        std::atomic<usize> m_signal_lower{0};
        std::atomic<usize> m_signal_upper{0};
        std::atomic<sync::Event *> m_signal_event{nullptr};

        /// Wakes the waiters, one per signal.
        sync::Event m_may_allocate{sync::Event::Options::AutoUnsignal};

        /// Orders set() and shut(): every write of m_state is made under it.
        /// So a shut is decided against the delay level set() last stored,
        /// and set() cannot land between the decision and the shut. Only the
        /// allocation that shuts the gate takes it on the allocation side,
        /// which is the rare path; the reads stay lock-free.
        sync::SpinLock m_watcher_lock;
    };

    /// Built with the other global objects, the first thing start_kernel()
    /// does, so before the first frame is allocated.
    FreeMemoryGate GATE;

    auto FreeMemoryGate::admit() -> Status {
        auto const state = m_state.load(std::memory_order_acquire);
        if (state == State::Shut) {
            return Status::ShouldWait;
        }
        auto const level = m_delay_level.load(std::memory_order_relaxed);
        if (level == 0 || approx_free_frame_count() >= level) {
            return Status::Ok;
        }
        if (state == State::Refusing) {
            return Status::OutOfMem;
        }
        return shut();
    }

    auto FreeMemoryGate::shut() -> Status {
        // The reads above were each a moment apart, and a watcher may have
        // armed the gate again since: with a lower level, or open again with
        // the same Open value, which a compare-and-swap on the state alone
        // cannot tell from the one it read. A gate shut on such stale reads
        // stays shut with free memory inside the watcher's bounds, where
        // nothing signals it and so nothing opens it. Under the lock set()
        // writes under, the state, the level and the free count read here
        // are the ones in force when the gate shuts.
        sync::Event *armed = nullptr;
        {
            Guard<sync::SpinLock, IrqSave> guard{m_watcher_lock};
            auto const state = m_state.load(std::memory_order_relaxed);
            if (state == State::Shut) {
                return Status::ShouldWait;
            }
            auto const level = m_delay_level.load(std::memory_order_relaxed);
            if (level == 0 || approx_free_frame_count() >= level) {
                return Status::Ok;
            }
            if (state == State::Refusing) {
                return Status::OutOfMem;
            }
            m_state.store(State::Shut, std::memory_order_release);
            // The watcher is told of every shut, whatever free memory does
            // next. Not through check_signal(), which looks at the bounds
            // again: frees can bring free memory back inside them before it
            // looks (at Critical the lower bound is the delay level itself),
            // and then nobody signals, and the gate stays shut on a count
            // the watcher would have opened it at. Taken under the lock, so
            // the event is the one in force at the shut; a set() after this
            // arms a new one and opens the gate itself.
            armed = m_signal_event.exchange(nullptr, std::memory_order_acq_rel);
        }
        // Outside the lock, as every signal is. With no event armed (a test
        // driving the gate by hand) nothing is told, and the next set()
        // opens the gate.
        if (armed) {
            armed->signal();
        }
        return Status::ShouldWait;
    }

    auto FreeMemoryGate::check_signal() -> void {
        if (m_signal_event.load(std::memory_order_acquire) == nullptr) {
            return;
        }
        auto const free = approx_free_frame_count();
        if (free >= m_signal_lower.load(std::memory_order_relaxed) &&
            free <= m_signal_upper.load(std::memory_order_relaxed)) {
            return;
        }
        // Two CPUs may both see the bounds left; the one that takes the event
        // signals it, and the other finds it gone.
        if (auto *const event = m_signal_event.exchange(nullptr, std::memory_order_acq_rel)) {
            event->signal();
        }
    }

    auto FreeMemoryGate::wait_for_opening(time::Deadline const &deadline,
                                          task::Interruptible const interruptible) -> Status {
        return m_may_allocate.wait(deadline, interruptible);
    }

    auto FreeMemoryGate::set(usize const lower, usize const upper, usize const delay_level,
                             sync::Event *const event, DelayedAllocations const delayed) -> bool {
        auto state = State::Open;
        if (delayed == DelayedAllocations::Refuse) {
            state = State::Refusing;
        }
        {
            Guard<sync::SpinLock, IrqSave> guard{m_watcher_lock};
            if (event) {
                auto const free = approx_free_frame_count();
                if (free < lower || free > upper) {
                    return false;
                }
            }
            // Disarmed while the bounds change. A CPU that read the old event
            // just before may still compare free memory with the new bounds and
            // signal the old event once more; that only makes the watcher look
            // again.
            m_signal_event.store(nullptr, std::memory_order_relaxed);
            m_signal_lower.store(lower, std::memory_order_relaxed);
            m_signal_upper.store(upper, std::memory_order_relaxed);
            m_delay_level.store(delay_level, std::memory_order_relaxed);
            m_state.store(state, std::memory_order_release);
            m_signal_event.store(event, std::memory_order_release);
        }
        // Outside the lock: a signal wakes a thread, and the scheduler's lock
        // is not one to take inside a spinlock of the PMM's. The first waiter
        // retries, or is refused, and passes it on.
        pass_on();
        return true;
    }

} // namespace

namespace {
    GKTL_COUNTER(PM_NODE_LOCAL_HIT, "pm.node.local.hit");
    GKTL_COUNTER(PM_NODE_LOCAL_MISS, "pm.node.local.miss");

    /// Releases that brought a zone back to admitting a frame at its Low
    /// watermark, and so woke a waiter. For tests: what a waiter put off at a zone's Low is woken by.
    GKTL_COUNTER(PM_ZONE_RELIEF, "pm.zone.relief");

    /// The gate every block passes on its way out of stock, with no zone lock
    /// held: what makes a block the buddy gave into the block the caller asked
    /// for. The zone moved it and the node chose the zone; neither prepares
    /// it.
    auto prepare_alloc(PmFrame &frame, AllocRequest const &request) -> void {
        // A pin says a frame stays where it is, and this one was on a free
        // list a moment ago. Every mm:: path into stock asserts the same in
        // prepare_free(), under the same switch, so an owner freeing a pinned
        // frame is stopped there. What this catches is a count that never
        // passed prepare_free(): a frame given to PmNode::free_frames() or
        // PmZone::free_frames() directly, a frame pinned while it was free,
        // or descriptor storage that was not zeroed (init() does not write the
        // count). A release build has neither check.
        DEBUG_ASSERT(frame.pin_count() == 0, "frame {:#x} leaves a free list with {} pin(s)",
                     frame_to_phys(&frame), frame.pin_count());

        // The block's layout: its order on the head and each tail's distance
        // from it, so that any frame of the block leads back to the head and
        // only the head can be freed (prepare_free()).
        frame.make_block(request.order());

        if (request.zeroes()) {
            std::fill_n(frame_to_virt<isize>(&frame),
                        (usize{1} << request.order()) * PAGE_SIZE / sizeof(isize), 0);
        }

        // Counted for the node that gave the block, against the node the
        // asking CPU is on: a block served from elsewhere is a miss whichever
        // home the walk started from.
        if (frame.nid() == CpuNodeMap::node_of(request.initiator())) {
            PM_NODE_LOCAL_HIT.add(1);
        } else {
            PM_NODE_LOCAL_MISS.add(1);
        }
    }

    /// The gate every block passes on its way into stock, before any lock is
    /// taken: what must be true of a frame for the buddy to hold it.
    auto prepare_free(PmFrame &frame) -> void {
        // The runtime half of the guard in early_mem.cpp's
        // free_frames_phys_range(): a reserved frame reaches the buddy only
        // through unwire_frame(). Nothing else on this path looks at the frame,
        // so without this a reserved frame would be handed out again still
        // flagged and still counted.
        DEBUG_ASSERT(!(frame.flags() & page::Flags::Reserved),
                     "frame {:#x} is reserved; unwire_frame() it before freeing",
                     frame_to_phys(&frame));
        DEBUG_ASSERT(!frame.is_role(page::Role::Vmm) || 0 == frame.page_queue(),
                     "frame {:#x} is freed while still on a page queue", frame_to_phys(&frame));
        // Whatever its role: a pin says the frame stays where it is, and a
        // frame on a free list is about to be someone else's.
        DEBUG_ASSERT(frame.pin_count() == 0, "frame {:#x} is freed while {} pin(s) hold it",
                     frame_to_phys(&frame), frame.pin_count());
        // Only a block's head may be freed: a frame inside a block would be
        // freed at whatever order it last held, while the head still owns it,
        // and the frames would have two owners. split_block() first, to free
        // the parts of a block apart.
        DEBUG_ASSERT(frame.is_head(), "frame {:#x} is frame {} of a block, not its head",
                     frame_to_phys(&frame), frame.compound_offset());
        frame.clear_block();
    }

    /// Looks at free memory once, after a whole allocation or return and with
    /// no zone lock held: signals the watcher when its bounds were left, and
    /// asks the reclaimer for the node of a zone an allocation found short.
    auto observe_pressure(ustl::Option<PmZone const &> depleted_zone = ustl::NONE) -> void {
        GATE.check_signal();
        if (depleted_zone) {
            MemoryReclaimer::get().request(*depleted_zone);
        }
    }

    auto apply_thread_policy(Gaf &alloc_flags) -> Status {
        if constexpr (MEMORY_ALLOCATION_POLICY) {
            if (!!(alloc_flags & Gaf::ThisNode)) {
                return Status::Ok;
            }
            using Kind = AllocPolicy::Kind;

            // The thread's resource lock covers reading the policy and nothing
            // more: it is what another thread takes to replace it. It used to be
            // held by every node-less allocation entry around the whole
            // allocation, slow path included -- a spin lock with interrupts off,
            // held across a path that could reschedule(). Copying the Arc out is
            // all that needs it.
            auto const policy = [] {
                auto *const thread = task::Thread::Current::get();
                Guard<sync::SpinLock, IrqSave> guard{thread->get_resource_lock()};
                return thread->alloc_state().policy();
            }();
            if (policy) {
                switch (policy->kind()) {
                    case Kind::Bind:
                        break;
                    case Kind::Interleave:
                        break;
                    case Kind::Preferred:
                        break;
                    default:
                        break;
                }
            }
        }

        return Status::Ok;
    }

    /// The node an allocation starts from: the one named, or the calling
    /// CPU's once the thread's placement policy has had its say. An id outside
    /// the node table names no node, which is how a caller says it has no
    /// preference.
    auto resolve_home(NodeId nid, Gaf &alloc_flags) -> ktl::Result<PmNode *> {
        if (nid < MAX_NODES) {
            if (auto *const node = find_node(nid)) {
                return ustl::ok(node);
            }
            return ustl::err(Status::InvArgs);
        }

        if (auto const status = apply_thread_policy(alloc_flags); Status::Ok != status) {
            return ustl::err(status);
        }
        auto *const node = CpuNodeMap::local_node();
        if (!node) {
            return ustl::err(Status::BadState);
        }
        return ustl::ok(node);
    }

    /// One block for the calling thread out of |home|'s walk, with pressure
    /// not looked at: the list forms take several and look once. When the
    /// first pass fails as a whole, |depleted| is set to the first zone it
    /// found short, if it was not set already, and stays set whether or not
    /// the second pass then brings a block back: an allocation served by the
    /// second pass was served below Low, and reclaim is wanted then too. A
    /// first pass that succeeds names nothing, though it may have passed over
    /// a short zone on the way to one that admitted the request.
    ///
    /// A request that can wait (AllocRequest::may_wait()) asks the free-memory
    /// gate before any zone, after the refusals that are about the request
    /// itself (a thread that disabled allocation, an order the buddy does not
    /// have, a home that does not exist), which it gets first. Unless it is
    /// confined, it then makes the first pass only
    /// (AllocRequest::waits_at_low()): when no zone admits it at Low it is
    /// answered ShouldWait, and waits for a zone to rise back above Low,
    /// instead of spending the band between Low and Min that the requests
    /// which cannot wait depend on. A confined one makes both passes, as a
    /// request that cannot wait does.
    auto alloc_block(PmNode &home, Gaf const alloc_flags, usize const order, NodeMask const &nodes,
                     ustl::Option<PmZone const &> &depleted) -> ktl::Result<PmFrame *> {
        // Whether the calling thread may allocate at all: a thread that has
        // disabled allocation (AllocState::disable()) is refused here,
        // whichever way it asked. This is the one function every frame
        // allocation reaches -- the node-less entries, the ones naming a node,
        // and the bulk and fragment loops once per step -- so the rule cannot
        // be walked around.
        //
        // It is a different question from where the frame comes from, and only
        // that one depends on whether the caller named a node. So this is
        // asked on every path, node named or not, and the placement policy
        // only on the node-less ones (resolve_home()).
        //
        // Thread::Current is always there by now: task::init_early() makes the
        // boot thread current before the PMM exists.
        if (!task::Thread::Current::alloc_state().is_enabled()) {
            return ustl::err(Status::AccessDenied);
        }

        // Before the request is built: a resolved request never carries an
        // order the buddy does not have.
        if (order > MAX_FRAME_ORDER) {
            return ustl::err(Status::InvArgs);
        }

        auto const request = resolve_request(
            alloc_flags, order, nodes, AllocRequest::Origin{.node = home.nid(), .cpu = Cpu::id()});

        // The gate first, for a request that can wait: it decides for the
        // whole machine whether such a request may go ahead now, before any
        // zone is asked.
        if (request.may_wait()) {
            if (auto const status = GATE.admit(); Status::Ok != status) {
                return ustl::err(status);
            }
        }

        // Two passes. The first holds the request at every zone's Low
        // watermark. Only when no zone admits it there is reclaim asked for,
        // for the first zone the pass found short, and the second pass lets
        // the request down to Min (or below, for a caller that cannot block):
        // the band between Low and Min is spent only after reclaim has been
        // asked for.
        auto block = home.alloc_frames(request, AdmissionPass::Low);
        if (!block) {
            if (!depleted) {
                depleted = block.error().depleted_zone;
            }
            // An ordinary waiting request waits here rather than spend the
            // band. A confined one does not: the gate and the watchdog count
            // memory it cannot use, so nothing is bound to end its wait, and
            // it goes on to Min and fails there like one that cannot wait.
            if (request.waits_at_low()) {
                return ustl::err(Status::ShouldWait);
            }
            block = home.alloc_frames(request, AdmissionPass::Min);
        }
        if (!block) {
            return ustl::err(block.error().status);
        }

        prepare_alloc(**block, request);
        return ustl::ok(*block);
    }

    /// One block back to the node that owns it, with pressure not looked at.
    auto free_block(PmFrame &frame, usize const order) -> void {
        prepare_free(frame);

        // A frame goes back to the node that owns it, which the frame names,
        // and not to the node of whoever is freeing it.
        auto *const node = find_node(frame.nid());
        DEBUG_ASSERT(node, "frame {:#x} belongs to node {}, which does not exist",
                     frame_to_phys(&frame), frame.nid());

        // A release that brings the zone back to admitting a frame at its
        // Low watermark wakes one waiter: a waiting request put off at Low
        // (alloc_block()) may now be served. Only at that crossing, not on
        // every release, so a zone above Low wakes nobody. The zone tells it
        // from the count its own atomic increment found, so of releases
        // racing each other and allocations, exactly the one that crossed
        // wakes. A waiter that retries and is beaten to the frame waits
        // again, for the next crossing.
        if (node->free_frames(frame, order)) {
            PM_ZONE_RELIEF.add(1);
            GATE.pass_on();
        }
    }

    auto free_blocks(PmFrame::List &list) -> void {
        list.clear_and_dispose([](PmFrame *frame) noexcept { free_block(*frame, frame->order()); });
    }
} // namespace

auto node_work_cpus(NodeId nid) -> CpuMask {
    DEBUG_ASSERT(nid < MAX_NODES);
    if (auto node = find_node(nid)) {
        return node->work_cpus();
    }
    return CpuMask{};
}

auto node_hard_affinity(NodeId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < MAX_NODES);
    if (auto node = find_node(nid)) {
        return &node->hard_affinity();
    }
    return nullptr;
}

auto node_soft_affinity(NodeId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < MAX_NODES);
    if (auto node = find_node(nid)) {
        return &node->soft_affinity();
    }
    return nullptr;
}

/// A zeroed page for one level of a page table, from whichever allocator is up.
/// See the declaration in pmm.hpp for why the choice lives here.
auto alloc_page_table(usize size, usize alignment, NodeId node) -> PhysAddr {
    if (pmm_enabled()) {
        // With |node| as the home, so that `node` means the same thing
        // on both sides of the handover. It used to be read only by the EarlyMem
        // branch below, which made "a table on node N" honoured before the
        // handover and silently ignored after it -- the caller's argument
        // dropped in favour of whatever node the running CPU belongs to.
        //
        // Which node a page table sits on is not cosmetic under NUMA: the CPU
        // that walks it is, by construction, one of the CPUs of the node whose
        // memory it maps, and a table walk is a chain of dependent loads that
        // the hardware cannot overlap.
        //
        // bootmem::ANY_NODE is >= MAX_NODES, which names no node, so "no
        // preference" -- every in-tree caller today -- starts on the calling
        // CPU's node, the thread's NUMA policy included (applied by
        // resolve_home()). The AllocState counter applies either way:
        // alloc_block() checks it for every caller.
        //
        // Through the pipeline under mm::alloc_frames(), not the entry itself:
        // a page table is the page-table failure site's
        // (test::AllocationSite::PageTable, asked in PageTable::allocate_table())
        // and must not also count as a frame asked for with no home.
        auto const result = priv::alloc_frames(
            ALLOC_KERNEL | Gaf::Zero, get_order(size / PAGE_SIZE), node, node_online_mask());
        if (!result) {
            return PhysAddr{0};
        }
        auto *const frame = *result;
        // Mmu, as the tables physboot built are (stamp_reserved_frames()), so
        // that a page-table page says what it is whichever allocator made it;
        // with its body cleared, so that it starts with no tables set aside
        // (MmuBody).
        frame->become<page::MmuBody>();
        return frame_to_phys(frame);
    }

    // Zeroed by hand: the boot allocator has no notion of Gaf, and a page table
    // whose unused entries are leftover bytes is a set of mappings nobody made.
    auto *table = EarlyMem::allocate<u8>(size, alignment, node, bootmem::MemoryType::PageTable);
    if (!table) {
        return PhysAddr{0};
    }
    std::fill_n(table, size, u8(0));

    // EarlyMem hands back a physmap address, which is what virt_to_phys inverts.
    return PhysMap::virt_to_phys(table);
}

auto free_page_table(PhysAddr base, usize size) -> void {
    // Nothing to do before the buddy: see the header. Freeing an EarlyMem
    // allocation would hand the boot allocator a range it never considered
    // free, and after do_handoff() there is no boot allocator left to tell.
    if (!pmm_enabled()) {
        return;
    }
    // Handed back still Mmu, as every caller of free_frames() hands frames
    // back in the role they were used in; only the buddy's insert may say
    // Pmm. Anything else here is a frame that was never a page table, which
    // the assertion below catches.
    auto *const frame = phys_to_frame(base);
    DEBUG_ASSERT(frame->is_role(page::Role::Mmu),
                 "freeing {:#x} as a page table, but its role is {}", base,
                 to_string(frame->role()));
    // A table that still has tables set aside for its large leaves would take
    // them with it, never to be freed: its owner gives them back first
    // (page_tables::X86PageTableImpl::free_tables(), through
    // discard_all_tables()).
    DEBUG_ASSERT(frame->as<page::MmuBody>().deposits == nullptr,
                 "freeing the page table {:#x} with tables still set aside for it", base);
    DEBUG_ASSERT(get_order(size / PAGE_SIZE) == frame->order(),
                 "page table {:#x} of {} bytes was allocated as a block of order {}", base, size,
                 frame->order());
    free_frames(frame);
}

auto free_frame_count() -> usize {
    usize total = 0;
    PmZone::for_each([&total](PmZone &zone) { total += zone.count_free_frames(); });
    return total;
}

auto wait_for_frame_allocation(Gaf const alloc_flags, time::Deadline const &deadline,
                               task::Interruptible const interruptible) -> ktl::Result<PmFrame *> {
    DEBUG_ASSERT(!!(alloc_flags & Gaf::CanWait), "a wait for a frame by a caller that cannot wait");
    if (auto const status = GATE.wait_for_opening(deadline, interruptible); Status::Ok != status) {
        return ustl::err(status);
    }

    auto result = alloc_frames(alloc_flags);
    // The opening goes on to the next waiter, unless this one was put off
    // again, by the gate or at a zone's Low: then the next opening, or the
    // next release that brings a zone back above Low, wakes them. A refusal goes
    // on too, so that every waiter hears it.
    if (result || Status::ShouldWait != result.unwrap_err()) {
        GATE.pass_on();
    }
    return result;
}

auto set_free_memory_signal(usize const lower, usize const upper, usize const delay_level,
                            sync::Event *const event, DelayedAllocations const delayed) -> bool {
    DEBUG_ASSERT(lower <= upper, "free-memory bounds {}..{} the wrong way round", lower, upper);
    DEBUG_ASSERT(!event || delayed == DelayedAllocations::Refuse || delay_level <= lower,
                 "a delay level {} above the lower bound {}: the gate could shut unsignalled",
                 delay_level, lower);
    return GATE.set(lower, upper, delay_level, event, delayed);
}

/// The node the calling CPU belongs to. Called from kmalloc and
/// MemCache::allocate to resolve "no preference" into a real node, and from
/// everywhere in mm that used to ask the topology directly.
///
/// Read out of the placement table rather than through the cached pointer, and
/// not as an optimisation. That cache is a per-CPU `PmNode *`, so
/// `CURRENT_INSTANCE->m_id` dereferences the *template* copy of that pointer
/// at compile time -- it folds to `&(nullptr->m_id)`, i.e. the constant 4,
/// which then gets handed to %gs as if it were a per-CPU handle. That is a
/// wild access whichever model is in effect, and it stays wrong after the
/// pointer is set, because the fold already happened.
///
/// The table is also the only answer that works this early: nothing fills the
/// per-CPU pointer until CpuNodeMap::publish_all() runs from an init hook, while
/// allocation starts asking for a node long before that.
///
/// The table is mm's own (CpuNodeMap), seeded once at bring-up from what
/// the firmware declared. Nothing on this path asks kernel/lib/numa anything.
auto current_node() -> NodeId {
    return CpuNodeMap::node_of(CpuLocal::current_cpu());
}

namespace priv {
    auto alloc_frames(Gaf alloc_flags, usize const order, NodeId const home, NodeMask const &nodes)
        -> ktl::Result<PmFrame *> {
        // The flags go down whole: the request built from them answers what
        // they mean, the gate included (alloc_block()).
        auto const node = resolve_home(home, alloc_flags);
        if (!node) {
            observe_pressure();
            return ustl::err(node.unwrap_err());
        }

        ustl::Option<PmZone const &> depleted;
        auto const block = alloc_block(**node, alloc_flags, order, nodes, depleted);
        observe_pressure(depleted);
        return block;
    }

    auto alloc_frames_bulk(Gaf alloc_flags, usize const n, NodeId const home, NodeMask const &nodes)
        -> ktl::Result<PmFrame::List> {
        DEBUG_ASSERT(!(alloc_flags & Gaf::CanWait), "batch allocation is immediate");

        ustl::Option<PmZone const &> depleted;
        auto observation = gktl::defer([&depleted] { observe_pressure(depleted); });
        auto const node = resolve_home(home, alloc_flags);
        if (!node) {
            log::trace("Failed to allocate {} frames with reason: {}", n,
                       to_string(node.unwrap_err()));
            return ustl::err(node.unwrap_err());
        }

        PmFrame::List list;
        auto rollback = gktl::defer([&list] { free_blocks(list); });
        for (usize i = 0; i < n; ++i) {
            auto block = alloc_block(**node, alloc_flags, 0, nodes, depleted);
            if (!block) {
                log::trace("Failed to allocate {} frames with reason: {}", n,
                           to_string(block.unwrap_err()));
                return ustl::err(block.unwrap_err());
            }
            list.push_back(*block.unwrap());
        }

        rollback.cancel();
        return ustl::ok(ustl::move(list));
    }

    auto alloc_frames_fragment(Gaf alloc_flags, usize n, NodeId const home, NodeMask const &nodes)
        -> ktl::Result<PmFrame::List> {
        DEBUG_ASSERT(!(alloc_flags & Gaf::CanWait), "fragment allocation is immediate");

        ustl::Option<PmZone const &> depleted;
        auto observation = gktl::defer([&depleted] { observe_pressure(depleted); });
        auto const node = resolve_home(home, alloc_flags);
        if (!node) {
            log::trace("Failed to allocate frame with reason: {}", to_string(node.unwrap_err()));
            return ustl::err(node.unwrap_err());
        }

        PmFrame::List list;
        auto rollback = gktl::defer([&list] { free_blocks(list); });
        // One bit of |n| per step, whether or not it is set. The shift used to
        // sit inside the branch that allocates, so the first clear bit was
        // never shifted out: `continue` bumped the order and tested the same
        // bit again, for ever. Any even |n| hung here -- n = 2 on its first
        // step.
        for (usize order = 0; n > 0; order += 1, n >>= 1) {
            if ((n & 1) == 0) {
                continue;
            }

            auto block = alloc_block(**node, alloc_flags, order, nodes, depleted);
            if (!block) {
                log::trace("Failed to allocate frame with reason: {}",
                           to_string(block.unwrap_err()));
                return ustl::err(block.unwrap_err());
            }
            list.push_back(*block.unwrap());
        }

        rollback.cancel();
        return ustl::ok(ustl::move(list));
    }
} // namespace priv

auto alloc_frames(Gaf const flags, usize const order, NodeId const home) -> ktl::Result<PmFrame *> {
#if MM_TEST_ALLOC_FAILURE
    // The frame site is a frame or block a caller asked for with no home
    // named, through this entry. A page table is not one: it has a site of
    // its own, and alloc_page_table() goes to the pipeline underneath this
    // so that one injected failure is not counted twice.
    if (home >= MAX_NODES && test::AllocationFailure::hit(test::AllocationSite::Frame)) {
        return ustl::err(Status::OutOfMem);
    }
#endif
    return priv::alloc_frames(flags, order, home, node_online_mask());
}

auto alloc_frames_bulk(Gaf const flags, usize const n, NodeId const home)
    -> ktl::Result<PmFrame::List> {
    return priv::alloc_frames_bulk(flags, n, home, node_online_mask());
}

auto alloc_frames_fragment(Gaf const flags, usize const n, NodeId const home)
    -> ktl::Result<PmFrame::List> {
    return priv::alloc_frames_fragment(flags, n, home, node_online_mask());
}

auto alloc_frames_contiguous(Gaf const flags, usize const n, usize const align_log2,
                             NodeId const home) -> ktl::Result<PmFrame::List> {
    DEBUG_ASSERT(!(flags & Gaf::CanWait), "contiguous allocation is immediate");
    if (n == 0) {
        return ustl::err(Status::InvArgs);
    }

    // A block is aligned to its own size, so one big enough for the alignment
    // is aligned as asked: the order is the larger of what the count and the
    // alignment need. An order no block has is refused by alloc_frames().
    usize align_order = 0;
    if (align_log2 > PAGE_SHIFT) {
        align_order = align_log2 - PAGE_SHIFT;
    }
    auto const order = std::max(get_order(n), align_order);

    auto const allocated = alloc_frames(flags, order, home);
    if (!allocated) {
        return ustl::err(allocated.unwrap_err());
    }
    auto *const block = *allocated;

    // Each frame is its own from here, as if it had been allocated alone. The
    // first |n| are the run; the rest of the block goes back now, one frame at
    // a time.
    split_block(block);
    PmFrame::List run;
    for (usize index = 0; index < n; ++index) {
        run.push_back(block[index]);
    }
    auto const nr_frames = usize{1} << order;
    for (usize index = n; index < nr_frames; ++index) {
        free_frames(&block[index]);
    }
    return ustl::ok(ustl::move(run));
}

auto free_frames(PmFrame *frame) -> void {
    DEBUG_ASSERT(frame, "free requires a frame");
    // A block is returned whole, as it was handed out, and the head records
    // the order the buddy gave it. A range of memory that was never a block
    // comes in through adopt_frames(), which is told the range; a single frame
    // that was never a block may come here, as order 0, which is what init()
    // leaves in its head (the bootstrap trampoline's buffer, the boot image's
    // pages once their VMO lets them go).
    auto const order = frame->order();
    log::trace("Free frame({:X}, {})", frame_to_phys(frame), order);
    free_block(*frame, order);
    observe_pressure();
}

auto adopt_frames(Pfn start, Pfn const end) -> void {
    while (start < end) {
        // Two separate limits, and the alignment one is not optional. A buddy
        // block of order N is identified by its lowest pfn and finds its partner
        // at `pfn ^ (1 << N)`; that identity only holds if the block is aligned to
        // 2^N pages. Hand the allocator a misaligned block and the merge walk
        // reaches for a "buddy" that belongs to some other block entirely --
        // which removes a live frame from its list and loses it. A range that
        // starts anywhere but on a MAX_FRAME_ORDER boundary is the normal case,
        // so this is not a corner.
        //
        // ustl::countr_zero(start) is the largest power of two `start` is
        // aligned to; the second limit then shrinks that to what is left in the
        // range.
        auto order = std::min(usize(MAX_FRAME_ORDER), usize(ustl::countr_zero(start)));
        while (start + (usize{1} << order) > end) {
            order -= 1;
        }

        free_block(*pfn_to_frame(start), order);
        start += usize{1} << order;
    }
    observe_pressure();
}

auto pin_frame(PmFrame *const frame) -> Status {
    DEBUG_ASSERT(frame, "pin requires a frame");
    if (frame->is_role(page::Role::Pmm)) {
        return Status::BadState;
    }
    if (!frame->pin()) {
        return Status::NoResource;
    }
    return Status::Ok;
}

auto unpin_frame(PmFrame *const frame) -> Status {
    DEBUG_ASSERT(frame, "unpin requires a frame");
    if (frame->pin_count() == 0) {
        return Status::BadState;
    }
    frame->unpin();
    return Status::Ok;
}

auto split_block(PmFrame *const frame, usize const order) -> void {
    DEBUG_ASSERT(frame, "split requires a block");
    auto const whole = frame->order();
    DEBUG_ASSERT(order <= whole, "block {:#x} of order {} cannot be split into blocks of order {}",
                 frame_to_phys(frame), whole, order);
    DEBUG_ASSERT(!frame->is_role(page::Role::Pmm), "block {:#x} is split while on a free list",
                 frame_to_phys(frame));
    DEBUG_ASSERT((frame_to_pfn(frame) & ((usize{1} << whole) - 1)) == 0,
                 "frame {:#x} does not head a block of order {}", frame_to_phys(frame), whole);

    // No lock: the block is the caller's, and nothing else reads the layout
    // of a block that is on no free list. The frame does the work: the new
    // heads' orders and their tails' offsets (PmFrame::split()).
    frame->split(order);
}

auto free_frames(PmFrame::List *list) -> void {
    free_blocks(*list);
    observe_pressure();
}

auto unwire_frame(PmFrame *const frame) -> void {
    DEBUG_ASSERT(!!(frame->flags() & page::Flags::Reserved), "frame {:#x} is not reserved",
                 frame_to_phys(frame));
    DEBUG_ASSERT(frame->is_role(page::Role::None), "reserved frame {:#x} has a role",
                 frame_to_phys(frame));
    DEBUG_ASSERT(!frame->is_linked(), "reserved frame {:#x} is on a list", frame_to_phys(frame));

    frame->clear_reserved();
    if (find_node(frame->nid()) != nullptr) {
        if (auto *const zone = PmZone::at(frame->nid(), frame->zone())) {
            zone->sub_reserved_frames(1);
        }
    }
}

} // namespace ours::mm
