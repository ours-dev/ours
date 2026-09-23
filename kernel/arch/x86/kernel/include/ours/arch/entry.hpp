/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ENTRY_HPP
#define OURS_ARCH_ENTRY_HPP 1

#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/entry.hpp>

#include <arch/x86/macro/system.hpp>

namespace ours {
namespace {
    [[noreturn]] FORCE_INLINE
    auto arch_enter_uspace(arch::IrqFrame *frame) -> void {
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