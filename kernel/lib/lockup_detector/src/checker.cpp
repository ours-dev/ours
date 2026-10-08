/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The checker: a heartbeat on every CPU, and every CPU's once-a-second look
/// at every other CPU's.
///
/// A CPU's tick stops in only two ways: interrupts are off on it, or its
/// tick device has stopped. Every spin lock is held and waited for with
/// interrupts off, so a CPU stuck on one takes no tick, and its heartbeat
/// stands still while the other CPUs' move.
///
/// Objects:
///  - HEARTBEAT: per CPU, the ticks it has taken. Written by its own CPU in
///    its tick, read by every other CPU's check.
///  - NOTES: per checking CPU, a HeartbeatNote for each other CPU: the
///    heartbeat it saw last, and how many checks in a row it has not moved.
///  - THRESHOLD: the checks in a row without a tick that make a CPU stuck;
///    0 while the checker is off.
///
/// Events:
///  1. Every tick on every CPU moves its HEARTBEAT.
///  2. The init hook at InitLevel::Platform, once every CPU is up, stores
///     THRESHOLD from `kernel.lockup.threshold`. Until then, checks do
///     nothing.
///  3. On every second's worth of its own ticks, a CPU checks every other
///     online CPU: it reads that CPU's HEARTBEAT and updates its note.
///  4. A note that reaches THRESHOLD makes the checking CPU try for the
///     report (report.cpp). It reports and panics, or finds that another CPU
///     already holds the claim, and from then on checks nothing.
///
/// Why no lock anywhere:
///  - HEARTBEAT has one writer, its own CPU, and the readers only ask
///    whether it moved, so a relaxed increment and relaxed loads do: nothing
///    else is published through it, and a load that returns a count a few
///    ticks old is still "it moved" or "it did not" one check later.
///  - NOTES has one writer and one reader, its own CPU, in its own tick
///    with interrupts off. Every CPU checks every other on notes of its own,
///    rather than sharing one note per target, so two checkers never meet
///    and there is no claim to take per target, nor a ring of who checks whom
///    to keep in step as CPUs come online.
///  - THRESHOLD is the checker's one state and its one setting at once,
///    written once at boot and read on its own; a relaxed load sees either
///    0, and checks nothing, or the threshold.
#include "checker.hpp"

#include <lockup_detector/heartbeat_note.hpp>
#include <lockup_detector/lockup_detector.hpp>

#include <atomic>

#include "report.hpp"
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/boot/options.hpp>
#include <ours/cpu.hpp>
#include <ours/init.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/panic.hpp>
#include <ours/time/cfg.hpp>

namespace lockup_detector {
using ours::CpuNum;

namespace {
    /// A check on every this many of the checking CPU's own ticks: one a
    /// second. Counted in its own ticks, so that a pause of the whole
    /// machine, which stops its ticks too, counts no checks.
    constexpr auto TICKS_PER_CHECK = static_cast<u64>(ours::time::HZ);

    /// The ticks this CPU has taken.
    CPU_LOCAL
    std::atomic<u64> HEARTBEAT;

    /// This CPU's notes on the other CPUs' heartbeats, by CPU number.
    CPU_LOCAL
    priv::Notes NOTES;

    /// The checks in a row without a tick that make a CPU stuck, in
    /// seconds; 0 until arm() has run, and for good under
    /// `kernel.lockup.threshold=0`.
    constinit std::atomic<u32> THRESHOLD{0};

    /// Event 3 of the file comment, on the CPU whose tick this is.
    auto check_others() -> void {
        auto const threshold = THRESHOLD.load(std::memory_order_relaxed);
        if (threshold == 0) {
            return;
        }
        // A panic stops the other CPUs, and a stopped CPU takes no tick: the
        // checks would find the panic's own work. Once the report is
        // claimed, its reporter is already gathering every CPU's state.
        if (ours::panic_in_progress() || priv::report_claimed()) {
            return;
        }

        auto const self = ours::Cpu::id();
        auto &notes = ours::mm::CpuLocal::access(NOTES);
        auto stuck = ours::INVALID_CPU_NUM;
        ours::Cpu::online_mask().for_each([self, threshold, &notes, &stuck](CpuNum const cpu) {
            if (cpu == self) {
                return;
            }
            auto const heartbeat =
                ours::mm::CpuLocal::access(HEARTBEAT, cpu).load(std::memory_order_relaxed);
            // Every note is brought up to date before any report, so that
            // the report's counts are all from this one check.
            if (notes[cpu].check(heartbeat, threshold) && stuck == ours::INVALID_CPU_NUM) {
                stuck = cpu;
            }
        });
        if (stuck == ours::INVALID_CPU_NUM) {
            return;
        }

        // The tick is an interrupt, and the frame it interrupted is the
        // reporter's own block.
        auto const *const frame = ours::irq::current_frame();
        ASSERT(frame != nullptr, "lockup: the check ran outside the tick's interrupt");
        priv::report_stuck(*frame, self, stuck, notes);
    }

    /// Event 2: the checker goes live. At InitLevel::Platform, after
    /// init_platform() has brought up every CPU, so that no CPU is checked
    /// before its tick has started; the notes leave a CPU whose heartbeat
    /// has never moved alone in any case.
    INIT_CODE
    auto arm() -> void {
        auto const threshold = ours::boot::options().lockup_threshold();
        if (threshold == 0) {
            log::info("lockup detector: off ({}=0)",
                      ours::boot::BootOptions::LOCKUP_THRESHOLD_OPTION);
            return;
        }
        THRESHOLD.store(threshold, std::memory_order_relaxed);
        log::info("lockup detector: a CPU that takes no tick for {} s is reported", threshold);
    }
    GKTL_INIT_HOOK(LockupDetector, arm, gktl::InitLevel::Platform);
} // namespace

namespace priv {
    /// Relaxed, as every other read of a heartbeat: the count is all that is
    /// wanted, and nothing is published through it. Its one caller is a
    /// CPU filling its own slot, in its NMI or in its tick; an NMI may land
    /// in the middle of this CPU's own increment, and reads the count either
    /// side of it.
    auto this_cpu_heartbeat() -> u64 {
        return ours::mm::CpuLocal::access(HEARTBEAT).load(std::memory_order_relaxed);
    }
} // namespace priv

auto on_tick() -> void {
    auto const ticks =
        ours::mm::CpuLocal::access(HEARTBEAT).fetch_add(1, std::memory_order_relaxed) + 1;
    if (ticks % TICKS_PER_CHECK == 0) {
        check_others();
    }
}
} // namespace lockup_detector
