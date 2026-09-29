/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/vma.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto vma_allocate(ours::HandleValue const parent, ours::VmOptions const options, usize const offset,
                  usize const size, ours::HandleValue *const child,
                  ours::VirtAddr *const child_addr) -> ours::Status {
    return ours_vma_allocate(parent, options, offset, size, child, child_addr);
}

auto vma_map(ours::HandleValue const vma, ours::VmOptions const options, usize const vma_offset,
             ours::HandleValue const vmo, u64 const vmo_offset, usize const length,
             ours::VirtAddr *const mapped_addr) -> ours::Status {
    return ours_vma_map(vma, options, vma_offset, vmo, vmo_offset, length, mapped_addr);
}
} // namespace ou
