/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_ARCH_X86_FAULTS_HPP
#define OURS_ARCH_X86_FAULTS_HPP 1

#include <ours/status.hpp>

#include <arch/x86/interrupt.hpp>

namespace ours {
/// Resolves the page fault |frame| describes at |addr|, the CR2 its caller
/// read before anything else could fault and replace it.
auto x86_handle_page_fault(arch::IrqFrame *frame, VirtAddr addr) -> Status;

auto x86_handle_ipi_generic() -> void;

auto x86_handle_ipi_interrupt() -> void;

auto x86_handle_ipi_resched() -> void;

auto x86_handle_ipi_suspend() -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_FAULTS_HPP