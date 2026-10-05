/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `crash <how>`: take the machine down on purpose, so that the panic path can
// be tested the way it is used.
//
//   crash panic          call ours::panic() with a fixed message
//   crash panic-locked   the same, holding the serial logger's lock, as a
//                        CPU that panics inside a log write does
//   crash panic-smp      every online CPU panics at the same moment
//
// The panic path cannot be exercised any other way: nothing panics in a
// healthy boot, and what it prints, and in what order, is the whole of what
// there is to check. scripts/kshell_run.py knows that a `crash` command ends
// the machine, and checks the report that follows it.
#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/panic.hpp>
#include <ours/start.hpp>

#include <atomic>
#include <string_view>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace {
/// What `crash panic` panics with. Fixed, so that a check reading the serial
/// log knows which line to look for.
constexpr auto PANIC_MESSAGE = std::string_view("crash: deliberate panic from the kernel shell");

/// Where the CPUs of `crash panic-smp` meet before they panic.
struct Rendezvous {
    std::atomic<usize> arrived;
    usize expected;
};

/// One CPU's share of `crash panic-smp`: count in, wait for the others, panic.
///
/// The wait is what makes the panics simultaneous. IpiService::sync_exec()
/// runs this on the other CPUs from their interrupt handlers and on this CPU
/// after the IPIs are out, so without it the panics would start an IPI's
/// latency apart, and the first CPU could have stopped the rest before they
/// got here. With it they start within a few instructions of each other,
/// which is the window the one-report guard in vpanic() is for.
///
/// Unbounded: a CPU that never arrives hangs the test, and the hang is the
/// report. Panicking anyway after a bound would pass without the panics
/// having overlapped.
auto panic_together(void *context) -> void {
    auto &rendezvous = *static_cast<Rendezvous *>(context);
    rendezvous.arrived.fetch_add(1, std::memory_order_relaxed);
    while (rendezvous.arrived.load(std::memory_order_relaxed) < rendezvous.expected) {
        arch::spin_hint();
    }
    ours::panic(PANIC_MESSAGE);
}

auto crash_command(int argc, char **argv) -> int {
    if (argc != 2) {
        log::warn("usage: crash panic|panic-locked|panic-smp");
        return 1;
    }
    auto const how = std::string_view(argv[1]);
    if (how == "panic") {
        ours::panic(PANIC_MESSAGE);
    }
    if (how == "panic-locked") {
        // Nothing can print from here on until the panic path stops waiting
        // for the lock: without panic mode, nothing ever does.
        ours::hold_console_lock_forever();
        ours::panic(PANIC_MESSAGE);
    }
    if (how == "panic-smp") {
        auto const cpus = ours::Cpu::num_online_cpus();
        if (cpus < 2) {
            log::warn("crash: panic-smp needs at least two online CPUs, have {}", cpus);
            return 1;
        }
        // On this thread's stack, which is safe only because this CPU's own
        // share panics too: sync_exec() never returns, so the frame outlives
        // every CPU that reads it.
        auto rendezvous = Rendezvous{.arrived = 0, .expected = cpus};
        ours::IpiService::sync_exec(ours::Cpu::online_mask(), panic_together, &rendezvous);
        ours::unreachable();
    }
    log::warn("usage: crash panic|panic-locked|panic-smp");
    return 1;
}
} // namespace

GKTL_COMMAND(Crash, "crash", crash_command,
             "crash panic|panic-locked|panic-smp: panic on purpose, to test the panic path")
