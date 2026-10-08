/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKUP_DETECTOR_LOCKUP_DETECTOR_HPP
#define LOCKUP_DETECTOR_LOCKUP_DETECTOR_HPP 1

#include <arch/interrupt.hpp>

/// The lockup detector: when a CPU has stopped taking its tick, or the
/// machine has stopped, every CPU's registers and backtrace, printed once by
/// one CPU, and then a panic.
///
/// Every CPU's tick counts a heartbeat, and once a second every CPU looks at
/// every other CPU's (on_tick()). One that has not moved for
/// `kernel.lockup.threshold` checks in a row is stuck with interrupts off,
/// and the CPU that finds it reports.
///
/// The dump reaches the other CPUs through an NMI, because the CPU that
/// matters is the one with interrupts off, and that is the one interrupt it
/// still takes. An NMI nothing in the kernel sent -- QEMU's monitor `nmi`,
/// a front-panel button, `lockup nmi` -- also asks for a dump: it is how a
/// machine whose every CPU has interrupts off, and so takes no tick to check
/// with, is reached from outside.
///
/// The architecture supplies three things: an NMI sent to a set of CPUs
/// (`ours::arch_mp_send_nmi()`), every NMI handed to on_nmi() below, and
/// the register lines of a CPU's block (arch_dump_registers()).
namespace lockup_detector {
/// One tick on this CPU: its heartbeat moves, and on every second's worth of
/// its own ticks it checks every other online CPU's. Called by the tick
/// (`ours::time::TickSched::on_tick()`) on every tick, first, in interrupt
/// context with interrupts off.
///
/// The check does nothing until the init hook at InitLevel::Platform has
/// armed it, by which time every CPU is up, nor ever with
/// `kernel.lockup.threshold=0`; nor while a panic is under way, nor once a
/// report has been claimed. It returns unless it finds a stuck CPU and wins
/// the report, which then ends in a panic.
auto on_tick() -> void;

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
