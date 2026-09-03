/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMMAP_INIT_HPP
#define OURS_MM_MEMMAP_INIT_HPP 1

#include <ours/init.hpp>
#include <ours/status.hpp>

namespace ours::mm {
/// Brings the selected memmap up and hands it every usable range the boot
/// allocator knows about. After this a pfn that is really there resolves to a
/// descriptor -- an uninitialised one.
INIT_CODE auto build_memmap() -> Status;

/// Fills in those descriptors, zone by zone.
///
/// Deliberately not a member of MemMap. Which zone a frame belongs to and what
/// its initial state is has nothing to do with how a pfn becomes a descriptor;
/// this pass only happens to call pfn_to_frame(). Linux draws the same line --
/// the equivalent lives in mm/mm_init.c, not in the per-model headers.
INIT_CODE auto init_memmap() -> void;

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMMAP_INIT_HPP
