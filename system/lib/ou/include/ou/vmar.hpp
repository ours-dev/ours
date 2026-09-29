/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_VMAR_HPP
#define OU_VMAR_HPP 1

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ou {
/// Reserves [offset, offset + size) of |parent| as a sub-region: its handle
/// into |child|, its address into |child_addr|. |options| must be Specific;
/// the sub-region allows no mapping, so it keeps the range empty (a guard).
/// Zircon zx_vmar_allocate().
auto vmar_allocate(ours::HandleValue parent, ours::VmOptions options, usize offset, usize size,
                   ours::HandleValue *child, ours::VirtAddr *child_addr) -> ours::Status;

/// Maps [vmo_offset, vmo_offset + length) of |vmo| into |vmar| and puts the
/// address in |mapped_addr|. |options| is PermRead, optionally with one of
/// PermWrite or PermExecute, plus Specific (at |vmar_offset| from the region's
/// start) and MapRange (populate now). |vmo| needs Map; each Perm bit needs
/// the matching right on both handles. Zircon zx_vmar_map().
auto vmar_map(ours::HandleValue vmar, ours::VmOptions options, usize vmar_offset,
              ours::HandleValue vmo, u64 vmo_offset, usize length, ours::VirtAddr *mapped_addr)
    -> ours::Status;
} // namespace ou

#endif // #ifndef OU_VMAR_HPP
