#include <ours/assert.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/node_placement.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/time/timer.hpp>

#include <algorithm>
#include <array>
#include <atomic>

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
    PmZone::for_each([&total](PmZone &zone) { total += zone.free_frames(); });
    return total;
}

namespace {
    /// The gate in front of the allocations that can wait (try_alloc_frames()
    /// with Gaf::CanWait), and the signal for whoever watches free memory
    /// (set_free_memory_signal()). It lives here, above PmNode, because every
    /// allocation and free of a frame comes through this file, and the node
    /// below keeps no policy of its own about who waits.
    ///
    /// Every allocation and free asks it something, so what they read takes
    /// no lock: atomics, each loaded on its own. The event that wakes waiters
    /// and the lock that orders the watcher's calls are touched only by
    /// waiters and by the watcher.
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
        auto wait_for_opening(time::Deadline const &deadline, bool interruptible) -> Status;

        /// An opening, or a refusal, passed on to the next waiter.
        auto pass_on() -> void {
            m_may_allocate.signal();
        }

        /// What set_free_memory_signal() does; see there.
        auto set(usize lower, usize upper, usize delay_level, sync::Event *event,
                 DelayedAllocations delayed) -> bool;

      private:
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

        /// Orders the calls of set() among themselves. The allocation side
        /// never takes it.
        sync::SpinLock m_watcher_lock;
    };

    /// Built with the other global objects, the first thing start_kernel()
    /// does, so before the first frame is allocated.
    FreeMemoryGate GATE;

    auto FreeMemoryGate::admit() -> Status {
        auto state = m_state.load(std::memory_order_acquire);
        while (true) {
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
            // Below the level with the gate open: shut it. A watcher that
            // changed the state meanwhile is answered from its new state.
            if (m_state.compare_exchange_weak(state, State::Shut, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                return Status::ShouldWait;
            }
        }
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

    auto FreeMemoryGate::wait_for_opening(time::Deadline const &deadline, bool const interruptible)
        -> Status {
        auto how = task::Interruptible::No;
        if (interruptible) {
            how = task::Interruptible::Yes;
        }
        return m_may_allocate.wait(deadline, how);
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

    /// The entries that answer a bare pointer or a Status cannot say
    /// ShouldWait, so a CanWait reaching one of them would be served past the
    /// gate without anyone noticing.
    auto assert_cannot_wait(Gaf const alloc_flags) -> void {
        DEBUG_ASSERT(!(alloc_flags & Gaf::CanWait),
                     "Gaf::CanWait goes through try_alloc_frames(), the entry that can answer "
                     "ShouldWait");
    }
} // namespace

auto num_nodes() -> usize {
    return PmNode::num_nodes();
}

auto node_work_cpus(NodeId nid) -> CpuMask {
    DEBUG_ASSERT(nid < MAX_NODES);
    if (auto node = PmNode::node(nid)) {
        return node->work_cpus();
    }
    return CpuMask{};
}

auto node_hard_affinity(NodeId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < MAX_NODES);
    if (auto node = PmNode::node(nid)) {
        return &node->hard_affinity();
    }
    return nullptr;
}

auto node_soft_affinity(NodeId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < MAX_NODES);
    if (auto node = PmNode::node(nid)) {
        return &node->soft_affinity();
    }
    return nullptr;
}

