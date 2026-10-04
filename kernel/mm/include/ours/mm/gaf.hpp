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

    ThisNode = 9_bit,

    /// Return a zeroed frame on success.
    Zero = 10_bit,

    Boot = static_cast<u32>(ThisNode) | static_cast<u32>(DirectReclaim) | static_cast<u32>(Zero),
    User = static_cast<u32>(ThisNode) | static_cast<u32>(Zero),
    Kernel = static_cast<u32>(ThisNode) | static_cast<u32>(DirectReclaim),
};
USTL_ENABLE_ENUM_BITMASK(Gaf);

inline constexpr auto GAF_BOOT = Gaf::Boot;
inline constexpr auto GAF_USER = Gaf::User;
inline constexpr auto GAF_KERNEL = Gaf::Kernel;

enum class AllocationOptions : u32 {
    NoFragment = 0_bit,

    NoBlock = 1_bit,

    Contiguous = 2_bit,

    Kswapd = 3_bit,
};
USTL_ENABLE_ENUM_BITMASK(AllocationOptions);

} // namespace ours::mm

#endif // OURS_MM_GAF_HPP
