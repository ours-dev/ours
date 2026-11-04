/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_INIT_STAGE_HPP
#define OURS_MM_INIT_STAGE_HPP 1

#include <ours/compiler.hpp>

/// How far memory-management bring-up has got.
///
/// Split out of <ours/mm/init.hpp> because that header pulls in
/// <ours/phys/handoff.hpp> for the handoff declarations, and the stage alone
/// has no business dragging the phys headers into everything that asks. pmm.hpp
/// derives pmm_enabled() and heap_enabled() from this, and pmm.hpp is included
/// nearly everywhere.
namespace ours::mm {
enum class InitStage {
    BootStrap,

    /// The buddy allocator owns physical memory: EarlyMem::do_handoff() has
    /// given it every range bootmem still called free.
    ///
    /// This is a fact about who owns the frames, and nothing else. It used to
    /// double as "the kernel heap is up", which is a later and unrelated
    /// event, and one stage cannot be both: between the handoff and the heap
    /// sits CpuLocal::init(), and alloc_page_table() read the combined stage as
    /// "the buddy is not up yet" and went to EarlyMem -- for memory the buddy
    /// already had. Every page table taken in that window was handed out twice,
    /// and the second taker (a per-CPU unit allocated with Gaf::Zero) zeroed a
    /// live leaf table.
    Pmm,

    /// The object caches, kmalloc and ktl's memory resource exist, so `new`
    /// works. Strictly after Pmm: the heap is built on the page allocator.
    Heap,

    Vmm,
    All,
};

extern InitStage INIT_STAGE;

FORCE_INLINE
inline auto get_init_stage() -> InitStage {
    return INIT_STAGE;
}

FORCE_INLINE
inline auto set_init_stage(InitStage stage) -> void {
    INIT_STAGE = stage;
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_INIT_STAGE_HPP
