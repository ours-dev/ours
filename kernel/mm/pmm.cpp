#include <ours/assert.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
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
    PmNode::for_each_nid(PmNode::State::Online, [&total](NnId const nid) {
        auto *const node = PmNode::node(nid);
        if (!node) {
            return;
        }
        node->zone_list()->for_each_local_zone(
            [&total](PmZone &zone) { total += zone.free_frames(); });
    });
    return total;
}

namespace {
    /// The gate in front of the allocations that can wait (try_alloc_frames()
    /// with Gaf::CanWait), and the signal for whoever watches free memory
    /// (set_free_memory_signal()). Both live here, above PmNode, because
    /// every allocation and free of a frame comes through this file, and the
    /// node below keeps no policy of its own about who waits.
    ///
    /// Every allocation and free reads them, so what they read takes no lock:
    /// atomics, each loaded on its own. The event that wakes waiters and the
    /// lock that orders the watcher's calls are touched only by waiters and
    /// by the watcher.
    enum class GateState : u8 {
        /// A CanWait allocation that finds free memory below the delay level
        /// shuts the gate.
        Open,
        /// Every CanWait allocation answers ShouldWait, whatever free memory
        /// is now, until the watcher opens the gate.
        Shut,
        /// A CanWait allocation that finds free memory below the delay level
        /// fails, and a waiter wakes with OutOfMem (refuse_delayed_allocations()).
        Refusing,
    };

    constinit std::atomic<GateState> GATE{GateState::Open};
    constinit std::atomic<usize> DELAY_LEVEL{0};

    /// The watcher's bounds, in frames, and its event, published last: an
    /// event read with acquire comes with bounds at least as new as the ones
    /// it was armed with.
    /// Null when nothing is armed, which is also after it has been signalled
    /// once.
    constinit std::atomic<usize> SIGNAL_LOWER{0};
    constinit std::atomic<usize> SIGNAL_UPPER{0};
    constinit std::atomic<sync::Event *> SIGNAL_EVENT{nullptr};

    /// Wakes the waiters in wait_for_frame_allocation(), one per signal.
    sync::Event MAY_ALLOCATE{sync::Event::Options::AutoUnsignal};

    /// Orders set_free_memory_signal() and refuse_delayed_allocations()
    /// among themselves. The allocation side never takes it.
    sync::SpinLock WATCHER_LOCK;

    /// After every allocation and free: signals the watcher's event, once,
    /// when free memory has left its bounds. One load when nothing is armed.
    auto check_free_memory_signal() -> void {
        if (SIGNAL_EVENT.load(std::memory_order_acquire) == nullptr) {
            return;
        }
        auto const free = approx_free_frame_count();
        if (free >= SIGNAL_LOWER.load(std::memory_order_relaxed) &&
            free <= SIGNAL_UPPER.load(std::memory_order_relaxed)) {
            return;
        }
        // Two CPUs may both see the bounds left; the one that takes the event
        // signals it, and the other finds it gone.
        if (auto *const event = SIGNAL_EVENT.exchange(nullptr, std::memory_order_acq_rel)) {
            event->signal();
        }
    }

    /// Whether a CanWait allocation may go ahead: Ok, ShouldWait while the
    /// gate is shut, or OutOfMem while delayed allocations are refused.
    auto admit_waitable() -> Status {
        auto state = GATE.load(std::memory_order_acquire);
        while (true) {
            if (state == GateState::Shut) {
                return Status::ShouldWait;
            }
            auto const level = DELAY_LEVEL.load(std::memory_order_relaxed);
            if (level == 0 || approx_free_frame_count() >= level) {
                return Status::Ok;
            }
            if (state == GateState::Refusing) {
                return Status::OutOfMem;
            }
            // Below the level with the gate open: shut it. A watcher that
            // changed the state meanwhile is answered from its new state.
            if (GATE.compare_exchange_weak(state, GateState::Shut, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
                return Status::ShouldWait;
            }
        }
    }

    /// The entries that answer a bare pointer or a Status cannot say
    /// ShouldWait, so a CanWait reaching one of them would be served past the
    /// gate without anyone noticing.
    auto assert_cannot_wait(Gaf const gaf) -> void {
        DEBUG_ASSERT(!(gaf & Gaf::CanWait),
                     "Gaf::CanWait goes through try_alloc_frames(), the entry that can answer "
                     "ShouldWait");
    }
} // namespace

auto num_nodes() -> usize {
    return PmNode::num_nodes();
}

auto node_work_cpus(NnId nid) -> CpuMask {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE);
    if (auto node = PmNode::node(nid)) {
        return node->work_cpus();
    }
    return CpuMask{};
}

