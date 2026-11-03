/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_ENTRY_HPP
#define OURS_IRQ_ENTRY_HPP 1

namespace arch {
struct IrqFrame;
}

namespace ours::irq {
/// Enter a regular trap with interrupts off. Return the previous frame as a
/// stack-local token and raise scheduler disable counts. NMI and other
/// architecture paranoid entries do not use this protocol.
[[nodiscard]]
auto enter_trap(::arch::IrqFrame *frame) -> ::arch::IrqFrame *;

/// Leave in stack order, still with interrupts off. Restore the previous
/// frame and disable counts before returning whether the architecture caller
/// should preempt. Only outermost exit may consume a local request.
[[nodiscard]]
auto leave_trap(::arch::IrqFrame *previous) -> bool;

/// Current regular trap frame, or nullptr outside this CPU's trap stack.
/// Retaining or dereferencing a non-null answer requires a non-preemptible
/// caller; readers that only test whether a frame exists may run normally.
[[nodiscard]]
auto current_frame() -> ::arch::IrqFrame *;

/// The root adapter identifies a source and dispatches its endpoint. If no
/// source exists, the adapter completes the unmatched interrupt itself.
using RootHandler = auto (*)(::arch::IrqFrame *frame) -> void;

/// Publish once during controller startup, before any CPU can receive one of
/// its interrupts. Publication also makes prior controller setup visible.
auto set_root_handler(RootHandler handler) -> void;

/// Invoke the installed root handler inside the architecture trap scope.
auto handle_root_irq(::arch::IrqFrame *frame) -> void;
} // namespace ours::irq

#endif // OURS_IRQ_ENTRY_HPP
