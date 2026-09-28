/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_VMAR_HPP
#define OU_VMAR_HPP 1

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ou {
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