auto node_hard_affinity(NnId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE);
    if (auto node = PmNode::node(nid)) {
        return &node->hard_affinity();
    }
    return nullptr;
}

auto node_soft_affinity(NnId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE);
    if (auto node = PmNode::node(nid)) {
        return &node->soft_affinity();
    }
    return nullptr;
}

/// A zeroed page for one level of a page table, from whichever allocator is up.
/// See the declaration in pmm.hpp for why the choice lives here.
auto alloc_page_table(usize size, usize alignment, NnId node) -> PhysAddr {
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
        // bootmem::ANY_NODE is >= NUMA_MAX_NODE, which this overload forwards to
        // the node-less one, so "no preference" -- every in-tree caller today --
        // behaves exactly as before, the thread's NUMA policy included
        // (PmNode::Current::apply_thread_policy()). The MemoryAllocationState
        // counter applies either way: PmNode::alloc_frames() checks it for
        // every caller.
        auto *frame = alloc_frames(node, GAF_KERNEL | Gaf::Zero, get_order(size / PAGE_SIZE));
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
    return PhysMap::virt_to_phys(VirtAddr(table));
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
    // The node's own zones, not its fallback order: that one lists other
    // nodes' zones too, and would count them once per node.
    PmNode::for_each_nid(PmNode::State::Online, [&total](NnId const nid) {
        auto *const node = PmNode::node(nid);
        if (!node) {
            return;
        }
        node->zone_list()->for_each_local_zone(
            [&total](PmZone &zone) { total += zone.count_free_frames(); });
    });
    return total;
}

auto try_alloc_frames(Gaf const gaf, usize const order) -> ktl::Result<PmFrame *> {
    if (!!(gaf & Gaf::CanWait)) {
        if (auto const status = admit_waitable(); Status::Ok != status) {
            return ustl::err(status);
        }
    }
    // The gate is this layer's: the node below never sees CanWait.
    auto result = PmNode::Current::alloc_frames(gaf & ~Gaf::CanWait, order, node_online_mask());
    check_free_memory_signal();
    return result;
}

auto wait_for_frame_allocation(Gaf const gaf, time::Deadline const &deadline,
                               bool const interruptible) -> ktl::Result<PmFrame *> {
    DEBUG_ASSERT(!!(gaf & Gaf::CanWait), "a wait for a frame by a caller that cannot wait");
    auto how = task::Interruptible::No;
    if (interruptible) {
        how = task::Interruptible::Yes;
    }
    if (auto const status = MAY_ALLOCATE.wait(deadline, how); Status::Ok != status) {
        return ustl::err(status);
    }

    auto result = try_alloc_frames(gaf, 0);
    // The opening goes on to the next waiter, unless the gate shut again
    // before this one got a frame: then the next opening wakes them. A
    // refusal goes on too, so that every waiter hears it.
    if (result || Status::ShouldWait != result.unwrap_err()) {
        MAY_ALLOCATE.signal();
    }
    return result;
}

auto set_free_memory_signal(usize const lower, usize const upper, usize const delay_level,
                            sync::Event *const event) -> bool {
    DEBUG_ASSERT(lower <= upper, "free-memory bounds {}..{} the wrong way round", lower, upper);
    DEBUG_ASSERT(!event || delay_level <= lower,
                 "a delay level {} above the lower bound {}: the gate could shut unsignalled",
                 delay_level, lower);
    {
        Guard<sync::SpinLock, IrqSave> guard{WATCHER_LOCK};
        if (event) {
            auto const free = approx_free_frame_count();
            if (free < lower || free > upper) {
                return false;
            }
        }
        // Disarmed while the bounds change. A CPU that read the old event just
        // before may still compare free memory with the new bounds and signal
        // the old event once more; that only makes the watcher look again.
        SIGNAL_EVENT.store(nullptr, std::memory_order_relaxed);
        SIGNAL_LOWER.store(lower, std::memory_order_relaxed);
        SIGNAL_UPPER.store(upper, std::memory_order_relaxed);
        DELAY_LEVEL.store(delay_level, std::memory_order_relaxed);
        GATE.store(GateState::Open, std::memory_order_release);
        SIGNAL_EVENT.store(event, std::memory_order_release);
    }
    // Outside the lock: a signal wakes a thread, and the scheduler's lock is
    // not one to take inside a spinlock of the PMM's.
    MAY_ALLOCATE.signal();
    return true;
}

