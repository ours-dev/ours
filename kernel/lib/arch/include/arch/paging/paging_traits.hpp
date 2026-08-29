/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_PAGING_TRAITS_HPP
#define ARCH_PAGING_PAGING_TRAITS_HPP 1

#include <arch/paging/paging_dispatcher.hpp>

namespace arch::paging {

// A read-only view over an architecture's paging policy (e.g. X86Paging):
// derived/computed constants (TABLE_ALIGNMENT, NUM_PTES<Level>, ...) plus
// forwarding accessors, all expressed in terms of `ArchPaging`. Deliberately
// composition rather than inheritance — a traits type describes ArchPaging,
// it isn't one, so it references it through the `T` alias instead of
// deriving from it.
template <typename ArchPaging>
class PagingTraits final {
    using T = ArchPaging;

  public:
    using Level = typename T::Level;
    using LevelType = Level;

    // The architecture's own PTE flag type, named by ArchPaging itself. Taken
    // from the template parameter rather than a global alias, so this header
    // stays free of any architecture-specific include.
    using ArchMmuFlags = typename T::ArchMmuFlags;

    static constexpr auto MAX_PHYS_ADDR_SIZE = T::MAX_PHYS_ADDR_SIZE;
    static constexpr auto PAGING_LEVEL = T::PAGING_LEVEL;
    static constexpr auto TABLE_ALIGNMENT_LOG2 = T::TABLE_ALIGNMENT_LOG2;
    static constexpr auto VIRT_ADDR_EXT = T::VIRT_ADDR_EXT;
    static constexpr auto TABLE_ALIGNMENT = BIT(TABLE_ALIGNMENT_LOG2);

    template <Level Level>
    static constexpr auto NUM_PTES_LOG2 = T::template NUM_PTES_LOG2<Level>;

    template <Level Level>
    static constexpr auto NUM_PTES = BIT(NUM_PTES_LOG2<Level>);

    template <Level Level>
    using Pte = typename T::template Pte<Level>;

    // A hardware page table is a fixed-size contiguous PTE array.  Keeping
    // that representation explicit avoids imposing a container ABI on code
    // running before the kernel allocator exists.
    template <Level Level>
    using Table = Pte<Level>[NUM_PTES<Level>];

    template <Level Level>
    using PteVal = typename Pte<Level>::ValueType;

    template <Level Level>
    static constexpr auto SIZE_OF_PTE = sizeof(PteVal<Level>);

    template <Level Level>
    static constexpr auto SIZE_OF_TABLE = NUM_PTES<Level> * SIZE_OF_PTE<Level>;

    static constexpr auto FINAL_LEVEL = T::ALL_LEVELS[0];

    template <Level Level>
    static constexpr auto NEXT_LEVEL = T::ALL_LEVELS[usize(Level) - 1];

    /// The methods below provide the capability to dynamically dispatch
    /// handling logic across different levels of the page table.
    static constexpr auto next_level(Level level) -> Level {
        return T::ALL_LEVELS[usize(level) - 1];
    }

    static constexpr auto virt_to_index(Level level, VirtAddr addr) -> usize {
        return T::virt_to_index(level, addr);
    }

    static constexpr auto page_size(Level level) -> usize {
        return T::PAGE_SIZE_MAP[usize(level)];
    }

    static constexpr auto max_entries(Level level) -> usize {
        return T::max_entries(level);
    }

    static constexpr auto arch_mmu_flags_from_pte(Level level, PteVal<FINAL_LEVEL> pte)
        -> ArchMmuFlags {
        return T::arch_mmu_flags_from_pte(level, pte);
    }

    static constexpr auto phys_addr_from_pte(Level level, PteVal<FINAL_LEVEL> pte) -> PhysAddr {
        return T::phys_addr_from_pte(level, pte);
    }

    static constexpr auto level_can_be_terminal(Level level) -> bool {
        return T::level_can_be_terminal(level);
    }
};

template <auto PagingLevel, typename... Options>
struct MakePagingTraits final {
    using Type = PagingTraits<typename PagingDispatcher<PagingLevel, Options...>::Type>;
};

} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_PAGING_TRAITS_HPP
