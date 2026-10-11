/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `tickreplace.test`: a CPU whose tick device refuses a deadline gives it up
// by itself and keeps its timers.
//
// One CPU, the last, is given a device that refuses every deadline and is
// rated above the local APIC timer, so enrolling it makes the CPU take it
// (TickDevice::check_new_device()). The first deadline the timer queue asks
// it for is refused, the queue tells the TickDevice, and a Dpc on that CPU
// chooses the best device that has not failed and moves the tick to it
// (TickDevice::on_device_refused(), replace_failed()). Two cases:
//
//   * The CPU's own device is fine. It must end on it again, and its timers
//     must fire.
//
//   * The CPU's own device is marked failed first (by the test, which clears
//     the mark at the end). Nothing of its own is left, so it must end on the
//     global device through a broadcast group, and its timers must fire there.
//
// The refusing devices are enrolled for good, as no device is ever
// unenrolled: each run leaks a few hundred bytes and two entries on the
// device list, marked failed so that nothing chooses them again.
//
// Needs kernel.time.oneshot and a global device that can be armed for one
// instant; without them it says so and passes.
//
// A console command rather than a unit test for the reason mutex.stress
// gives. It is named *.test so that scripts/kshell_run.py waits for its
// verdict line, as for timershare.test.
#include <ours/arch/time.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_broadcast.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer.hpp>
#include <ours/time/timer_queue.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using time::ClockEventDevice;
    using time::Deadline;
    using time::SteadyClock;
    using time::TickBroadcast;
    using time::TickDevice;
    using time::Timer;
    using time::TimerQueue;

    constexpr usize SAMPLES = 10;
    constexpr auto REQUESTED = Milliseconds(1);
    constexpr auto LOST_AFTER = Milliseconds(200);
    /// How long the replacement may take, from the refusal to the CPU being
    /// on its new device.
    constexpr auto REPLACE_WITHIN = Seconds(5);
    constexpr auto GIVE_UP = Seconds(30);
    constexpr auto POLL_INTERVAL = Milliseconds(5);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("tickreplace.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("tickreplace.test: FAIL  {}", name);
    }

    /// A device that is rated above every real one, takes any mode, and
    /// refuses to be armed for any instant.
    class RefusingDevice {
        using Self = RefusingDevice;

      public:
        explicit RefusingDevice(CpuNum const cpu)
            : m_cpumask(CpuMask::of(cpu))
            , m_clkevt("refusing", &time::CLKEVT_VTABLE_OF<Self, &Self::m_clkevt>,
                       ClockEventDevice::Feature::Oneshot, ClockEventDevice::Rating(1000),
                       &m_cpumask) {}

        auto set_mode_oneshot() -> Status {
            return Status::Ok;
        }

        auto set_next_event_tick(Ticks) -> Status {
            return Status::Unsupported;
        }

        auto shutdown() -> void {}

        auto device() -> ClockEventDevice & {
            return m_clkevt;
        }

      private:
        CpuMask m_cpumask;
        ClockEventDevice m_clkevt;
    };

    struct Result {
        CpuNum cpu = 0;
        TickBroadcast *group = nullptr;

        /// The first case.
        bool own_back = false;
        bool first_failed = false;
        usize first_fired = 0;
        /// The second case.
        bool on_group = false;
        bool second_failed = false;
        usize second_fired = 0;
        bool restored = false;

        usize refusals = 0;
        std::atomic<bool> done{false};
    };

    struct Fired {
        std::atomic<bool> done{false};
    };

    /// Arms one timer for REQUESTED from now and waits for it.
    auto fires() -> bool {
        Fired fired;
        Timer timer;
        auto *const pf = &fired;
        auto const deadline = SteadyClock::now() + REQUESTED;
        timer.activate(Deadline::at(deadline), [pf](Timer *) {
            pf->done.store(true, std::memory_order_release);
            return Timer::HandlerResult::Handled;
        });

        auto const give_up = deadline + LOST_AFTER;
        auto const arrived = [&] {
            while (!fired.done.load(std::memory_order_acquire)) {
                if (SteadyClock::now() >= give_up) {
                    return false;
                }
                arch::spin_hint();
            }
            return true;
        }();
        auto _ = timer.cancel();
        return arrived;
    }

    auto fire_some() -> usize {
        usize fired = 0;
        for (usize i = 0; i < SAMPLES; ++i) {
            if (fires()) {
                fired += 1;
            }
        }
        return fired;
    }

    /// Spins until |done| holds or REPLACE_WITHIN has passed. Not a sleep: a
    /// sleep is a timer, and until the CPU has a device that works there is
    /// none to fire it.
    template <typename Condition>
    auto spin_until(Condition const &done) -> bool {
        auto const give_up = SteadyClock::now() + REPLACE_WITHIN;
        while (!done()) {
            if (SteadyClock::now() >= give_up) {
                return false;
            }
            arch::spin_hint();
        }
        return true;
    }

    auto enroll_refusing(CpuNum const cpu) -> RefusingDevice * {
        auto *const device = new (mm::GAF_KERNEL) RefusingDevice(cpu);
        if (device == nullptr) {
            return nullptr;
        }
        ClockEventDevice::enroll(device->device(), 1'000'000'000, 0xF, 0x7FFFFFFF);
        return device;
    }

    auto on_device(ClockEventDevice *const device) -> bool {
        arch::AutoInterruptDisabler const no_interrupts;
        auto &tick = TickDevice::Current::get();
        return tick.device() == device && tick.group() == nullptr;
    }

    auto worker(void *arg) -> i32 {
        auto &result = *static_cast<Result *>(arg);
        auto const refusals_before = TimerQueue::num_refused_programs();

        ClockEventDevice *own = nullptr;
        {
            arch::AutoInterruptDisabler const no_interrupts;
            own = TickDevice::Current::get().device();
        }

        // The first case: the CPU's own device is fine.
        if (auto *const first = enroll_refusing(result.cpu); first != nullptr) {
            result.own_back = spin_until([own] { return on_device(own); });
            result.first_failed = first->device().is_failed();
            if (result.own_back) {
                result.first_fired = fire_some();
            }
        }

        // The second: it is not, so the global device has to take the tick.
        if (result.own_back) {
            own->mark_failed();
            if (auto *const second = enroll_refusing(result.cpu); second != nullptr) {
                result.on_group = spin_until([group = result.group] {
                    arch::AutoInterruptDisabler const no_interrupts;
                    return TickDevice::Current::get().group() == group;
                });
                result.second_failed = second->device().is_failed();
                if (result.on_group) {
                    result.second_fired = fire_some();
                }
            }

            // Back to its own, whatever happened.
            {
                arch::AutoInterruptDisabler const no_interrupts;
                auto &tick = TickDevice::Current::get();
                if (tick.group() != nullptr || tick.device() != own) {
                    auto _ = tick.replace_with(*own, nullptr);
                }
            }
            own->clear_failed();
            result.restored = on_device(own);
        }

        result.refusals = TimerQueue::num_refused_programs() - refusals_before;

        // Last touch of |result| by this thread.
        result.done.store(true, std::memory_order_release);
        return 0;
    }

    struct Find {
        ClockEventDevice *global = nullptr;
    };

    auto find_global(ClockEventDevice &device, void *context) -> bool {
        auto &find = *static_cast<Find *>(context);
        if (!device.is_failed() && device.has_feature(ClockEventDevice::Feature::Oneshot) &&
            device.cpumask().count() > 1 && &device == time::global_clock_event()) {
            find.global = &device;
        }
        return true;
    }

    auto tick_replace_command(int, char **) -> int {
        Tally tally;

        if (!TickDevice::Current::is_mode_oneshot()) {
            log::info("tickreplace.test: ok -- skipped, kernel.time.oneshot is off");
            return 0;
        }
        Find find;
        ClockEventDevice::for_each_enrolled(find_global, &find);
        auto const num_cpus = Cpu::num_online_cpus();
        if (find.global == nullptr || num_cpus < 2) {
            log::info("tickreplace.test: ok -- skipped, needs a global timer device and a CPU "
                      "besides the boot CPU");
            return 0;
        }
        auto *const group = TickBroadcast::for_device(*find.global);
        if (group == nullptr) {
            log::error("tickreplace.test: FAILED -- the global device has no broadcast group");
            return 1;
        }

        auto *const result = new (mm::GAF_KERNEL) Result{};
        if (result == nullptr) {
            log::error("tickreplace.test: FAILED -- out of memory");
            return 1;
        }
        result->cpu = static_cast<CpuNum>(num_cpus - 1);
        result->group = group;

        auto thread = task::Thread::spawn("tickreplace", worker, result, task::DEFAULT_PRIORITY);
        if (!thread) {
            log::error("tickreplace.test: FAILED -- cannot spawn the worker; leaking the case");
            return 1;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(result->cpu));
        auto handle = ustl::move(thread).start();

        auto const give_up = SteadyClock::now() + GIVE_UP;
        while (!result->done.load(std::memory_order_acquire)) {
            if (SteadyClock::now() >= give_up) {
                log::error("tickreplace.test: FAILED -- CPU[{}] did not finish; leaking the case",
                           result->cpu);
                return 1;
            }
            auto _ = task::Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }

        log::info("tickreplace.test: CPU[{}], global device {}", result->cpu, find.global->name());
        verdict(tally, result->own_back,
                "refused by the device it was given, the CPU goes back to its own");
        verdict(tally, result->first_failed, "and the device that refused is marked failed");
        verdict(tally, result->first_fired == SAMPLES, "and its timers fire");
        verdict(tally, result->on_group,
                "with its own device failed too, the CPU moves to the global device");
        verdict(tally, result->second_failed, "and the second refusing device is marked failed");
        verdict(tally, result->second_fired == SAMPLES, "and its timers fire there");
        verdict(tally, result->restored, "it is put back on its own device afterwards");
        verdict(tally, !group->has_member(result->cpu), "and has left the group");
        verdict(tally, result->refusals >= 2, "each refusal was counted by the timer queue");

        delete result;
        if (tally.failed != 0) {
            log::error("tickreplace.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("tickreplace.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(TickReplaceTest, "tickreplace.test", ours::tick_replace_command,
             "give a CPU a tick device that refuses, and check it replaces it");
