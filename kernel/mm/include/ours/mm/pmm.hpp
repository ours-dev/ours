/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PMM_HPP
#define OURS_MM_PMM_HPP 1

#include <bootmem/types.hpp>
#include <ktl/result.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/init_stage.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/node_mask.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/status.hpp>
#include <ustl/bit.hpp>

namespace ours::mm {
namespace priv {
    NO_MANGLE PhysAddr MAX_PHYS_ADDR;
    NO_MANGLE PhysAddr MIN_PHYS_ADDR;
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
/// this true.
FORCE_INLINE
inline auto pmm_enabled() -> bool {
    return get_init_stage() >= InitStage::Pmm;
}

[[nodiscard]]
auto cpu_to_node(CpuNum cpunum) -> NnId;

[[nodiscard]]
auto current_node() -> NnId;

[[nodiscard]]
/// By value: the node's mask is rewritten when a CPU is re-homed, so there
/// is no reference to it that stays valid outside the node lock. An empty
/// mask for a node that does not exist.
auto node_work_cpus(NnId nid) -> CpuMask;

[[nodiscard]]
auto node_soft_affinity(NnId nid) -> CpuMask const *;

[[nodiscard]]
auto node_hard_affinity(NnId nid) -> CpuMask const *;

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

/// Prefer to allocate a frame on the local node.
[[nodiscard]]
auto alloc_frames(Gaf, usize order = 0, NodeMask const & = node_online_mask()) -> PmFrame *;

[[nodiscard]]
auto alloc_frames(Gaf, PhysAddr *addr, usize order = 0) -> PmFrame *;

[[nodiscard]]
auto alloc_frames_fragment(Gaf, usize, NodeMask const & = node_online_mask())
    -> ktl::Result<PmFrame::List>;

FORCE_INLINE [[nodiscard]]
inline auto alloc_frame_n(Gaf gaf, usize nr_frames, NodeMask const &nodes = node_online_mask())
    -> PmFrame * {
    return alloc_frames(gaf, get_order(nr_frames), nodes);
}

/// Allocate the a frame on a specified node.
[[nodiscard]]
auto alloc_frames(NnId, Gaf, PmFrame::List *, usize, NodeMask const & = node_online_mask())
    -> Status;

[[nodiscard]]
auto alloc_frames(NnId, Gaf, usize, NodeMask const & = node_online_mask()) -> PmFrame *;

FORCE_INLINE [[nodiscard]]
inline auto alloc_frame_n(NnId nid, Gaf gaf, usize nr_frames,
                          NodeMask const &nodes = node_online_mask()) -> PmFrame * {
    return alloc_frames(nid, gaf, get_order(nr_frames), nodes);
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frames(Gaf gaf, usize order = 0, NodeMask const &nodes = node_online_mask()) -> T * {
    if (auto frame = alloc_frames(gaf, order, nodes)) {
        return PhysMap::phys_to_virt<T>(frame_to_phys(frame));
    }

    return nullptr;
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frames_n(Gaf gaf, usize nr_frame, NodeMask const &nodes = node_online_mask()) -> T * {
    return get_frames<T>(gaf, get_order(nr_frame), nodes);
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frames(NnId nid, Gaf gaf, usize order = 0, NodeMask const &nodes = node_online_mask())
    -> T * {
    if (auto frame = alloc_frames(nid, gaf, order, nodes)) {
        return PhysMap::phys_to_virt<T>(frame_to_phys(frame));
    }

    return nullptr;
}

template <typename T = u8>
[[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]
FORCE_INLINE
auto get_frame_n(NnId nid, Gaf gaf, usize nr_frame, NodeMask const &nodes = node_online_mask())
    -> T * {
    return get_frame<T>(nid, gaf, get_order(nr_frame), nodes);
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
/// Linux's counterpart is alloc_low_pages() (arch/x86/mm/init.c), branching on
/// `after_bootmem` exactly as this branches on pmm_enabled().
[[nodiscard]]
auto alloc_page_table(usize size, usize alignment, NnId node = bootmem::ANY_NODE) -> PhysAddr;

/// Only the buddy path returns anything; an early table page is not reclaimable.
auto free_page_table(PhysAddr base, usize size) -> void;

auto free_frames(PmFrame *frame, usize order = 0) -> void;

FORCE_INLINE
inline auto free_frames(VirtAddr virt_addr, usize order = 0) -> void {
    return free_frames(virt_to_frame(virt_addr), order);
}

auto free_frames(PmFrame::List *list) -> void;

auto pin_frame(PmFrame *frame) -> Status;

auto unpin_frame(PmFrame *frame) -> Status;

} // namespace ours::mm

#endif // #ifndef OURS_MM_PMM_HPP