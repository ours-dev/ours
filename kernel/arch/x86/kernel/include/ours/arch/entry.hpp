/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ENTRY_HPP
#define OURS_ARCH_ENTRY_HPP 1

#include <arch/x86/macro/system.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/entry.hpp>

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
        frame->flags
            .set<ArchCpuState::IOPL>(1) // Enable pesmission level
            .set<ArchCpuState::IF>(1);  // Enable interrupt

        // Don't modify, it is required.
        frame->di = arg1;
        frame->si = arg2;
    }

} // namespace
} // namespace ours

#endif // #ifndef OURS_ARCH_ENTRY_HPP