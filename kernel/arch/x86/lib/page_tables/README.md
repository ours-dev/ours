# page_tables

x86's page tables: the walks of a page table, written once in a CRTP base (`page_tables::X86PageTableImpl<Derived>`) and shaped by the kind of table that derives from it. The kernel's MMU page table, `ours::mm::PageTable`, is the one kind today.

The walks hold no state and take no lock. The derived class owns the root table and the lock, says how a table is reached and what a leaf's bits mean, and calls the walks under its lock. The level is a run-time argument, so each walk is emitted once for all levels.

## Layout

    include/page_tables/   public headers, reached as <page_tables/...>
    test/                  host-side tests, built with googletest

Header-only: the walks are templates on the derived class.

## Building and testing

The library is `//kernel/arch/x86/lib/page_tables`. Its tests are `//kernel/arch/x86/lib/page_tables/test:page_tables`, which `//:default` builds in the host toolchain; they run on tables built by hand in host memory, and compare the answers with the generic paging engine's.

The kernel's x86 toolchains put its include directory on every translation unit (`build/switch/kernel_switchset.gni`), since `ours/arch/x86/page_table.hpp` is reached from most of the kernel through the address space.
