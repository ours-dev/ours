/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_IDT_HPP
#define OURS_ARCH_X86_IDT_HPP 1

#include <arch/x86/interrupt.hpp>

namespace ours {
    auto x86_init_idt_early() -> void;

    auto x86_setup_idt() -> void;

    auto x86_load_idt() -> void;

    auto x86_dump_idt() -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_IDT_HPP