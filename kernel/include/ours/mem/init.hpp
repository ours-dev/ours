/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_INIT_HPP
#define OURS_MEM_INIT_HPP 1

#include <ours/init.hpp>
#include <ours/status.hpp>
#include <ours/phys/handoff.hpp>

#include <ustl/views/span.hpp>

#include <ours/phys/arch-bootmem.hpp>

#include <gktl/range.hpp>

namespace ours::mem {
    enum class InitStage {
        BootStrap,
        Pmm,
        Vmm,
        All, 
    };

    extern InitStage g_init_stage;

    FORCE_INLINE
    auto get_init_stage() -> InitStage {
        return g_init_stage;
    }

    FORCE_INLINE
    auto set_init_stage(InitStage stage) -> void {
        g_init_stage = stage;
    }

    enum class InitContext {
        Early,
        Hotplug,
    };

    INIT_CODE
    auto handoff_early_pmm(phys::MemoryHandoff &) -> Status;

    /// Perform the initialization of physical memory manager. Call this function after
    /// invoking the `setup_nodes`.
    ///
    /// |zpis| gives out of the priority topology among non-overlapping memory 
    /// regions. Such as the following example showed:
    ///     [0x0, 0xFFFF]           MemoryPriority::Low,
    ///     [0x10000, 0x1FFFF]      MemoryPriority::Normal,
    ///     [0x20000, 0x3FFFF]      MemoryPriority::High,
    ///     [0x40000, 0x5FFFF]      MemoryPriority::Critical,
    ///     [0x60000, 0x7FFFF]      MemoryPriority::Normal,
    /// But it is not required, 
    ///
    INIT_CODE
    auto init_pmm(ustl::views::Span<Pfn> pfn = {}) -> void;

    /// Perform the initialization of virtual memory manager.
    INIT_CODE
    auto init_vmm() -> void;

    enum class RemTag {
        All,
        Mem,
        Task,
        Sched,
    };
    INIT_CODE
    auto reclaim_init_area(RemTag = RemTag::All) -> Status;

} // namespace ours::mem

#endif // #ifndef OURS_MEM_INIT_HPP