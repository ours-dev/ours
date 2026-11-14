/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_ALLOC_FLAGS_HPP
#define OURS_MM_ALLOC_FLAGS_HPP

#include <ours/mm/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
/// Physical-frame allocation flags, also carried through the heap and VMOs.
/// Bit values are preserved from the original allocation interface.
enum class Gaf : u32 {
    /// The caller's context permits blocking. It decides how deep an
    /// allocation may take a zone once memory is short: a caller that may
    /// block stops at a zone's Min watermark, one that may not at three
    /// quarters of it (AdmissionPass). It does not make an allocation wait
    /// for memory, and the allocator performs no direct reclaim.
    CanBlock = 7_bit,

    /// alloc_frames() may answer ShouldWait under memory pressure: when the
    /// free-memory gate is shut, or when every zone the request may use is
    /// below its Low watermark. The caller drops its locks before
    /// wait_for_frame_allocation(), which retries for one page. The
    /// mm::alloc_frames() block interface handles admission and returns the
    /// outcome without sleeping.
    CanWait = 8_bit,

    /// Allocate only from the explicit node or the current CPU's node if
    /// omitted. Skip the thread's placement policy. If the allowed mask
    /// excludes that node or it has no suitable frames, allocation fails.
    ThisNode = 9_bit,

    /// Return zeroed memory on success.
    Zero = 10_bit,
};
USTL_ENABLE_ENUM_BITMASK(Gaf);

/// The kernel caller permits blocking; this does not imply zeroing or a
/// request to wait under memory pressure.
inline constexpr auto ALLOC_KERNEL = Gaf::CanBlock;

/// Memory visible to a process is zeroed and fails rather than waits. It is
/// allocated from a system call, which may block.
inline constexpr auto ALLOC_USER = Gaf::CanBlock | Gaf::Zero;

/// User VMO pages may be delayed under pressure. The VMO's own objects still
/// use the immediate kernel allocation path.
inline constexpr auto ALLOC_USER_VMO = ALLOC_USER | Gaf::CanWait;

} // namespace ours::mm

#endif // OURS_MM_ALLOC_FLAGS_HPP
