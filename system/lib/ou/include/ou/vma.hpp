/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_VMA_HPP
#define OU_VMA_HPP 1

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ou {
/// Reserves part of |parent| as a sub-region: its handle into |child|, its
/// address into |child_addr|. With Specific at |offset|, if |parent| has
/// CanMapSpecific; without it where the kernel finds room, |offset| 0.
/// CanMapRead/Write/Execute say what mappings in it may have; with none it
/// allows no mapping (a guard). CanMapSpecific lets it take Specific
/// placements in turn. Zircon zx_vmar_allocate().
auto vma_allocate(ours::HandleValue parent, ours::VmOptions options, usize offset, usize size,
                  ours::HandleValue *child, ours::VirtAddr *child_addr) -> ours::Status;

/// Maps [vmo_offset, vmo_offset + length) of |vmo| into |vma| and puts the
/// address in |mapped_addr|. |options| is PermRead, optionally with one of
/// PermWrite or PermExecute, plus Specific (at |vma_offset| from the region's
/// start, if the region has CanMapSpecific), MapRange (populate now) and
/// Sealed (no call can unmap or reprotect it, only the process's death takes
/// it down; the vDSO's VMO maps only this way, whole and PermRead |
/// PermExecute). |vmo| needs Map; each Perm bit needs the matching right on
/// both handles. Zircon zx_vmar_map().
auto vma_map(ours::HandleValue vma, ours::VmOptions options, usize vma_offset,
             ours::HandleValue vmo, u64 vmo_offset, usize length, ours::VirtAddr *mapped_addr)
    -> ours::Status;
} // namespace ou

#endif // #ifndef OU_VMA_HPP
