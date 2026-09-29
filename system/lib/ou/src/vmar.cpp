/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/vmar.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto vmar_allocate(ours::HandleValue const parent, ours::VmOptions const options,
                   usize const offset, usize const size, ours::HandleValue *const child,
                   ours::VirtAddr *const child_addr) -> ours::Status {
    return ours_vmar_allocate(parent, options, offset, size, child, child_addr);
}

auto vmar_map(ours::HandleValue const vmar, ours::VmOptions const options, usize const vmar_offset,
              ours::HandleValue const vmo, u64 const vmo_offset, usize const length,
              ours::VirtAddr *const mapped_addr) -> ours::Status {
    return ours_vmar_map(vmar, options, vmar_offset, vmo, vmo_offset, length, mapped_addr);
}
} // namespace ou
