/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_GAF_HPP
#define OURS_MM_GAF_HPP

#include <ours/macro_abi.hpp>
#include <ours/mm/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
/// Shorthand of Getting Available Frames.
///
/// Only the bits something reads. The Linux GFP names this once copied for
/// DMA zones (Dma, Dma32, Movable), retry policy (NoFail, NoRetry), page-cache
/// dirtying (Write), kswapd (DaemonReclaim) and compound pages (Compound)
/// went: PmNode masked every one of them off before use, except
/// DaemonReclaim, which nothing read. The values of the survivors are
/// unchanged; nothing stores a Gaf.
enum class Gaf : u32 {
    /// The allocation may wait: when the zones have nothing, it reclaims and
    /// compacts and tries again rather than failing. Linux's
    /// __GFP_DIRECT_RECLAIM. Zircon's nearest is PMM_ALLOC_FLAG_CAN_WAIT,
    /// which lets the PMM answer SHOULD_WAIT and leaves the waiting to the
    /// caller. Reclaim and compaction are stubs today, so the retry never
    /// ends (docs/repairs/260908/heap.md item 3).
    DirectReclaim = 7_bit,

    /// Only the node the allocation starts from: the local node, or the one
    /// named by an alloc_frames(nid, ...) overload. When that node has nothing
    /// the allocation fails (or, with DirectReclaim, retries) rather than
    /// spilling to another node. Linux's __GFP_THISNODE. No preset carries
    /// it: local-first is already the zone walk's order (PmNode::m_fallback),
    /// so ask for this only where memory on another node would defeat the
    /// allocation's purpose.
    ThisNode = 9_bit,

    /// Return a zeroed frame on success.
    Zero = 10_bit,
};
USTL_ENABLE_ENUM_BITMASK(Gaf);

/// The presets live outside the enum, so that Gaf is only a set of bits, as
/// Linux's GFP_KERNEL and GFP_USER are #defines beside the bit list. There is
/// no boot preset: what boot code needs beyond GAF_KERNEL is Zero, and it asks
/// for that by name.
///
/// The kernel's own memory: may wait.
inline constexpr auto GAF_KERNEL = Gaf::DirectReclaim;

/// Memory a process will see: zeroed, and fails rather than waits.
inline constexpr auto GAF_USER = Gaf::Zero;

enum class AllocationOptions : u32 {
    NoFragment = 0_bit,

    NoBlock = 1_bit,

    Contiguous = 2_bit,

    Kswapd = 3_bit,
};
USTL_ENABLE_ENUM_BITMASK(AllocationOptions);

} // namespace ours::mm

#endif // OURS_MM_GAF_HPP