auto refuse_delayed_allocations() -> void {
    {
        Guard<sync::SpinLock, IrqSave> guard{WATCHER_LOCK};
        GATE.store(GateState::Refusing, std::memory_order_release);
    }
    MAY_ALLOCATE.signal();
}

/// Called from kmalloc and MemCache::allocate to resolve "no preference" into a
/// real node.
auto current_node() -> NnId {
    return PmNode::Topology::current_node();
}

auto node_online_mask() -> NodeMask const & {
    return PmNode::node_online_mask();
}

auto node_possible_mask() -> NodeMask const & {
    return PmNode::node_possible_mask();
}

auto alloc_frames(Gaf gaf, usize order, NodeMask const &fallback) -> PmFrame * {
    assert_cannot_wait(gaf);
    auto result = PmNode::Current::alloc_frames(gaf, order, fallback);
    check_free_memory_signal();
    if (!result) {
        log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
        return nullptr;
    }
    return *result;
}

auto alloc_frames(Gaf gaf, PhysAddr *addr, usize order) -> PmFrame * {
    auto *const frame = alloc_frames(gaf, order, node_online_mask());
    if (frame && addr) {
        *addr = frame_to_phys(frame);
    }
    return frame;
}

auto alloc_frames_fragment(Gaf gaf, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    assert_cannot_wait(gaf);
    auto result = PmNode::Current::alloc_frames_fragment(gaf, n, fallback);
    check_free_memory_signal();
    if (!result) {
        log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
    }
    return ustl::move(result);
}

auto alloc_frames(NnId nid, Gaf gaf, PmFrame::List *list, usize n, NodeMask const &nodes)
    -> Status {
    assert_cannot_wait(gaf);
    // NUMA_MAX_NODE means "no preference": use the node the current CPU
    // belongs to, which is what the node-less overloads do.
    if (nid >= NUMA_MAX_NODE) {
        auto result = PmNode::Current::alloc_frames_bulk(gaf, n, nodes);
        check_free_memory_signal();
        if (!result) {
            return result.unwrap_err();
        }
        *list = ustl::move(*result);
        return Status::Ok;
    }

    auto const node = PmNode::node(nid);
    if (!node) {
        return Status::InvArgs;
    }

    auto result = node->alloc_frames_bulk(gaf, n, nodes);
    check_free_memory_signal();
    if (!result) {
        log::trace("Failed to allocate {} frames with reason: {}", n,
                   to_string(result.unwrap_err()));
        return result.unwrap_err();
    }

    *list = ustl::move(*result);
    return Status::Ok;
}

auto alloc_frames(NnId nid, Gaf gaf, usize order, NodeMask const &nodes) -> PmFrame * {
    if (nid >= NUMA_MAX_NODE) {
        return alloc_frames(gaf, order, nodes);
    }

    auto const node = PmNode::node(nid);
    if (!node) {
        return nullptr;
    }

    assert_cannot_wait(gaf);
    auto result = node->alloc_frames(gaf, order, nodes);
    check_free_memory_signal();
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
    check_free_memory_signal();
}

auto free_frames(PmFrame::List *list) -> void {
    PmNode::free_frames(list);
    check_free_memory_signal();
}

auto unwire_frame(PmFrame *const frame) -> void {
    DEBUG_ASSERT(!!(frame->flags() & page::Flags::Reserved), "frame {:#x} is not reserved",
                 frame_to_phys(frame));
    DEBUG_ASSERT(frame->is_role(page::Role::None), "reserved frame {:#x} has a role",
                 frame_to_phys(frame));
    DEBUG_ASSERT(!frame->is_linked(), "reserved frame {:#x} is on a list", frame_to_phys(frame));

    frame->clear_reserved();
    if (auto *const node = PmNode::node(frame->nid())) {
        node->sub_reserved_frames(1);
        if (auto *const zone = node->zone_list()->local_zone(frame->zone())) {
            zone->sub_reserved_frames(1);
        }
    }
}

} // namespace ours::mm