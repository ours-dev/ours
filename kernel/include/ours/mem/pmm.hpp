/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_PMM_HPP
#define OURS_MEM_PMM_HPP 1

#include <ours/mem/gaf.hpp>
#include <ours/mem/pm-frame.hpp>
#include <ours/mem/memory-model.hpp>
#include <ours/mem/node-mask.hpp>
#include <ours/mem/physmap.hpp>

#include <ours/status.hpp>
#include <ours/cpu-mask.hpp>

#include <ktl/result.hpp>
#include <ustl/bit.hpp>

namespace ours::mem {
namespace priv {
    NO_MANGLE PhysAddr g_max_phys_addr;
    NO_MANGLE PhysAddr g_min_phys_addr;
} // namespace priv

    FORCE_INLINE CXX11_CONSTEXPR
    auto max_phys_addr() -> PhysAddr { return priv::g_max_phys_addr; }

    FORCE_INLINE CXX11_CONSTEXPR
    auto min_phys_addr() -> PhysAddr { return priv::g_min_phys_addr; }

    FORCE_INLINE CXX11_CONSTEXPR
    auto max_pfn() -> Pfn { return phys_to_pfn(max_phys_addr()); }

    FORCE_INLINE CXX11_CONSTEXPR
    auto min_pfn() -> Pfn { return phys_to_pfn(min_phys_addr()); }

    FORCE_INLINE CXX11_CONSTEXPR
    static auto pmm_enabled() -> bool {
        extern bool g_pmm_enabled;
        return g_pmm_enabled;
    }

    auto cpu_to_node(CpuNum cpunum) -> NnId;

    auto current_node() -> NnId;

    auto node_work_cpus(NnId nid) -> CpuMask const *;
    auto node_soft_affinity(NnId nid) -> CpuMask const *;
    auto node_hard_affinity(NnId nid) -> CpuMask const *;

    auto node_online_mask() -> NodeMask const &;
    auto node_possible_mask() -> NodeMask const &;

    auto num_nodes() -> usize;

    FORCE_INLINE
    inline auto num_online_nodes() -> usize { return node_online_mask().count(); }

    FORCE_INLINE
    inline auto num_possible_nodes() -> usize { return node_possible_mask().count(); }

    /// Using constexpr to make those pre-culcalate requests able to 
    /// convert the order of frames requested at compiling time.
    FORCE_INLINE CXX11_CONSTEXPR
    auto get_order(usize n) -> usize { return ustl::countr_zero(ustl::bit_ceil(n)); }

    /// Prefer to allocate a frame on the local node.
    [[nodiscard]]
    auto alloc_frames(Gaf, usize order = 0, NodeMask const & = node_online_mask()) -> PmFrame *;

    [[nodiscard]]
    auto alloc_frames(Gaf, PhysAddr *addr, usize order = 0) -> PmFrame *; 

    [[nodiscard]]
    auto alloc_frames_fragment(Gaf, usize, NodeMask const & = node_online_mask()) 
        -> ktl::Result<PmFrame::List>;

    [[nodiscard]]
    FORCE_INLINE
    inline auto alloc_frame_n(Gaf gaf, usize nr_frames, NodeMask const &nodes = node_online_mask()) -> PmFrame * {
        return alloc_frames(gaf, get_order(nr_frames), nodes);
    }

    /// Allocate the a frame on a specified node.
    [[nodiscard]]
    auto alloc_frames(NnId, Gaf, PmFrame::List *, usize, NodeMask const & = node_online_mask()) -> Status;

    [[nodiscard]]
    auto alloc_frames(NnId, Gaf, usize, NodeMask const & = node_online_mask()) -> PmFrame *;

    [[nodiscard]] FORCE_INLINE
    inline auto alloc_frame_n(NnId nid, Gaf gaf, usize nr_frames, NodeMask const &nodes = node_online_mask()) -> PmFrame * {
        return alloc_frames(nid, gaf, get_order(nr_frames), nodes);
    }

    template <typename T = u8>
    OM_API FORCE_INLINE
    auto get_frames(Gaf gaf, usize order = 0, NodeMask const &nodes = node_online_mask()) -> T * {
        if (auto frame = alloc_frames(gaf, order, nodes)) {
            return PhysMap::phys_to_virt<T>(frame_to_phys(frame));
        }

        return nullptr;
    }

    template <typename T = u8>
    OM_API FORCE_INLINE
    auto get_frames_n(Gaf gaf, usize nr_frame, NodeMask const &nodes = node_online_mask()) -> T * {
        return get_frames<T>(gaf, get_order(nr_frame), nodes);
    }

    template <typename T = u8>
    OM_API FORCE_INLINE
    auto get_frames(NnId nid, Gaf gaf, usize order = 0, NodeMask const &nodes = node_online_mask()) -> T * {
        if (auto frame = alloc_frames(nid, gaf, order, nodes)) {
            return PhysMap::phys_to_virt<T>(frame_to_phys(frame));
        }

        return nullptr;
    }

    template <typename T = u8>
    OM_API FORCE_INLINE
    auto get_frame_n(NnId nid, Gaf gaf, usize nr_frame, NodeMask const &nodes = node_online_mask()) -> T * {
        return get_frame<T>(nid, gaf, get_order(nr_frame), nodes);
    }

    auto free_frames(PmFrame *frame, usize order = 0) -> void;

    FORCE_INLINE
    inline auto free_frames(VirtAddr virt_addr, usize order = 0) -> void {
        return free_frames(virt_to_frame(virt_addr), order);
    }

    auto free_frames(PmFrame::List *list) -> void;

    auto pin_frame(PmFrame *frame) -> Status;

    auto unpin_frame(PmFrame *frame) -> Status;

} // namespace ours::mem

#endif // #ifndef OURS_MEM_PMM_HPP