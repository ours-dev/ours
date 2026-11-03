/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_X86_PAGINGS_HPP
#define ARCH_PAGING_X86_PAGINGS_HPP 1

#include <arch/paging/arch_mmu_flags.hpp>
#include <arch/paging/paging_dispatcher.hpp>
#include <arch/types.hpp>
#include <arch/x86/macro/mmu.hpp>

namespace arch::paging {
enum class X86PagingLevel : usize {
    PageTable,
    PageDirectory,
    PageDirectoryPointerTable,
    PageMapLevel4,
    PageMapLevel5,
};

struct X86PagingBase {
    using LevelType = X86PagingLevel;
    using Level = LevelType;

    static constexpr usize TABLE_ALIGNMENT_LOG2 = 12;
    static constexpr VirtAddrExt VIRT_ADDR_EXT = VirtAddrExt::Canonical;
    static constexpr Level ALL_LEVELS[] = {
        Level::PageTable,     Level::PageDirectory, Level::PageDirectoryPointerTable,
        Level::PageMapLevel4, Level::PageMapLevel5,
    };
};

template <X86PagingLevel Level>
struct X86Pte {
    using ValueType = PteVal;

    /// An entry pointing at |addr|: a page mapped with |flags| when |terminal|,
    /// otherwise the next table down.
    ///
    /// A leaf carries exactly the permissions it was asked for. This used to OR
    /// X86_KERNEL_PAGE_FLAGS -- Present, Accessed, Dirty, Writable and Global
    /// -- into every leaf, after MmuFlagsConv::to_arch() had worked out the
    /// right bits, so every page in the system was writable and global:
    ///
    ///  - Writable everywhere meant no read-only mapping was read-only to the
    ///    kernel. Kernel .text and .rodata, the IDT and GDT aliases, a
    ///    program's read-only segments and the vDSO could all be written from
    ///    ring 0, and a system call handed a pointer into the vDSO wrote its
    ///    answer there and returned Ok.
    ///  - Global on a user page meant its TLB entry survived the CR3 switch to
    ///    another process. Every program is mapped at the same addresses, so
    ///    the next one could run through the previous one's translation --
    ///    and once a dead process's frames are freed (VmAspace::destroy), read
    ///    and write whoever the allocator gives them to next.
    ///
    /// Global belongs to kernel mappings only, because only they are the same
    /// in every address space. This layer does not know which address space it
    /// is filling, but it does not need to: a leaf without User is a kernel
    /// page -- the kernel aspace maps nothing with User, and a user aspace maps
    /// nothing without it below the kernel half it shares -- so "not User" is
    /// exactly "kernel".
    ///
    /// Accessed is preset on every leaf, and Dirty on a writable one, so the
    /// CPU does not have to set them itself on the first access; nothing reads
    /// either bit yet.
    ///
    /// A table entry grants everything -- Writable and User, and never NX,
    /// since NX ANDs down the hierarchy -- because only the leaf may decide: an
    /// upper-level entry that withheld a permission would withhold it from
    /// every page beneath, and mapping a read-only segment first would then
    /// poison the executable one next to it. It used to be the leaf's own flags
    /// plus the kernel defaults, which made a table's permissions depend on
    /// whichever page happened to be mapped under it first.
    static constexpr auto make(PhysAddr addr, X86MmuFlags flags, bool terminal) -> X86Pte {
        auto const frame = static_cast<PteVal>(std::to_underlying(addr) & X86_PG_FRAME);
        if (!terminal) {
            constexpr auto TABLE_FLAGS =
                PteVal(X86_MMUF_PRESENT | X86_MMUF_ACCESSED | X86_MMUF_WRITABLE | X86_MMUF_USER);
            return {frame | TABLE_FLAGS};
        }

        auto value = frame | PteVal(flags) | PteVal(X86_MMUF_PRESENT | X86_MMUF_ACCESSED);
        if (!!(flags & X86MmuFlags::Writable)) {
            value |= PteVal(X86_MMUF_DIRTY);
        }
        if (!(flags & X86MmuFlags::User)) {
            value |= PteVal(X86_MMUF_GLOBAL);
        }
        if constexpr (Level != X86PagingLevel::PageTable) {
            value |= PteVal(X86_MMUF_PAGE_SIZE);
        }
        return {value};
    }

