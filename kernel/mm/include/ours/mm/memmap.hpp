/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMMAP_HPP
#define OURS_MM_MEMMAP_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/types.hpp>

/// The one public door onto the memory model.
///
/// Both implementations live under `ours/mm/priv/`; nothing outside this header
/// should name either of them. Callers use `MemMap` and the free functions
/// below, which is what keeps the choice of model from leaking into the rest of
/// the kernel.
#if MM_VFLAT_MEMMAP
#include <ours/mm/priv/vflat_memmap.hpp>
#else
#include <ours/mm/priv/sparse_memmap.hpp>
#endif

namespace ours::mm {
/// The model this kernel was built with.
///
/// A name, not a base class: the choice is made once at build time, so every
/// call site below compiles straight into the selected implementation with no
/// dispatch of any kind. `mm_vflat_memmap` in kernel/kernel_params.gni is the
/// switch; see config("memory") in kernel/products/BUILD.gn for where it turns
/// into this define.
#if MM_VFLAT_MEMMAP
using MemMap = priv::VflatMemMap;
#else
using MemMap = priv::SparseMemMap;
#endif

/// Accessors for conveniency.

/// Whether `pfn` has a descriptor behind it at all. The counterpart of Linux's
/// pfn_valid(); a pfn inside a zone's span need not be backed.
FORCE_INLINE inline auto pfn_valid(Pfn pfn) -> bool {
    return MemMap::get().exist(pfn);
}

FORCE_INLINE constexpr auto pfn_to_frame(Pfn pfn) -> PmFrame * {
    return MemMap::get().pfn_to_frame(pfn);
}

template <typename T>
FORCE_INLINE constexpr auto phys_to_frame(T phys_addr) -> PmFrame * {
    return pfn_to_frame(phys_to_pfn(phys_addr));
}

FORCE_INLINE constexpr auto frame_to_pfn(PmFrame const *frame) -> Pfn {
    return MemMap::get().frame_to_pfn(frame);
}

FORCE_INLINE constexpr auto frame_to_phys(PageFrame const *frame) -> PhysAddr {
    // Deliberately not `frame->as_ptr<PmFrame>()`. That stamps Role::Pmm as a
    // side effect of converting -- and the buddy allocator reads Role::Pmm as
    // "this frame is free" (see FrameBuddy::is_mergeable / insert_frame /
    // remove_frame). Taking the physical address of an *allocated* frame
    // therefore re-marked it free, so the next release of its buddy would
    // coalesce live memory into a free block and hand it out twice.
    //
    // Asking for a frame's address must not change the frame. The cast below
    // is only about types: the memmap really is an array of PmFrame.
    return pfn_to_phys(frame_to_pfn(static_cast<PmFrame const *>(frame)));
}

template <typename T>
FORCE_INLINE constexpr auto virt_to_frame(T virt_addr) -> PmFrame * {
    return phys_to_frame(PhysMap::virt_to_phys(virt_addr));
}

FORCE_INLINE constexpr auto frame_to_virt(PmFrame *frame) -> VirtAddr {
    return PhysMap::phys_to_virt(frame_to_phys(frame));
}

template <typename T>
FORCE_INLINE constexpr auto frame_to_virt(PmFrame *frame) -> T * {
    DEBUG_ASSERT(frame->phys_size() >= sizeof(T), "T is too large");
    return PhysMap::phys_to_virt<T>(frame_to_phys(frame));
}

/// Walks from any frame of a multi-frame block back to its head -- the
/// descriptor that actually carries the block's metadata.
///
/// A buddy block of order N is 2^N frames long, but a tail descriptor does not
/// record N, so the head cannot be found by masking. Instead each tail records
/// its distance from the head (see PageFrame::compound_offset), and this is
/// just that subtraction. A frame that is not part of a larger block reports a
/// zero offset, so this is the identity for it.
FORCE_INLINE inline auto frame_to_folio(PmFrame *frame) -> PmFolio * {
    return frame ? frame - frame->compound_offset() : nullptr;
}

/// Resolves an address anywhere inside a block to the block's head descriptor.
/// kfree relies on this: it is handed an interior object pointer and has to
/// reach the slab metadata, which lives only on the head.
template <typename T>
FORCE_INLINE auto virt_to_folio(T virt_addr) -> PmFolio * {
    return frame_to_folio(phys_to_frame(PhysMap::virt_to_phys(virt_addr)));
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMMAP_HPP
