/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `tickfallback.test`: a CPU offered a device that claims one-shot and will
// not enter it keeps a periodic tick (TickDevice::rebind_device()).
//
// The device is a fake that can run Periodic and Oneshot on paper and refuses
// the second when asked. The tick device it is offered to is a TickDevice of
// the test's own, on its stack, and not the CPU's: a fresh TickDevice points at
// the placeholder device and is in Periodic mode, so the first real device is
// always taken, and the refusal returns from setup_oneshot_tick() before it
// touches the timer queue or the tick, so nothing the running CPU depends on is
// involved. The fake is not enrolled (ClockEventDevice::enroll()): enrolling
// offers it to the CPU's own TickDevice, which would take it, rated as it is
// above the local APIC timer.
//
// What it checks is what must hold after a refusal: the mode is still
// Periodic, because setup_oneshot_tick() commits it only after the device
// accepted Oneshot; the fake is in Periodic mode with a handler, which is
// what a periodic tick in service is; and it was asked for Oneshot once.
//
// Needs kernel.time.oneshot, as the fallback is only entered when the CPU
// wants a one-shot tick; without it the test says so and passes.
//
// A console command rather than a unit test for the reason mutex.stress
// gives. It is named *.test so that scripts/kshell_run.py waits for its
// verdict line, as for timershare.test.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/time/clock_event_device.hpp>
#include <ours/time/tick_device.hpp>

#include <arch/auto_interrupt_disabler.hpp>
#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using time::ClockEventDevice;
    using time::TickDevice;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("tickfallback.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("tickfallback.test: FAIL  {}", name);
    }

    /// A device that is rated above the local APIC timer, can run Periodic,
    /// and refuses Oneshot every time it is asked.
    class OneshotRefusingDevice {
        using Self = OneshotRefusingDevice;

      public:
        explicit OneshotRefusingDevice(CpuNum const cpu)
            : m_cpumask(CpuMask::of(cpu))
            , m_oneshot_asked(0)
            , m_clkevt("oneshot-refusing", &time::CLKEVT_VTABLE_OF<Self, &Self::m_clkevt>,
                       ClockEventDevice::Feature::Periodic | ClockEventDevice::Feature::Oneshot,
                       ClockEventDevice::Rating(700), &m_cpumask) {}

        auto set_mode_oneshot() -> Status {
            m_oneshot_asked += 1;
            return Status::Unsupported;
        }

        auto set_mode_periodic() -> Status {
            return Status::Ok;
        }

        auto set_next_event_tick(Ticks) -> Status {
            return Status::Ok;
        }

        auto shutdown() -> void {}

        [[nodiscard]]
        auto oneshot_asked() const -> usize {
            return m_oneshot_asked;
        }

        auto device() -> ClockEventDevice & {
            return m_clkevt;
        }

      private:
        CpuMask m_cpumask;
        usize m_oneshot_asked;
        ClockEventDevice m_clkevt;
    };

    auto tick_fallback_command(int, char **) -> int {
        Tally tally;

        if (!TickDevice::Current::is_mode_oneshot()) {
            log::info("tickfallback.test: ok -- skipped, kernel.time.oneshot is off");
            return 0;
        }

        // What happened, noted with interrupts off and judged after them.
        Status offered = Status::BadState;
        bool holds_fake = false;
        bool still_periodic = false;
        bool fake_periodic = false;
        bool fake_has_handler = false;
        usize oneshot_asked = 0;
        {
            // check_new_device() swaps a device and its handler, and requires
            // that no interrupt can find it half way.
            arch::AutoInterruptDisabler const no_interrupts;

            OneshotRefusingDevice fake(Cpu::id());
            fake.device().configure(1'000'000'000, 0xF, 0x7FFFFFFF);

            TickDevice own;
            offered = own.check_new_device(&fake.device());
            holds_fake = own.device() == &fake.device();
            still_periodic = own.mode() == TickDevice::Mode::Periodic;
            fake_periodic = fake.device().is_mode_periodic();
            fake_has_handler = fake.device().has_event_handler();
            oneshot_asked = fake.oneshot_asked();
        }

        verdict(tally, offered == Status::Ok, "a device that will not run one-shot is still taken");
        verdict(tally, holds_fake, "the tick device points at it");
        verdict(tally, still_periodic,
                "the mode is still Periodic: it was not committed before the device accepted");
        verdict(tally, fake_periodic, "and the device runs Periodic");
        verdict(tally, fake_has_handler, "with the periodic tick's handler on it");
        verdict(tally, oneshot_asked == 1, "after being asked for one-shot once");

        if (tally.failed != 0) {
            log::error("tickfallback.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("tickfallback.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(
    TickFallbackTest, "tickfallback.test", ours::tick_fallback_command,
    "offer a tick device a device that will not run one-shot, and check it stays periodic");
