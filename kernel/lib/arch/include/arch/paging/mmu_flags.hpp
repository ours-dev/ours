// arch/paging ARCH/PAGING_MMU_FLAGS_HPP
/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_MMU_FLAGS_HPP
#define ARCH_PAGING_MMU_FLAGS_HPP 1

#include <ustl/util/enum_bits.hpp>

namespace arch::paging {
enum class MmuFlags : usize {
    Present = 0_bit,
    Writable = 1_bit,
    Readable = 2_bit,
    Executable = 3_bit,
    PermMask = 1_bit | 2_bit | 3_bit,

    /// Only valid on some architectures.
    Discache = 5_bit,
    User = 4_bit,
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
