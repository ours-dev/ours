/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_RISCV_PAGINGS_HPP
#define ARCH_PAGING_RISCV_PAGINGS_HPP 1

#include <arch/paging/arch_mmu_flags.hpp>
#include <arch/paging/paging_dispatcher.hpp>
#include <arch/types.hpp>

namespace arch::paging {
/// The levels of a riscv page table, numbered as the privileged
/// specification's translation algorithm numbers them: an entry at level i
/// maps 4 KiB << 9i -- a page at level 0, a 2 MiB megapage at 1, a 1 GiB
/// gigapage at 2 -- and is picked by the nine virtual address bits from
/// 12 + 9i up. A mode walks down from its top level: Sv39 from level 2,
/// Sv48 from 3, Sv57 from 4.
enum class RiscvPagingLevel : usize {
    Level0,
    Level1,
    Level2,
    Level3,
    Level4,
};

/// satp's MODE field, bits 63:60: how the hart translates S-mode and U-mode
/// addresses.
enum class RiscvSatpMode : u64 {
    Bare = 0,
    Sv39 = 8,
    Sv48 = 9,
    Sv57 = 10,
};

/// An entry of a riscv page table at |Level|: the eight flag bits of
/// RiscvMmuFlags, two bits left to software, and the physical page number in
/// bits 53:10. Bits 63:54 -- reserved, and the memory-type and contiguity
/// bits of extensions this layer does not use -- are always written zero.
///
/// The format is the same at every level. Whether an entry is a leaf is read
/// from its R, W and X bits, not from where it sits or from a size bit.
template <RiscvPagingLevel Level>
struct RiscvPte {
    using ValueType = PteVal;

    /// An entry pointing at |addr|: a page mapped with |flags| when
    /// |terminal|, otherwise the next table down.
    ///
    /// A table entry is Valid and the address, nothing else. R=W=X=0 is what
    /// makes it a pointer; the specification reserves U, A and D in it, which
    /// software must keep clear; and a permission there would mean nothing
    /// anyway, since riscv does not narrow permissions down the hierarchy:
    /// only the leaf decides. Global is left off too: on a table entry it
    /// would declare every page below it global, which only the leaves know.
    ///
    /// A leaf carries the access it was asked for (leaf_access()), and:
    ///  - Accessed always, and Dirty when writable. The specification lets a
    ///    hart either set them itself or raise a page fault when it finds one
    ///    clear, and nothing here handles that fault. Nothing reads either
    ///    bit yet, so they are preset rather than tracked.
    ///  - Global when not User. Only kernel mappings are the same in every
    ///    address space, so only they may keep a TLB entry across a satp
    ///    switch; a user page marked global would let the next address space
    ///    run through the previous one's translation. "Not User" is exactly
    ///    "kernel", since a kernel address space maps nothing with User and a
    ///    user one maps nothing without it below the kernel half it shares.
    static constexpr auto make(PhysAddr const addr, RiscvMmuFlags const flags, bool const terminal)
        -> RiscvPte {
        auto const frame = ((addr >> FRAME_SHIFT) << PPN_SHIFT) & PPN_MASK;
        if (!terminal) {
            return {frame | PteVal(RiscvMmuFlags::Valid)};
        }

        auto value =
            frame | leaf_access(flags) | PteVal(RiscvMmuFlags::Valid | RiscvMmuFlags::Accessed);
        if ((value & PteVal(RiscvMmuFlags::Writable)) != 0) {
            value |= PteVal(RiscvMmuFlags::Dirty);
        }
        if (!(flags & RiscvMmuFlags::User)) {
            value |= PteVal(RiscvMmuFlags::Global);
        }
        return {value};
    }

    [[nodiscard]]
    constexpr auto address() const -> PhysAddr {
        return ((inner & PPN_MASK) >> PPN_SHIFT) << FRAME_SHIFT;
    }

    [[nodiscard]]
    constexpr auto is_present() const -> bool {
        return (inner & PteVal(RiscvMmuFlags::Valid)) != 0;
    }

    /// A valid entry with any of R, W or X is a leaf; with none it points at
    /// the next table. W alone is reserved -- the hardware faults on it --
    /// and make() never writes it, but it is still not a pointer, so a walk
    /// must not go down through it into what is a data page.
    [[nodiscard]]
    constexpr auto is_terminal() const -> bool {
        return is_present() && (inner & PERMISSIONS) != 0;
    }

    [[nodiscard]]
    constexpr auto arch_mmu_flags() const -> RiscvMmuFlags {
        return static_cast<RiscvMmuFlags>(inner & FLAGS_MASK);
    }

    /// The entry with R, W, X and U as a leaf asked for |flags| would have
    /// them (leaf_access()), and everything else kept: the frame, Valid,
    /// Global and Accessed. Dirty is kept even where W goes, since it only
    /// says the page may have been written, and added where W is set.
    [[nodiscard]]
    constexpr auto with_protection(RiscvMmuFlags const flags) const -> RiscvPte {
        constexpr auto PROTECTION_MASK = PERMISSIONS | PteVal(RiscvMmuFlags::User);
        auto value = (inner & ~PROTECTION_MASK) | leaf_access(flags);
        if ((value & PteVal(RiscvMmuFlags::Writable)) != 0) {
            value |= PteVal(RiscvMmuFlags::Dirty);
        }
        return {value};
    }

    PteVal inner = 0;