    constexpr auto address() const -> PhysAddr {
        return PhysAddr{inner_ & X86_PG_FRAME};
    }

    constexpr auto is_present() const -> bool {
        return (inner_ & X86_MMUF_PRESENT) != 0;
    }

    constexpr auto is_terminal() const -> bool {
        if (!is_present()) {
            return false;
        }
        if constexpr (Level == X86PagingLevel::PageTable) {
            return true;
        }
        if constexpr (Level == X86PagingLevel::PageDirectory ||
                      Level == X86PagingLevel::PageDirectoryPointerTable) {
            return (inner_ & X86_MMUF_PAGE_SIZE) != 0;
        }
        return false;
    }

    constexpr auto arch_mmu_flags() const -> X86MmuFlags {
        return static_cast<X86MmuFlags>(inner_ & X86_MMUF_MASK);
    }

    constexpr auto with_protection(X86MmuFlags flags) const -> X86Pte {
        constexpr auto PROTECTION_MASK =
            PteVal(X86_MMUF_WRITABLE | X86_MMUF_USER | X86_MMUF_DISCACHE | X86_MMUF_NOEXECUTABLE);
        return {(inner_ & ~PROTECTION_MASK) | (PteVal(flags) & PROTECTION_MASK)};
    }

    PteVal inner_ = 0;
};

template <X86PagingLevel TopLevel, usize MaxPhysAddrSize>
struct X86Paging: X86PagingBase {
    using Base = X86PagingBase;
    using Level = Base::Level;

    // Each paging scheme names its own PTE flag type, so the neutral layer
    // (PagingTraits, MapContext, ...) can reach it through the template
    // parameter instead of a global `ArchMmuFlags` alias.
    using ArchMmuFlags = X86MmuFlags;

    template <Level AtLevel>
    using Pte = X86Pte<AtLevel>;

    static constexpr Level PAGING_LEVEL = TopLevel;
    static constexpr usize MAX_PHYS_ADDR_SIZE = MaxPhysAddrSize;

    template <Level AtLevel>
    static constexpr usize NUM_PTES_LOG2 = 9;

    static constexpr usize PAGE_SIZE_MAP[] = {4_kb, 2_mb, 1_gb, 512_gb, 256_tb};
    static constexpr bool LEVEL_CAN_BE_TERMINAL_MAP[] = {true, true, true, false, false};

    static constexpr auto virt_to_index(Level level, VirtAddr virt) -> usize {
        constexpr usize SHIFT[] = {
            X86_PT_SHIFT, X86_PD_SHIFT, X86_PDP_SHIFT, X86_PML4_SHIFT, X86_PML5_SHIFT,
        };
        return (std::to_underlying(virt) >> SHIFT[usize(level)]) & (X86_MAX_PTES - 1);
    }

    static constexpr auto max_entries(Level) -> usize {
        return X86_MAX_PTES;
    }
    static constexpr auto level_can_be_terminal(Level level) -> bool {
        return LEVEL_CAN_BE_TERMINAL_MAP[usize(level)];
    }
    static constexpr auto phys_addr_from_pte(Level, PteVal pte) -> PhysAddr {
        return PhysAddr{pte & X86_PG_FRAME};
    }
    static constexpr auto arch_mmu_flags_from_pte(Level, PteVal pte) -> X86MmuFlags {
        return static_cast<X86MmuFlags>(pte & X86_MMUF_MASK);
    }
};

template <>
struct PagingDispatcher<X86PagingLevel::PageMapLevel4> {
    using Type = X86Paging<X86PagingLevel::PageMapLevel4, 52>;
};

template <>
struct PagingDispatcher<X86PagingLevel::PageMapLevel5> {
    using Type = X86Paging<X86PagingLevel::PageMapLevel5, 57>;
};
} // namespace arch::paging

#endif // ARCH_PAGING_X86_PAGINGS_HPP
