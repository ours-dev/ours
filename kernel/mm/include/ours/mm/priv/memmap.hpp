/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_MEMORY_MODEL_HPP
#define OURS_MM_PRIV_MEMORY_MODEL_HPP 1

#include <ours/init.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/types.hpp>
#include <ours/status.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

#include <algorithm>

namespace ours::mm {
/// A run of frame descriptors covering consecutive pfns.
using FrameSpan = ustl::views::Span<PmFrame>;
} // namespace ours::mm

namespace ours::mm {
/// Section arithmetic.
///
/// A section is the granularity the sparse model allocates descriptors in, but
/// the number itself is not model-specific: `PmFrame` carries one, and
/// init_memmap_range() stamps it whichever model is in effect. So the pure
/// arithmetic lives here rather than beside the section table.

template <typename T>
FORCE_INLINE
constexpr auto phys_to_secnum(T phys_addr) -> SecNum {
    static_assert(sizeof(T) == sizeof(PhysAddr));
    return reinterpret_cast<PhysAddr>(phys_addr) >> SECTION_SHIFT;
}

FORCE_INLINE
constexpr auto secnum_to_phys(SecNum secnum) -> PhysAddr {
    return secnum << SECTION_SHIFT;
}

FORCE_INLINE
constexpr auto pfn_to_secnum(Pfn pfn) -> SecNum {
    return pfn >> PFN_SECTION_SHIFT;
}

FORCE_INLINE
constexpr auto secnum_to_pfn(SecNum secnum) -> Pfn {
    return secnum << PFN_SECTION_SHIFT;
}
} // namespace ours::mm

namespace ours::mm::priv {

/// An alternative home for a range's frame descriptors.
///
/// Placing the memmap of a device's memory inside that device rather than in
/// ordinary RAM is the reason this exists; a null Altmap means "use the normal
/// allocator".
struct Altmap {
    PhysAddr start;
    PhysAddr end;
};

} // namespace ours::mm::priv

#endif // #ifndef OURS_MM_PRIV_MEMORY_MODEL_HPP
