/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_GAF_HPP
#define OURS_MM_GAF_HPP

#include <ours/mm/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
/// Shorthand of Getting Available Frames.
///
/// Only the bits something reads. The flags this once carried for DMA zones
/// (Dma, Dma32, Movable), retry policy (NoFail, NoRetry), page-cache dirtying
/// (Write), background reclaim (DaemonReclaim) and compound pages (Compound)
/// went: PmNode masked every one of them off before use, except
/// DaemonReclaim, which nothing read. The values of the survivors are
/// unchanged; nothing stores a Gaf.
enum class Gaf : u32 {
    /// The caller is able to block, so the allocator may turn it away from a
    /// zone that is running short and send it on to the next one, while a
    /// caller that cannot block is allowed to dip there instead. That gate is
    /// the watermark check in PmZone::alloc_frames(), and it reads
    /// this bit directly; nothing consults it yet, so the gate currently turns
    /// away every caller of a short zone. The AllocationOptions set that used
    /// to stand between the two is gone: it was always empty, and the place a
    /// request's derived policy belongs is the request (AllocRequest).
    ///
    /// It used to promise that the allocator itself would reclaim, compact and
    /// retry rather than fail -- "the allocator does the waiting, not its
    /// caller". It never did: both were stubs returning null, so the retry
    /// never ended and a kernel allocation that could not be served hung
    /// instead of being refused. Reclaim is not something this allocator can
    /// perform, because it has to reach into the layers above it, so the loop
    /// went; an allocation it cannot serve is refused now, and the waiting
    /// moves to the reclaimer once there is one.
    DirectReclaim = 7_bit,

    /// The caller can be told to wait. While free memory is short (below the
    /// level set_free_memory_signal() last set), try_alloc_frames() answers
    /// ShouldWait instead of taking a frame, and the caller lets go of every
    /// lock and waits in wait_for_frame_allocation(). The frames below that
    /// level are then left to the callers that cannot wait: the kernel's own.
    ///
    /// Not DirectReclaim, which says the caller may block here, inside the
    /// call. This one says the call returns at once and the waiting is done
    /// later, by the caller, where it holds no lock. Only try_alloc_frames()
    /// takes it, since only a Result can say ShouldWait.
    CanWait = 8_bit,

    /// Only the node the allocation starts from: the local node, or the one
    /// named by an alloc_frames(nid, ...) overload. When that node has nothing
    /// the allocation fails rather than spilling to another node. No preset
    /// carries it: local-first is already the zone walk's order
    /// (PmNode::m_fallback), so ask for this only where memory on another node
    /// would defeat the allocation's purpose.
    ThisNode = 9_bit,

    /// Return a zeroed frame on success.
    Zero = 10_bit,
};
USTL_ENABLE_ENUM_BITMASK(Gaf);

/// The presets live outside the enum, so that Gaf is only a set of bits and a
/// preset is a named combination of them. There is no boot preset: what boot
/// code needs beyond GAF_KERNEL is Zero, and it asks for that by name.
///
/// The kernel's own memory: may wait.
inline constexpr auto GAF_KERNEL = Gaf::DirectReclaim;

/// Memory a process will see: zeroed, and fails rather than waits.
inline constexpr auto GAF_USER = Gaf::Zero;

/// The pages of a VMO a process made: zeroed, and waiting while free memory
/// is short, so that the frames below the delay level stay the kernel's. Only
/// for the pages: the VMO's own objects come from the heap, which cannot wait.
inline constexpr auto GAF_USER_VMO = GAF_USER | Gaf::CanWait;

} // namespace ours::mm

#endif // OURS_MM_GAF_HPP
