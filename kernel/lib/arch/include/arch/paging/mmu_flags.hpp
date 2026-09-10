// arch/paging ARCH/PAGING_MMU_FLAGS_HPP
/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_MMU_FLAGS_HPP
#define ARCH_PAGING_MMU_FLAGS_HPP 1

#include <ours/macro_abi.hpp>

#include <ustl/util/enum_bits.hpp>

namespace arch::paging {
enum class MmuFlags : usize {
    Present = BIT(0),
    Writable = BIT(1),
    Readable = BIT(2),
    Executable = BIT(3),
    PermMask = BIT(1) | BIT(2) | BIT(3),

    /// Only valid on some architectures.
    Discache = BIT(5),
    User = BIT(4),
}; // enum class MmuFlags: usize
USTL_ENABLE_ENUM_BITMASK(MmuFlags);

/// Converts between the architecture-neutral MmuFlags and an architecture's
/// own flag representation. A trait struct rather than a function template
/// so the conversion can be partially specialized if a future architecture
/// ever needs that; specialized per architecture (see arch_mmu_flags.hpp
/// for the x86 specialization).
template <typename ArchFlags>
struct MmuFlagsConv;

} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_MMU_FLAGS_HPP
