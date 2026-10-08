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
///    heartbeat it saw last, how many checks in a row it has not moved, and
///    the closest it has come to THRESHOLD, for `lockup status`.
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
///    with interrupts off (and, for `lockup status`, in an IPI on that same
///    CPU: the last point below). Every CPU checks every other on notes of
///    its own, rather than sharing one note per target, so two checkers
///    never meet and there is no claim to take per target, nor a ring of who
///    checks whom to keep in step as CPUs come online.
///  - THRESHOLD is the checker's one state and its one setting at once,
///    written once at boot and read on its own; a relaxed load sees either
///    0, and checks nothing, or the threshold.
///  - `lockup status` (log_status()) reads every CPU's NOTES, and so does
///    not read them from its own CPU: it asks each CPU for a copy of its
///    own, in an IPI. The IPI runs with interrupts off, as the tick does, so
///    on the CPU that owns the notes the two never overlap, and the notes
///    keep one reader and one writer at a time, both on that CPU.
#include "checker.hpp"

#include <lockup_detector/heartbeat_note.hpp>
#include <lockup_detector/lockup_detector.hpp>

#include <array>
#include <atomic>
#include <format>
#include <string_view>

#include "report.hpp"
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/boot/options.hpp>
#include <ours/cpu.hpp>
#include <ours/init.hpp>
#include <ours/ipi_service.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/panic.hpp>
#include <ours/time/cfg.hpp>
#include <ustl/option.hpp>

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

    /// The longest line of one CPU's notes in `lockup status`:
    /// " cpu<N> <now>/<most>/<fewest>" for every CPU, at most 50 characters
    /// each.
    constexpr auto STATUS_BYTES = usize{50} * ours::MAX_CPU;

    /// `lockup status`'s task on every CPU: a copy of that CPU's notes into
    /// |*context|. It runs on the CPU that owns them, with interrupts off,
    /// in its IPI or, on the CPU that asks, in IpiService::sync_exec()
    /// itself, so that it never meets the tick that writes them.
    auto copy_notes(void *const context) -> void {
        *static_cast<priv::Notes *>(context) = ours::mm::CpuLocal::access(NOTES);
    }

    /// The closest any CPU has come to the threshold, over every note of
    /// every checking CPU: the most checks in a row without a tick, and the
    /// fewest ticks between two checks, each with the CPU it is about and
    /// the CPU that saw it.
    class Closest final {
      public:
        /// Takes in |checker|'s note on |target|.
        auto add(CpuNum const checker, CpuNum const target, HeartbeatNote const &note) -> void {
            if (m_unchanged_target == ours::INVALID_CPU_NUM ||
                note.most_unchanged() > m_most_unchanged) {
                m_most_unchanged = note.most_unchanged();
                m_unchanged_target = target;
                m_unchanged_checker = checker;
            }
            auto const fewest = note.fewest_ticks();
            if (fewest.is_some() &&
                (m_fewest_ticks.is_none() || fewest.value() < m_fewest_ticks.value())) {
                m_fewest_ticks = fewest;
                m_fewest_target = target;
                m_fewest_checker = checker;
            }
        }

        /// The last line of `lockup status`.
        auto print(u32 const threshold) const -> void {
            if (m_unchanged_target == ours::INVALID_CPU_NUM) {
                log::info("lockup status: closest to the threshold: no notes yet");
                return;
            }
            if (m_fewest_ticks.is_none()) {
                log::info("lockup status: closest to the threshold: at most {} checks in a row "
                          "without a tick (cpu{} as cpu{} saw it), threshold {}; no two checks "
                          "of a ticking CPU yet",
                          m_most_unchanged, m_unchanged_target, m_unchanged_checker, threshold);
                return;
            }
            log::info("lockup status: closest to the threshold: at most {} checks in a row "
                      "without a tick (cpu{} as cpu{} saw it), threshold {}; at fewest {} ticks "
                      "between two checks (cpu{} as cpu{} saw it)",
                      m_most_unchanged, m_unchanged_target, m_unchanged_checker, threshold,
                      m_fewest_ticks.value(), m_fewest_target, m_fewest_checker);
        }

      private:
        u32 m_most_unchanged{0};
        CpuNum m_unchanged_target{ours::INVALID_CPU_NUM};
        CpuNum m_unchanged_checker{ours::INVALID_CPU_NUM};
        ustl::Option<u64> m_fewest_ticks;
        CpuNum m_fewest_target{ours::INVALID_CPU_NUM};
        CpuNum m_fewest_checker{ours::INVALID_CPU_NUM};
    };

    /// One line of `lockup status`: |checker|'s heartbeat, then its notes
    /// on every other online CPU, each as "<now>/<most>/<fewest>", "-" for a
    /// fewest not counted yet. Each note also goes into |closest|.
    auto log_notes(CpuNum const checker, priv::Notes const &notes, Closest &closest) -> void {
        auto text = std::array<char, STATUS_BYTES>{};
        auto *out = text.data();
        auto *const end = text.data() + text.size();
        ours::Cpu::online_mask().for_each([checker, &notes, &closest, &out, end](CpuNum const cpu) {
            if (cpu == checker) {
                return;
            }
            auto const &note = notes[cpu];
            closest.add(checker, cpu, note);
            auto const room = static_cast<std::ptrdiff_t>(end - out);
            auto const fewest = note.fewest_ticks();
            if (fewest.is_none()) {
                out = std::format_to_n(out, room, " cpu{} {}/{}/-", cpu, note.unchanged(),
                                       note.most_unchanged())
                          .out;
                return;
            }
            out = std::format_to_n(out, room, " cpu{} {}/{}/{}", cpu, note.unchanged(),
                                   note.most_unchanged(), fewest.value())
                      .out;
        });
        auto const heartbeat =
            ours::mm::CpuLocal::access(HEARTBEAT, checker).load(std::memory_order_relaxed);
        log::info("lockup status: cpu{} has taken {} ticks; its notes:{}", checker, heartbeat,
                  std::string_view(text.data(), static_cast<usize>(out - text.data())));
    }
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

auto log_status() -> void {
    auto closest = Closest();
    auto const threshold = THRESHOLD.load(std::memory_order_relaxed);
    if (threshold == 0) {
        // No check has run, so no note has been written and there is
        // nothing to copy. Nor is any CPU asked for its notes: one with
        // interrupts off would hold the status up, and with the checker
        // off nothing would report it and end the wait.
        log::info("lockup status: the checker is off ({}=0, or InitLevel::Platform not reached)",
                  ours::boot::BootOptions::LOCKUP_THRESHOLD_OPTION);
        closest.print(threshold);
        return;
    }
    log::info("lockup status: a CPU is reported at {} checks in a row without a tick; a check "
              "comes every {} ticks of the checking CPU's own",
              threshold, TICKS_PER_CHECK);
    log::info("lockup status: per CPU, the ticks it has taken, then its notes on each other "
              "CPU: checks in a row without a tick now/at most/fewest ticks between two checks");

    ours::Cpu::online_mask().for_each([&closest](CpuNum const checker) {
        auto notes = priv::Notes{};
        ours::IpiService::sync_exec(ours::CpuMask::of(checker), &copy_notes, &notes);
        log_notes(checker, notes, closest);
    });
    closest.print(threshold);
}
} // namespace lockup_detector
