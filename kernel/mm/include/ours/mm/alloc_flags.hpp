/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_ALLOC_FLAGS_HPP
#define OURS_MM_ALLOC_FLAGS_HPP

#include <ours/mm/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
/// Physical-frame allocation flags, also carried through the heap and VMOs.
/// Bit values are preserved from the original allocation interface.
enum class AllocFlags : u32 {
    /// The caller's context permits blocking. Reserved for zone admission
    /// policy; the current allocator does not consult this bit or perform
    /// direct reclaim. It does not make an allocation wait for memory.
    CanBlock = 7_bit,

    /// try_alloc_frames() may answer ShouldWait under memory pressure. The
    /// caller drops its locks before wait_for_frame_allocation(), which
    /// retries for one page. Only the Result-returning entry accepts this
    /// flag: a pointer cannot distinguish ShouldWait from allocation failure.
    CanWait = 8_bit,

    /// If the allowed mask includes the home node, use only that node, even
    /// when it has no suitable frames. Otherwise use the caller's mask as given.
    /// The home is the explicit node or, if omitted, the current CPU's node.
    HomeIfAllowed = 9_bit,

    /// Return zeroed memory on success.
    Zero = 10_bit,
};
USTL_ENABLE_ENUM_BITMASK(AllocFlags);

/// The kernel caller permits blocking; this does not imply zeroing or a
/// request to wait under memory pressure.
inline constexpr auto ALLOC_KERNEL = AllocFlags::CanBlock;

/// Memory visible to a process is zeroed and fails rather than waits.
inline constexpr auto ALLOC_USER = AllocFlags::Zero;

/// User VMO pages may be delayed under pressure. The VMO's own objects still
/// use the immediate kernel allocation path.
inline constexpr auto ALLOC_USER_VMO = ALLOC_USER | AllocFlags::CanWait;

} // namespace ours::mm

#endif // OURS_MM_ALLOC_FLAGS_HPP
