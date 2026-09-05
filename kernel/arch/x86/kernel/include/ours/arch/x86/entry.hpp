/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_ENTRY_HPP
#define OURS_ARCH_X86_ENTRY_HPP 1

#include <arch/interrupt.hpp>
#include <ours/compiler.hpp>

namespace ours {
extern "C" auto x86_syscall(arch::IrqFrame *) -> void;

extern "C" [[noreturn]]
auto x86_enter_uspace(arch::IrqFrame *) -> void;

extern "C" auto x86_switch_context(VirtAddr *prev, VirtAddr next) -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_ENTRY_HPP