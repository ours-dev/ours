/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_ENTRY_HPP
#define OURS_ARCH_X86_ENTRY_HPP 1

#include <ours/compiler.hpp>

#include <arch/interrupt.hpp>

namespace ours {
    NO_MANGLE 
    auto x86_syscall(arch::IrqFrame *) -> void;

    NO_MANGLE [[noreturn]] 
    auto x86_enter_uspace(arch::IrqFrame *) -> void;

    NO_MANGLE
    auto x86_switch_context(VirtAddr *prev, VirtAddr next) -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_ENTRY_HPP