/// A zeroed page for one level of a page table, from whichever allocator is up.
/// See the declaration in pmm.hpp for why the choice lives here.
auto alloc_page_table(usize size, usize alignment, NodeId node) -> PhysAddr {
    if (pmm_enabled()) {
        // Through the node-taking overload, so that `node` means the same thing
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
        // bootmem::ANY_NODE is >= MAX_NODES, which this overload forwards to
        // the node-less one, so "no preference" -- every in-tree caller today --
        // behaves exactly as before, the thread's NUMA policy included
        // (applied by PmNode). The AllocState
        // counter applies either way: PmNode::alloc_frames() checks it for
        // every caller.
        auto *frame = alloc_frames(node, ALLOC_KERNEL | Gaf::Zero, get_order(size / PAGE_SIZE));
        if (!frame) {
            return PhysAddr{0};
        }
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
    free_frames(frame, get_order(size / PAGE_SIZE));
}

auto free_frame_count() -> usize {
    usize total = 0;
    PmZone::for_each([&total](PmZone &zone) { total += zone.count_free_frames(); });
    return total;
}

auto try_alloc_frames(Gaf const alloc_flags, usize const order) -> ktl::Result<PmFrame *> {
    if (!!(alloc_flags & Gaf::CanWait)) {
        if (auto const status = GATE.admit(); Status::Ok != status) {
            return ustl::err(status);
        }
    }
    // The gate is this layer's: the node below never sees CanWait.
    auto plain = alloc_flags & ~Gaf::CanWait;
    auto result = PmNode::alloc_frames(plain, order, node_online_mask());
    GATE.check_signal();
    return result;
}

auto wait_for_frame_allocation(Gaf const alloc_flags, time::Deadline const &deadline,
                               bool const interruptible) -> ktl::Result<PmFrame *> {
    DEBUG_ASSERT(!!(alloc_flags & Gaf::CanWait), "a wait for a frame by a caller that cannot wait");
    if (auto const status = GATE.wait_for_opening(deadline, interruptible); Status::Ok != status) {
        return ustl::err(status);
    }

    auto result = try_alloc_frames(alloc_flags, 0);
    // The opening goes on to the next waiter, unless the gate shut again
    // before this one got a frame: then the next opening wakes them. A
    // refusal goes on too, so that every waiter hears it.
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
/// per-CPU pointer until PmNode::publish_cpus() runs from an init hook, while
/// allocation starts asking for a node long before that.
///
/// The table is mm's own (NodePlacement), seeded once at bring-up from what
/// the firmware declared. Nothing on this path asks kernel/lib/numa anything.
auto current_node() -> NodeId {
    return NodePlacement::node_of(CpuLocal::current_cpu());
}

auto node_online_mask() -> NodeMask const & {
    return PmNode::node_online_mask();
}

auto node_possible_mask() -> NodeMask const & {
    return PmNode::node_possible_mask();
}

auto alloc_frames(Gaf alloc_flags, usize order, NodeMask const &fallback) -> PmFrame * {
    assert_cannot_wait(alloc_flags);
    auto result = PmNode::alloc_frames(alloc_flags, order, fallback);
    GATE.check_signal();
    if (!result) {
        log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
        return nullptr;
    }
    return *result;
}

auto alloc_frames(Gaf alloc_flags, PhysAddr *addr, usize order) -> PmFrame * {
    auto *const frame = alloc_frames(alloc_flags, order, node_online_mask());
    if (frame && addr) {
        *addr = frame_to_phys(frame);
    }
    return frame;
}

auto alloc_frames_fragment(Gaf alloc_flags, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    assert_cannot_wait(alloc_flags);
    auto result = PmNode::alloc_frames_fragment(alloc_flags, n, fallback);
    GATE.check_signal();
    if (!result) {
        log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
    }
    return ustl::move(result);
}

auto alloc_frames(NodeId nid, Gaf alloc_flags, PmFrame::List *list, usize n, NodeMask const &nodes)
    -> Status {
    assert_cannot_wait(alloc_flags);
    auto result = PmNode::alloc_frames_bulk(nid, alloc_flags, n, nodes);
    GATE.check_signal();
    if (!result) {
        log::trace("Failed to allocate {} frames with reason: {}", n,
                   to_string(result.unwrap_err()));
        return result.unwrap_err();
    }

    *list = ustl::move(*result);
    return Status::Ok;
}

auto alloc_frames(NodeId nid, Gaf alloc_flags, usize order, NodeMask const &nodes) -> PmFrame * {
    assert_cannot_wait(alloc_flags);
    auto result = PmNode::alloc_frames(nid, alloc_flags, order, nodes);
    GATE.check_signal();
    if (!result) {
        log::trace("Failed to allocate frame on node {} with reason: {}", nid,
                   to_string(result.unwrap_err()));
        return nullptr;
    }
    return *result;
}

auto free_frames(PmFrame *frame, usize order) -> void {
    log::trace("Free frame({:X}, {})", frame_to_phys(frame), order);
    PmNode::free_frames(frame, order);
    GATE.check_signal();
}

auto free_frames(PmFrame::List *list) -> void {
    PmNode::free_frames(list);
    GATE.check_signal();
}

auto unwire_frame(PmFrame *const frame) -> void {
    DEBUG_ASSERT(!!(frame->flags() & page::Flags::Reserved), "frame {:#x} is not reserved",
                 frame_to_phys(frame));
    DEBUG_ASSERT(frame->is_role(page::Role::None), "reserved frame {:#x} has a role",
                 frame_to_phys(frame));
    DEBUG_ASSERT(!frame->is_linked(), "reserved frame {:#x} is on a list", frame_to_phys(frame));

    frame->clear_reserved();
    if (PmNode::node(frame->nid()) != nullptr) {
        if (auto *const zone = PmZone::at(frame->nid(), frame->zone())) {
            zone->sub_reserved_frames(1);
        }
    }
}

} // namespace ours::mm
