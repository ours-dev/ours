/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_VFLAT_MEMMAP_HPP
#define OURS_MM_PRIV_VFLAT_MEMMAP_HPP 1

#include <ours/mm/priv/memmap.hpp>

namespace ours::mm::priv {
/// The virtually-flat memory model.
///
/// One array of `PmFrame`, indexed straight by pfn, laid out in a reserved
/// virtual window. Nothing is looked up: a pfn becomes a descriptor with a
/// shift and an add, which is the whole point of this model and what makes it
/// worth having next to SparseMemMap's multi-level section table.
///
/// The flatness is virtual only. The window is address space, not memory: the
/// pages behind the stretch of it that a real pfn range needs are mapped by
/// populate_memmap(), and the holes between ranges are simply never backed. So
/// a sparse machine costs address space rather than descriptors, which on a
/// 64-bit kernel is the cheaper of the two.
class VflatMemMap {
    using Self = VflatMemMap;

  public:
    /// The one instance.
    FORCE_INLINE
    static auto get() -> Self & {
        static Self MODEL;
        return MODEL;
    }

    /// How far init_memmap() rounds the tail of the last range up: one page of
    /// descriptors, the unit this model backs the window in.
    constexpr static usize FRAMES_PER_MEMMAP_CHUNK = PAGE_SIZE / sizeof(PmFrame);

    /// Reserves the window. No table to build -- the translation is arithmetic.
    INIT_CODE
    auto init() -> Status;

    auto add_range(PhysAddr start, PhysAddr end, NnId nid) -> Status;

    auto remove_range(PhysAddr start, PhysAddr end, NnId nid) -> Status;

    auto exist(Pfn pfn) const -> bool;

    auto dump() -> void;

    /// Backs the descriptors for `nr_frames` frames from `start` with real pages.
    MEM_INIT_CODE
    auto populate_memmap(Pfn start, usize nr_frames, NnId, Altmap *)
        -> ustl::Result<FrameSpan, Status>;

    MEM_INIT_CODE
    auto depopulate_memmap(Pfn start, usize nr_frames, NnId) -> Status;

    auto pfn_to_frame(Pfn pfn) -> PmFrame *;

    auto frame_to_pfn(PmFrame const *frame) -> Pfn;
};

} // namespace ours::mm::priv

#endif // #ifndef OURS_MM_PRIV_VFLAT_MEMMAP_HPP
