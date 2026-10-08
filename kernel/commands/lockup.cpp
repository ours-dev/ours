/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `lockup <what>`: drive the lockup detector (//kernel/lib/lockup_detector)
// on purpose, so that its report can be tested the way it is used.
//
//   lockup nmi   send this CPU an NMI that nothing in the kernel asked for,
//                as QEMU's monitor `nmi` or a front-panel button does: every
//                CPU's state is printed once, and the machine panics
//
// The report never comes in a healthy boot, and what it prints, and in what
// order, is the whole of what there is to check.
#include <ours/arch/mp.hpp>
#include <ours/cpu.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>

#include <string_view>

#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace {
using namespace ours;

/// How long `lockup nmi` waits for its own NMI before it says it never came.
/// An NMI a CPU sends itself arrives before the next instruction; a second
/// is a bound for the case where it never does, nothing more.
constexpr auto NMI_WAIT = Seconds(1);

/// `lockup nmi`. Returns only if the NMI was not sent or never arrived.
///
/// The NMI goes to this CPU through the same path the report itself uses
/// for the others, arch_mp_send_nmi(). Preemption stays off from asking
/// which CPU this is until the NMI is sent, so that the thread cannot move
/// and send it to a CPU it is no longer on. The NMI lands while the send
/// still has interrupts off around the ICR writes, so this thread's block
/// says "interrupts off" and points into the send.
auto dump_by_nmi() -> int {
    task::AutoPreemptDisabler same_cpu;
    auto const self = Cpu::id();
    auto const unsent = arch_mp_send_nmi(CpuMask::of(self));
    same_cpu.enable();
    if (unsent.any()) {
        log::error("lockup: could not send cpu{} an NMI: its ICR never went idle", self);
        return 1;
    }
    // Woken on time or early, the answer is the same: the NMI did not come.
    auto _ = task::Thread::Current::sleep_for(NMI_WAIT, task::Interruptible::No);
    log::error("lockup: cpu{} sent itself an NMI, and it never arrived", self);
    return 1;
}

auto lockup_command(int argc, char **argv) -> int {
    if (argc == 2 && std::string_view(argv[1]) == "nmi") {
        return dump_by_nmi();
    }
    log::warn("usage: lockup nmi");
    return 1;
}
} // namespace

GKTL_COMMAND(Lockup, "lockup", lockup_command,
             "lockup nmi: dump every CPU's state through an NMI, then panic")
