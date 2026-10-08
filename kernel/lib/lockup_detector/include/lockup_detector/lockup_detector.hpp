/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKUP_DETECTOR_LOCKUP_DETECTOR_HPP
#define LOCKUP_DETECTOR_LOCKUP_DETECTOR_HPP 1

#include <arch/interrupt.hpp>

/// The lockup detector: when the machine has stopped, every CPU's registers
/// and backtrace, printed once by one CPU, and then a panic.
///
/// The dump reaches the other CPUs through an NMI, because the CPU that
/// matters is the one with interrupts off, and that is the one interrupt it
/// still takes. An NMI nothing in the kernel sent -- QEMU's monitor `nmi`,
/// a front-panel button, `lockup nmi` -- asks for a dump.
///
/// The architecture supplies three things: an NMI sent to a set of CPUs
/// (`ours::arch_mp_send_nmi()`), every NMI handed to on_nmi() below, and
/// the register lines of a CPU's block (arch_dump_registers()).
namespace lockup_detector {

/// Takes one NMI, with the frame it interrupted. Called by the
/// architecture's NMI vector, with %gs and the CPU's per-CPU data in place,
/// for every NMI the CPU takes.
///
/// It returns for an NMI it leaves alone -- one that arrives while a panic
/// is already stopping the machine, or one taken by the reporting CPU itself
/// -- and for a request that reaches a CPU in a panic of its own, which
/// leaves its state for the report and goes back to that panic. Otherwise it
/// never returns: the CPU either prints the report and panics, or leaves its
/// state for the report and halts with NMIs still blocked, since only an
/// iretq would unblock them.
auto on_nmi(arch::IrqFrame const &frame) -> void;

/// Logs |frame|'s registers at error level, in the layout a kernel fault's
/// report uses. Implemented by the architecture (x86: faults.cpp), whose
/// register names they are.
auto arch_dump_registers(arch::IrqFrame const &frame) -> void;

} // namespace lockup_detector

#endif // #ifndef LOCKUP_DETECTOR_LOCKUP_DETECTOR_HPP