  private:
    /// The physical page number counts 4 KiB frames whatever the kernel's own
    /// page size, and is 44 bits wide: physical addresses are 56 bits.
    static constexpr usize FRAME_SHIFT = 12;
    static constexpr usize PPN_SHIFT = 10;
    static constexpr usize PPN_BITS = 44;
    static constexpr PteVal PPN_MASK = ((PteVal{1} << PPN_BITS) - 1) << PPN_SHIFT;
    static constexpr PteVal FLAGS_MASK = 0xff;
    static constexpr PteVal PERMISSIONS =
        PteVal(RiscvMmuFlags::Readable | RiscvMmuFlags::Writable | RiscvMmuFlags::Executable);

    /// The R, W, X and U bits of a leaf asked for |flags|. Two requests have
    /// no leaf encoding, and both get read access added rather than an entry
    /// that means something else:
    ///  - W without R is reserved: the hardware faults on any access through
    ///    it, reads and writes alike.
    ///  - None of R, W and X is not a page without access but a pointer: the
    ///    hardware, and this engine's walk, would read the frame as the next
    ///    table down.
    /// No access at all is expressed by unmapping the page. A page this layer
    /// maps is at least readable, as it is on x86, which has no way to say
    /// otherwise; execute-only, which riscv can say, stays execute-only.
    static constexpr auto leaf_access(RiscvMmuFlags const flags) -> PteVal {
        constexpr auto READABLE = PteVal(RiscvMmuFlags::Readable);
        constexpr auto WRITABLE = PteVal(RiscvMmuFlags::Writable);
        auto access = PteVal(flags) & (PERMISSIONS | PteVal(RiscvMmuFlags::User));
        if ((access & PERMISSIONS) == 0 || (access & (READABLE | WRITABLE)) == WRITABLE) {
            access |= READABLE;
        }
        return access;
    }
};

/// The paging scheme a hart runs when satp's MODE is |Mode|, walked down from
/// |TopLevel|.
///
/// One root translates the whole address space, so the lower and upper halves
/// are one scheme, told apart by sign extension (VirtAddrExt::Canonical):
/// every bit above the top level's index must equal its highest bit. For
/// Sv39 that is bit 38, which leaves 256 GiB at the bottom of the address
/// space and 256 GiB at the top.
template <RiscvPagingLevel TopLevel, RiscvSatpMode Mode>
struct RiscvPaging final {
    using LevelType = RiscvPagingLevel;
    using Level = LevelType;

    // Each paging scheme names its own PTE flag type, so the neutral layer
    // (PagingTraits, MapContext, ...) can reach it through the template
    // parameter instead of a global `ArchMmuFlags` alias.
    using ArchMmuFlags = RiscvMmuFlags;

    template <Level AtLevel>
    using Pte = RiscvPte<AtLevel>;

    static constexpr Level PAGING_LEVEL = TopLevel;
    static constexpr RiscvSatpMode SATP_MODE = Mode;

    /// 44 bits of physical page number over 4 KiB frames, in every mode.
    static constexpr usize MAX_PHYS_ADDR_SIZE = 56;

    static constexpr usize TABLE_ALIGNMENT_LOG2 = 12;
    static constexpr VirtAddrExt VIRT_ADDR_EXT = VirtAddrExt::Canonical;
    static constexpr Level ALL_LEVELS[] = {
        Level::Level0, Level::Level1, Level::Level2, Level::Level3, Level::Level4,
    };

    template <Level AtLevel>
    static constexpr usize NUM_PTES_LOG2 = 9;

    static constexpr usize PAGE_SIZE_MAP[] = {4_kb, 2_mb, 1_gb, 512_gb, 256_tb};

    static constexpr auto virt_to_index(Level const level, VirtAddr const virt) -> usize {
        constexpr usize PAGE_OFFSET_BITS = 12;
        constexpr usize INDEX_BITS = 9;
        auto const shift = PAGE_OFFSET_BITS + INDEX_BITS * usize(level);
        return (virt >> shift) & ((usize{1} << INDEX_BITS) - 1);
    }

    static constexpr auto max_entries(Level) -> usize {
        return usize{1} << NUM_PTES_LOG2<Level::Level0>;
    }

    /// Any level may hold a leaf, as long as its frame is aligned to the size
    /// it maps -- the hardware faults on a misaligned one, and map() only
    /// writes a leaf where both addresses are aligned.
    static constexpr auto level_can_be_terminal(Level) -> bool {
        return true;
    }

    static constexpr auto phys_addr_from_pte(Level, PteVal const pte) -> PhysAddr {
        return Pte<Level::Level0>{pte}.address();
    }

    static constexpr auto arch_mmu_flags_from_pte(Level, PteVal const pte) -> RiscvMmuFlags {
        return Pte<Level::Level0>{pte}.arch_mmu_flags();
    }

    /// The satp value that turns this scheme on with the table at |root| as
    /// the top level, for address space |asid|: MODE in bits 63:60, the ASID
    /// in 59:44, and the root's physical page number in 43:0. |root| must be
    /// 4 KiB aligned.
    [[nodiscard]]
    static constexpr auto satp(PhysAddr const root, u16 const asid) -> u64 {
        constexpr usize MODE_SHIFT = 60;
        constexpr usize ASID_SHIFT = 44;
        constexpr u64 PPN_MASK = (u64{1} << ASID_SHIFT) - 1;
        return (static_cast<u64>(Mode) << MODE_SHIFT) | (u64{asid} << ASID_SHIFT) |
               ((static_cast<u64>(root) >> 12) & PPN_MASK);
    }
};

/// Sv39: three levels walked from level 2, 4 KiB pages with 2 MiB and 1 GiB
/// leaves, a 39-bit virtual address space.
template <>
struct PagingDispatcher<RiscvPagingLevel::Level2> {
    using Type = RiscvPaging<RiscvPagingLevel::Level2, RiscvSatpMode::Sv39>;
};
} // namespace arch::paging

#endif // ARCH_PAGING_RISCV_PAGINGS_HPP
