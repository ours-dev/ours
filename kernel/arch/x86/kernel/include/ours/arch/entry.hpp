/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ENTRY_HPP
#define OURS_ARCH_ENTRY_HPP 1

#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/entry.hpp>
#include <ours/assert.hpp>

#include <arch/interrupt.hpp>
#include <arch/x86/macro/system.hpp>

namespace ours {
namespace {
    /// Whether `pc` may be where a user thread starts or resumes: an address
    /// inside the user address space.
    ///
    /// This is what keeps IRETQ and SYSRET from faulting in ring 0. On Intel
    /// both raise #GP on a non-canonical return address *before* the
    /// privilege change -- in ring 0, after the swapgs that put the user's
    /// %gs back, and for SYSRET on the user's stack too. interrupt_common sees
    /// a kernel CS in that #GP's frame, skips its swapgs, and runs the handler
    /// on per-CPU data the user placed. AMD raises the fault in ring 3
    /// instead, and so does QEMU's TCG, which is why booting under QEMU shows
    /// nothing worse than a killed process.
    ///
    /// The user range lies wholly below the 2^47 split, so anything inside it
    /// is canonical. Every path that sets a user pc has to ask this:
    /// ThreadDispatcher::start() does, and so must the future thread-state
    /// writes, signal returns and exception resumes. SYSRET's own return
    /// address needs no check -- see USER_ASPACE_SIZE for why.
    ///
    /// Zircon also accepts 0, for tests that want a thread to fault at once.
    /// This does not: TaskState's entry defaults to 0, so accepting it would
    /// only let a caller that forgot to set one start a thread nowhere.
    ///
    /// Zircon counterpart: arch_is_valid_user_pc() (kernel/arch/x86/include/
    /// arch/vm.h); the reasoning is docs/concepts/kernel/sysret_problem.md.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto arch_is_valid_user_pc(VirtAddr pc) -> bool {
        return pc >= USER_ASPACE_BASE && pc < USER_ASPACE_BASE + USER_ASPACE_SIZE;
    }

    /// The caller must have interrupts off, as Zircon's arch_enter_uspace()
    /// asserts too. x86_enter_uspace swaps GS to the user's base before it
    /// reaches iretq, and an interrupt taken in between sees a kernel CS in
    /// its frame, so the entry stub skips its own swapgs and runs the handler
    /// with the user's GS base under every per-CPU access. IF=1 in the frame
    /// being built is what turns interrupts back on, atomically with the
    /// privilege change.
    ///
    /// The entry point is checked once more here, in every build: this is the
    /// last moment a bad one is a kernel panic with the kernel's own %gs
    /// rather than a ring-0 #GP on the user's. Callers are expected to have
    /// refused it already (ThreadDispatcher::start()). Zircon's
    /// arch_enter_uspace() ASSERTs the same, also in every build.
    [[noreturn]] FORCE_INLINE
    auto arch_enter_uspace(arch::IrqFrame *frame) -> void {
        DEBUG_ASSERT(arch::irqs_disabled());
        ASSERT(arch_is_valid_user_pc(frame->ip), "entering user mode at {:#x}", frame->ip);
        x86_enter_uspace(frame);
    }

    FORCE_INLINE
    auto arch_build_uspace_irqframe(arch::IrqFrame *frame, usize ip, usize sp, usize arg1,
                                    usize arg2) -> void {
        using arch::ArchCpuState;

        frame->ip = ip;
        frame->usp = sp;

        frame->cs = X86_GDT_USER_CODE64;
        frame->uss = X86_GDT_USER_DATA;
        // IOPL 0, stated rather than left to the zeroed frame, because it used
        // to say 1 under a comment calling it "enable permission level". IOPL
        // is the highest CPL allowed to run in/out/cli/sti without faulting;
        // ring 3 is CPL 3, so 1 granted it nothing and only made the number
        // look like a privilege. Port access for a user driver is the TSS I/O
        // bitmap's job, per task, not a flag every process inherits. Linux
        // and Zircon both start user threads at IOPL 0.
        frame->flags.set<ArchCpuState::IOPL>(0).set<ArchCpuState::IF>(1);

        // Don't modify, it is required.
        frame->di = arg1;
        frame->si = arg2;
    }

} // namespace
} // namespace ours

#endif // #ifndef OURS_ARCH_ENTRY_HPP