/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_PAGE_TABLE_HPP
#define OURS_ARCH_X86_PAGE_TABLE_HPP 1

#include <ours/mem/mod.hpp>
#include <ours/mem/gaf.hpp>
#include <ours/mem/vm_page.hpp>
#include <ours/mem/memory-model.hpp>
#include <ours/mem/physmap.hpp>

#include <ours/sync/spin-lock.hpp>

#include <arch/page_table.hpp>
#include <ustl/bitfields.hpp>

namespace ours::mem {
    class MakePageTable {
        struct X86PageAllocator;
        struct X86MmuPageSynchroniser;
        struct X86EptPageSynchroniser;

        /// TODO(SmallHuaZi) We need to make a tracker helper class to trace all mapping action.
        using PageTable = arch::PageTable <
            arch::paging::PageAllocator<X86PageAllocator>,
            // A x86-custom option to give out a type of page synchroniser for MMU.
            arch::paging::X86MmuPageSynchroniser<X86MmuPageSynchroniser>,
            // A x86-custom option to give out a type of page synchroniser for EPT.
            arch::paging::X86EptPageSynchroniser<X86EptPageSynchroniser>,
            arch::paging::PhysToVirt<PhysMap>,
            arch::MutexT<sync::SpinLock>
        >;

        struct X86PageAllocator {
            static auto alloc_pages(usize nr_pages, usize align) -> PhysAddr {
                PhysAddr phys_addr;
                auto frame = alloc_frames(kGafKernel | Gaf::Zero, &phys_addr, 0); 
                if (!frame) {
                    return 0;
                }
                auto page = frame->as_ptr<VmPage>();
                page->increase_mappings();

                return phys_addr;
            }

            static auto free_page(PhysAddr phys_addr, usize nr_pages) -> void {
                if (auto frame = phys_to_frame(phys_addr)) {
                    auto page = frame->as_ptr<VmPage>();
                    page->decrease_mappings();
                    if (0 == page->num_mappings()) {
                        free_frames(frame, 0);
                    } 
                }
            }
        };

        struct X86MmuPageSynchroniser {
            typedef X86MmuPageSynchroniser  Self;
            X86MmuPageSynchroniser(PageTable::Mmu *mmu)
                : page_table_(mmu)
            {}

            auto sync(arch::paging::PendingInvalidationItems const &items) -> void;

            PageTable::Mmu *page_table_;
        };
      public:
        using Type = PageTable;
    };

    using PageTable = MakePageTable::Type;

} // namespace ours::mem

#endif // #ifndef OURS_ARCH_X86_PAGE_TABLE_HPP