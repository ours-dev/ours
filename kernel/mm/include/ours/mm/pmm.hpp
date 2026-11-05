/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PMM_HPP
#define OURS_MM_PMM_HPP 1

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/init_stage.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/node_mask.hpp>
#include <ours/status.hpp>

#include <bootmem/types.hpp>
#include <ktl/result.hpp>
#include <ustl/bit.hpp>

namespace ours::sync {
class Event;
} // namespace ours::sync

namespace ours::time {
class Deadline;
} // namespace ours::time

namespace ours::mm {
namespace priv {
    extern "C" PhysAddr MAX_PHYS_ADDR;
    extern "C" PhysAddr MIN_PHYS_ADDR;
} // namespace priv

FORCE_INLINE
constexpr auto max_phys_addr() -> PhysAddr {
    return priv::MAX_PHYS_ADDR;
}

FORCE_INLINE
constexpr auto min_phys_addr() -> PhysAddr {
    return priv::MIN_PHYS_ADDR;
}

FORCE_INLINE
constexpr auto max_pfn() -> Pfn {
    return phys_to_pfn(max_phys_addr());
}

FORCE_INLINE
constexpr auto min_pfn() -> Pfn {
    return phys_to_pfn(min_phys_addr());
}

/// Whether the real page allocator has taken over from EarlyMem.
///
/// Derived from the init stage rather than tracked by a flag of its own: the
/// two would say the same thing, and two things saying the same thing can
/// disagree. set_init_stage(InitStage::Pmm) is the single point that makes
/// this true, and it sits immediately after EarlyMem::do_handoff() because
/// that call is the handover -- see InitStage::Pmm.
FORCE_INLINE
inline auto pmm_enabled() -> bool {
    return get_init_stage() >= InitStage::Pmm;
}

/// Whether the kernel heap exists, i.e. whether `new`/kmalloc may be called.
///
/// Separate from pmm_enabled() and never a substitute for it. A caller that
/// only needs frames must ask pmm_enabled(); a caller that needs `new` must
/// ask this one. Collapsing the two is what made alloc_page_table() keep
/// taking frames from EarlyMem after the buddy already owned them.
FORCE_INLINE
inline auto heap_enabled() -> bool {
    return get_init_stage() >= InitStage::Heap;
}

[[nodiscard]]
auto cpu_to_node(CpuNum cpunum) -> NodeId;

[[nodiscard]]
auto current_node() -> NodeId;

[[nodiscard]]
/// By value: the node's mask is rewritten when a CPU is re-homed, so there
/// is no reference to it that stays valid outside the node lock. An empty
/// mask for a node that does not exist.
auto node_work_cpus(NodeId nid) -> CpuMask;

[[nodiscard]]
auto node_soft_affinity(NodeId nid) -> CpuMask const *;

[[nodiscard]]
auto node_hard_affinity(NodeId nid) -> CpuMask const *;

[[nodiscard]]
auto node_online_mask() -> NodeMask const &;

[[nodiscard]]
auto node_possible_mask() -> NodeMask const &;

auto num_nodes() -> usize;

FORCE_INLINE
inline auto num_online_nodes() -> usize {
    return node_online_mask().count();
}

FORCE_INLINE
inline auto num_possible_nodes() -> usize {
    return node_possible_mask().count();
}

/// Using constexpr to make those pre-culcalate requests able to
/// convert the order of frames requested at compiling time.
FORCE_INLINE
constexpr auto get_order(usize n) -> usize {
    return ustl::countr_zero(ustl::bit_ceil(n));
}

/// Attempt a block allocation, starting on the local node. With CanWait,
/// pressure can return ShouldWait. The caller releases its locks before waiting.
[[nodiscard]]
auto alloc_frames(Gaf, usize order = 0, NodeMask const & = node_online_mask())
    -> ktl::Result<PmFrame *>;

/// Write the frame's physical address on success when |addr| is supplied.
[[nodiscard]]
auto alloc_frames(Gaf, PhysAddr *addr, usize order = 0) -> ktl::Result<PmFrame *>;

[[nodiscard]]
auto alloc_frames_fragment(Gaf, usize, NodeMask const & = node_online_mask())
    -> ktl::Result<PmFrame::List>;

/// Allocate one power-of-two block large enough for the requested page count.
FORCE_INLINE [[nodiscard]]
inline auto alloc_frames_n(Gaf alloc_flags, usize nr_frames,
                           NodeMask const &nodes = node_online_mask()) -> ktl::Result<PmFrame *> {
    return alloc_frames(alloc_flags, get_order(nr_frames), nodes);
}

/// Allocate an immediate list of single frames.
[[nodiscard]]
auto alloc_frames(NodeId, Gaf, PmFrame::List *, usize, NodeMask const & = node_online_mask())
    -> Status;

/// The same block allocation protocol with an explicit home node.
[[nodiscard]]
auto alloc_frames(NodeId, Gaf, usize, NodeMask const & = node_online_mask())
    -> ktl::Result<PmFrame *>;

/// Allocate one power-of-two block large enough for the requested page count.
FORCE_INLINE [[nodiscard]]
inline auto alloc_frames_n(NodeId nid, Gaf alloc_flags, usize nr_frames,
                           NodeMask const &nodes = node_online_mask()) -> ktl::Result<PmFrame *> {
    return alloc_frames(nid, alloc_flags, get_order(nr_frames), nodes);
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frames(Gaf alloc_flags, usize order = 0, NodeMask const &nodes = node_online_mask())
    -> T * {
    DEBUG_ASSERT(!(alloc_flags & Gaf::CanWait), "use alloc_frames() to handle ShouldWait");
    if (auto frame = alloc_frames(alloc_flags, order, nodes)) {
        return PhysMap::phys_to_virt<T>(frame_to_phys(*frame));
    }

    return nullptr;
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frames_n(Gaf alloc_flags, usize nr_frames, NodeMask const &nodes = node_online_mask())
    -> T * {
    return get_frames<T>(alloc_flags, get_order(nr_frames), nodes);
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frames(NodeId nid, Gaf alloc_flags, usize order = 0,
                NodeMask const &nodes = node_online_mask()) -> T * {
    DEBUG_ASSERT(!(alloc_flags & Gaf::CanWait), "use alloc_frames() to handle ShouldWait");
    if (auto frame = alloc_frames(nid, alloc_flags, order, nodes)) {
        return PhysMap::phys_to_virt<T>(frame_to_phys(*frame));
    }

    return nullptr;
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frames_n(NodeId nid, Gaf alloc_flags, usize nr_frames,
                  NodeMask const &nodes = node_online_mask()) -> T * {
    return get_frames<T>(nid, alloc_flags, get_order(nr_frames), nodes);
}

/// A fresh, zeroed page-table page, from whichever allocator is up.
///
/// This is what lets the page-table walker run before the buddy exists.
/// Everything else it needs -- the root table, the phys-to-virt view -- is
/// there the moment KERNEL_PGD is, so the page source was the only thing
/// tying page-table edits to a particular point in boot. Hard-coding
/// alloc_frames() here meant CpuLocal::Mapper worked only because it happened
/// to run after the buddy, and left VflatMemMap unable to run at all.
///
/// Before the buddy this comes from EarlyMem, which never gives it back:
/// do_handoff() releases what bootmem still calls free, and an allocation is
/// not free. A page table taken this early therefore stays reserved for the
/// life of the kernel, which is what we want -- kernel page tables are not
/// freed anyway.
///
/// That reasoning only holds *before* do_handoff(), and pmm_enabled() is what
/// keeps it there. It used to become true two steps later, when the heap came
/// up, so every page table allocated in between -- all of CpuLocal::init()'s --
/// came out of EarlyMem after the buddy had already been handed the same
/// ranges. Marking such a page used in bootmem changes nothing: the buddy's
/// copy of it does not go away, and the next Gaf::Zero allocation to land on
/// that frame zeroes a live page table.
[[nodiscard]]
auto alloc_page_table(usize size, usize alignment, NodeId node = bootmem::ANY_NODE) -> PhysAddr;

/// Only the buddy path returns anything; an early table page is not reclaimable.
/// The frame must still be Role::Mmu, as alloc_page_table() left it; a debug
/// build asserts it.
auto free_page_table(PhysAddr base, usize size) -> void;

/// Returns |frame|'s block to its zone's buddy allocator. The frame must not
/// be reserved: a frame stamped Reserved at boot has to go through
/// unwire_frame() first, or the buddy would hold a frame the reserved counts
/// still claim (a debug build asserts it, in PmNode::free_frames()).
auto free_frames(PmFrame *frame, usize order = 0) -> void;

/// Takes |frame|, reserved since boot, off the reserved books, so that it can
/// be owned like a frame the allocator handed out: freed to the buddy
/// (EarlyMem::reclaim_init_area()) or adopted into a VMO. Clears Reserved and
/// subtracts the frame from its node's and zone's reserved counts. The role
/// is the new owner's to set.
///
/// A debug build asserts the frame is reserved, has role None and is on no
/// list. The only reserved frames with a role are boot page tables (Mmu),
/// and nothing frees a boot page table, so nothing takes one off the books.
/// That includes physboot's identity-map tables, which x86_setup_mmu()
/// unhooks and leaves in place, still Reserved and counted
/// (docs/repairs/261004/x86-mm.md). Reclaiming them would need this to
/// accept Role::Mmu, and the page-table free path to tell a boot table apart
/// by its Reserved flag.
auto unwire_frame(PmFrame *frame) -> void;

FORCE_INLINE
inline auto free_frames(VirtAddr virt_addr, usize order = 0) -> void {
    return free_frames(virt_to_frame(virt_addr), order);
}

auto free_frames(PmFrame::List *list) -> void;

/// Every frame the buddy allocators could hand out right now, in pages,
/// summed over the online nodes' own zones. For diagnostics: it walks every
/// free list under each zone's lock, and the answer is stale the moment it
/// returns. What it is good for is a difference -- the same count taken
/// before and after something that should have given memory back.
[[nodiscard]]
auto free_frame_count() -> usize;

/// Free frames, summed from each zone's own counter without its lock: the
/// count the allocation gate and the free-memory signal compare
/// with their levels, and the one to set those levels from. It also counts
/// the frames the per-CPU caches hold, which free_frame_count() does not, so
/// the two differ by up to those caches. Stale by the time it returns, as
/// any count would be; the levels are a margin, not an exact line.
[[nodiscard]]
auto approx_free_frame_count() -> usize;

/// Sleeps until the gate opens and takes one frame for the caller, with
/// |alloc_flags|, which carries CanWait. Called with no lock held, after
/// alloc_frames(alloc_flags) answered ShouldWait for a single page with the
/// default node selection.
///
/// Waiters wake one at a time: each that gets its frame wakes the next, so
/// an opening does not send every waiter after the same frames at once.
/// Answers ShouldWait when the gate was shut again before this one got a
/// frame, and the caller waits again; OutOfMem when delayed allocations are
/// refused (DelayedAllocations::Refuse); TimeOut at |deadline|.
[[nodiscard]]
auto wait_for_frame_allocation(Gaf alloc_flags, time::Deadline const &deadline, bool interruptible)
    -> ktl::Result<PmFrame *>;

/// What a CanWait allocation gets when it finds free memory below the delay
/// level.
enum class DelayedAllocations : u8 {
    /// Told to wait (ShouldWait), and the gate shuts behind it.
    Wait,
    /// Refused (OutOfMem), and so is every waiter: for whoever watches
    /// memory, when nothing can be given back.
    Refuse,
};

/// For whoever watches memory. Signals |event| once, the first time an
/// allocation or a free leaves free memory outside [lower, upper] frames;
/// sets the delay level to |delay_level| frames and what an allocation below
/// it gets to |delayed|; and opens the gate, waking the waiters, which
/// retry, or with Refuse are refused. A delay level of 0 never shuts the
/// gate; a null |event| asks for no signal.
///
/// With Wait, the level must not be above |lower| when an event is given: a
/// shut gate then always means the event has been signalled, so the watcher
/// is never asleep while allocations wait for it. With Refuse nobody waits.
///
/// Answers false, and arms nothing, when free memory is already outside
/// [lower, upper]: the caller has missed the change it wants to hear about,
/// and works out its bounds again.
auto set_free_memory_signal(usize lower, usize upper, usize delay_level, sync::Event *event,
                            DelayedAllocations delayed) -> bool;

auto pin_frame(PmFrame *frame) -> Status;

auto unpin_frame(PmFrame *frame) -> Status;

} // namespace ours::mm

#endif // #ifndef OURS_MM_PMM_HPP
