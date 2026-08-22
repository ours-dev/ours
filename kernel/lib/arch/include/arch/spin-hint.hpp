/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_SPIN_HINT_HPP
#define ARCH_SPIN_HINT_HPP 1

namespace arch {

/// Tells the CPU that the current thread is busy-waiting on a lock.
///
/// This is a scheduling hint, not a stop: it relaxes the pipeline, backs off
/// the memory subsystem and lets an SMT sibling make progress, then returns.
/// Callers must keep re-checking their own condition around it.
///
/// Do not reach for halt() here.  halt() parks the CPU until the next
/// interrupt, so using it to pace a spin loop stalls the waiter for an
/// unbounded time and deadlocks outright once interrupts are masked.
auto spin_hint() noexcept -> void;

} // namespace arch

// See arch/tick.hpp for why the architecture completion is pulled in here
// rather than left for each caller to include separately.
#if defined(ARCH_X86)
#include <arch/x86/spin-hint.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/spin-hint.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/spin-hint.hpp>
#endif

#endif // ARCH_SPIN_HINT_HPP
