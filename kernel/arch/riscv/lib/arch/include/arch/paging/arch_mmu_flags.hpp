/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_ARCH_MMU_FLAGS_HPP
#define ARCH_PAGING_ARCH_MMU_FLAGS_HPP 1

#include <arch/paging/mmu_flags.hpp>

#include <ustl/util/enum_bits.hpp>

namespace arch::paging {
/// The low eight bits of a riscv page-table entry, where the privileged
/// specification puts them. Bits 9:8 are left to software and the physical
/// page number starts at bit 10 (RiscvPte).
///
/// R, W and X are three permissions, not a "present" and a "no-execute" bit:
/// a valid entry with all three clear is not a page without access but a
/// pointer to the next table down, and W without R is reserved.
enum class RiscvMmuFlags : usize {
    Valid = 0_bit,
    Readable = 1_bit,
    Writable = 2_bit,
    Executable = 3_bit,
    User = 4_bit,
    Global = 5_bit,
    Accessed = 6_bit,
    Dirty = 7_bit,
};
USTL_ENABLE_ENUM_BITMASK(RiscvMmuFlags);

/// Each neutral permission is one riscv bit, so the conversion is bit for bit
/// both ways; what a leaf cannot encode (W without R, no permission at all)
/// is the entry's to settle when it is written (RiscvPte::make()).
///
/// Discache has no bit here. Without the page-based memory type extension a
/// riscv page cannot choose its cacheability: the platform's physical memory
/// attributes fix it per address range, and a device's registers are I/O
/// there whatever the page says. It is dropped on the way in and never
/// reported back.
template <>
struct MmuFlagsConv<RiscvMmuFlags> final {
    [[nodiscard]]
    static constexpr auto to_arch(MmuFlags const flags) -> RiscvMmuFlags {
        RiscvMmuFlags target{};
        if (!!(flags & MmuFlags::Present)) {
            target |= RiscvMmuFlags::Valid;
        }
        if (!!(flags & MmuFlags::Readable)) {
            target |= RiscvMmuFlags::Readable;
        }
        if (!!(flags & MmuFlags::Writable)) {
            target |= RiscvMmuFlags::Writable;
        }
        if (!!(flags & MmuFlags::Executable)) {
            target |= RiscvMmuFlags::Executable;
        }
        if (!!(flags & MmuFlags::User)) {
            target |= RiscvMmuFlags::User;
        }
        return target;
    }

    [[nodiscard]]
    static constexpr auto from_arch(RiscvMmuFlags const flags) -> MmuFlags {
        MmuFlags target{};
        if (!!(flags & RiscvMmuFlags::Valid)) {
            target |= MmuFlags::Present;
        }
        if (!!(flags & RiscvMmuFlags::Readable)) {
            target |= MmuFlags::Readable;
        }
        if (!!(flags & RiscvMmuFlags::Writable)) {
            target |= MmuFlags::Writable;
        }
        if (!!(flags & RiscvMmuFlags::Executable)) {
            target |= MmuFlags::Executable;
        }
        if (!!(flags & RiscvMmuFlags::User)) {
            target |= MmuFlags::User;
        }
        return target;
    }
};

using ArchMmuFlags = RiscvMmuFlags;
} // namespace arch::paging

#endif // ARCH_PAGING_ARCH_MMU_FLAGS_HPP
