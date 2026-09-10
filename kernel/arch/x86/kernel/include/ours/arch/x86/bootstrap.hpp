/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_BOOTSTRAP_HPP
#define OURS_ARCH_X86_BOOTSTRAP_HPP 1

#define X86_BOOTSTRAP_OFFSET_PGD 0
#define X86_BOOTSTRAP_OFFSET_GDTR 10
#define X86_BOOTSTRAP_OFFSET_LONGMODE_PHYS_ENTRY 16
#define X86_BOOTSTRAP_OFFSET_LONGMODE_VIRT_ENTRY 24
#define X86_BOOTSTRAP_OFFSET_STACK_TOP 32
#define X86_BOOTSTRAP_OFFSET_CPUNUM 40

#define X86_BOOTSTRAP_OFFSET_THREAD 48

#if !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)

#include <ours/init.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/types.hpp>

#include <arch/x86/descriptor.hpp>
#include <ustl/sync/atomic.hpp>

namespace ours {
namespace task {
    class Thread;
}

struct BootstrapData {
    u32 pgd;
    u32 _padding0;

    u16 _padding1;
    u16 gdtr_limit;
    u32 gdtr_base;

    u32 long_mode_phys_entry;
    u32 lond_mode_cs;

    // High half address space.
    VirtAddr long_mode_virt_entry;

    /// What the CPU being started needs before it can run C: the stack to run
    /// on, its own number, and the Thread it is to adopt.
    ///
    /// One slot, not a table indexed by CPU. x86_bring_aps() starts one AP at
    /// a time -- it sends the SIPI and then spins until that CPU reports
    /// online before it touches this page again -- so only one CPU's values
    /// are ever live. A `percpu[2 * MP_MAX_CPUS]` array of (stack, thread)
    /// pairs indexed by `cpunum - 1` sat here instead, which bought nothing
    /// and cost an index convention that three files had to agree on, plus
    /// the arithmetic to apply it in 64-bit assembly with no symbols.
    VirtAddr stack_top;
    CpuNum cpunum;
    u32 padding3_;
    VirtAddr thread;
};
static_assert(offsetof(BootstrapData, pgd) == X86_BOOTSTRAP_OFFSET_PGD);
static_assert(offsetof(BootstrapData, gdtr_limit) == X86_BOOTSTRAP_OFFSET_GDTR);
static_assert(offsetof(BootstrapData, long_mode_phys_entry) ==
              X86_BOOTSTRAP_OFFSET_LONGMODE_PHYS_ENTRY);
static_assert(offsetof(BootstrapData, long_mode_virt_entry) ==
              X86_BOOTSTRAP_OFFSET_LONGMODE_VIRT_ENTRY);
static_assert(offsetof(BootstrapData, stack_top) == X86_BOOTSTRAP_OFFSET_STACK_TOP);
static_assert(offsetof(BootstrapData, cpunum) == X86_BOOTSTRAP_OFFSET_CPUNUM);
static_assert(offsetof(BootstrapData, thread) == X86_BOOTSTRAP_OFFSET_THREAD);

constexpr static auto MIN_BOOTSTRAP16_BUFFER_SIZE = PAGE_SIZE * 3;

INIT_CODE
auto set_bootstrap16_buffer(PhysAddr base) -> void;

INIT_CODE [[nodiscard]]
auto make_bootstrap_area(BootstrapData **data) -> PhysAddr;

} // namespace ours

#endif // !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)
#endif // #ifndef OURS_ARCH_X86_BOOTSTRAP_HPP
