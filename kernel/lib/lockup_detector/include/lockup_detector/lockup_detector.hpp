/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKUP_DETECTOR_LOCKUP_DETECTOR_HPP
#define LOCKUP_DETECTOR_LOCKUP_DETECTOR_HPP 1

#include <arch/interrupt.hpp>

/// The lockup detector: when a CPU has stopped taking timer interrupts, or
/// the machine has stopped, every CPU's registers and backtrace, printed once
/// by one CPU, and then a panic.
///
/// Every CPU has a heartbeat timer of its own, once a second, and on each
/// beat it looks at every other CPU's heartbeat. One that has not moved for
/// `kernel.lockup.threshold` checks in a row is stuck with interrupts off,
/// and the CPU that finds it reports.
///
/// The dump reaches the other CPUs through an NMI, because the CPU that
/// matters is the one with interrupts off, and that is the one interrupt it
/// still takes. An NMI nothing in the kernel sent -- QEMU's monitor `nmi`,
/// a front-panel button, `lockup nmi` -- also asks for a dump: it is how a
/// machine whose every CPU has interrupts off, and so has no heartbeat to
/// check with, is reached from outside.
///
/// The architecture supplies three things: an NMI sent to a set of CPUs
/// (`ours::arch_mp_send_nmi()`), every NMI handed to on_nmi() below, and
/// the register lines of a CPU's block (arch_dump_registers()).
namespace lockup_detector {
/// This CPU is going offline: its heartbeat timer is cancelled, so that it
/// is not among the timers another CPU takes over. On the CPU, interrupts
/// off, before its tick stops (task::take_cpu_offline()).
auto on_cpu_offline() -> void;

/// This CPU is back: its heartbeat timer is armed again, if the checker is
/// on. On the CPU, interrupts off, once its timers run again.
auto on_cpu_online() -> void;

/// Logs the checker's state, for `lockup status`: the threshold; each online
/// CPU's heartbeat and its notes on every other online CPU -- the checks in a
/// row that CPU has gone without a beat, now and at most, and the fewest
/// beats it has had between two checks; and, last, the closest any CPU has
/// come to the threshold.
///
/// A CPU's notes are its own, written only in its heartbeat timer,
/// so each CPU copies its own, in an IPI (`ours::IpiService::sync_exec()`),
/// rather than having them read from here while it writes them. Call it
/// from a thread with interrupts on; a CPU with interrupts off holds it up
/// until it takes the IPI. While the checker is off there are no notes, and
/// it says so and asks no CPU.
auto log_status() -> void;

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
