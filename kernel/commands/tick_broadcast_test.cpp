/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `tickbcast.test`: a CPU whose tick is served by a global timer device
// through a broadcast group (time::TickBroadcast) still gets its timers, and
// goes back to its own device afterwards.
//
// The two last CPUs are moved, one worker thread pinned to each. Each worker
// swaps its CPU's tick from the local APIC timer to the HPET with
// TickDevice::replace_with(), the same call a CPU makes when its own device
// has failed, then arms SAMPLES timers of one millisecond one after another
// and waits for each, and last swaps back. With both CPUs in the group at
// once, the HPET is armed for the earlier of their deadlines and, when it
// fires on the boot CPU, the one whose deadline came is told by IPI; a timer
// that never fires is a deadline the group lost or an IPI it did not send.
//
// Needs kernel.time.oneshot, as a CPU only replaces a Oneshot device, and a
// global device that can be armed for one instant; without either it says so
// and passes.
//
// A console command rather than a unit test for the reason mutex.stress
// gives. It is named *.test so that scripts/kshell_run.py waits for its
// verdict line, as for timershare.test.
#include <ours/cpu.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_broadcast.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer.hpp>
#include <ours/time/timer_queue.hpp>

#include <array>
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

    constexpr usize SAMPLES = 20;
    constexpr auto REQUESTED = Milliseconds(1);
    /// A timer that has not fired this long after its deadline never will.
    constexpr auto LOST_AFTER = Milliseconds(200);
    constexpr auto GIVE_UP = Seconds(20);
    constexpr auto POLL_INTERVAL = Milliseconds(5);
    /// Not a tight bound: the group adds an IPI and the HPET's latency to
    /// what a CPU's own device would give, under TCG a good deal.
    constexpr auto MEDIAN_LIMIT = Milliseconds(5);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("tickbcast.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("tickbcast.test: FAIL  {}", name);
    }

    struct Member {
        CpuNum cpu = 0;
        ClockEventDevice *global = nullptr;
        TickBroadcast *group = nullptr;

        Status moved_in = Status::BadState;
        bool in_group = false;
        usize fired = 0;
        std::array<Duration, SAMPLES> lateness{};
        Status moved_out = Status::BadState;
        bool back_on_own = false;
        std::atomic<bool> done{false};
    };

    struct Find {
        ClockEventDevice *best = nullptr;
    };

    /// The best global device that can be armed for one instant.
    auto find_global(ClockEventDevice &device, void *context) -> bool {
        auto &find = *static_cast<Find *>(context);
        if (device.is_failed() || !device.has_feature(ClockEventDevice::Feature::Oneshot) ||
            device.cpumask().count() <= 1) {
            return true;
        }
        if (find.best == nullptr || device.rating() > find.best->rating()) {
            find.best = &device;
        }
        return true;
    }

    struct Fired {
        TimePoint<> at;
        std::atomic<bool> done{false};
    };

    /// Arms one timer for REQUESTED from now and waits for it. False if it had
    /// not fired LOST_AFTER past its deadline.
    auto sample(Duration &lateness) -> bool {
        Fired fired;
        Timer timer;
        auto *const pf = &fired;
        auto const deadline = SteadyClock::now() + REQUESTED;
        timer.activate(Deadline::at(deadline), [pf](Timer *) {
            pf->at = SteadyClock::now();
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

        // Waits out a handler still running, whichever way this ended.
        auto _ = timer.cancel();
        if (arrived) {
            lateness = fired.at - deadline;
        }
        return arrived;
    }

    auto worker(void *arg) -> i32 {
        auto &member = *static_cast<Member *>(arg);

        ClockEventDevice *own = nullptr;
        {
            arch::AutoInterruptDisabler const no_interrupts;
            auto &tick = TickDevice::Current::get();
            own = tick.device();
            member.moved_in = tick.replace_with(*member.global, member.group);
            member.in_group = tick.group() == member.group && tick.device() == member.global;
        }

        if (member.moved_in == Status::Ok) {
            for (usize i = 0; i < SAMPLES; ++i) {
                if (sample(member.lateness[member.fired])) {
                    member.fired += 1;
                }
            }

            arch::AutoInterruptDisabler const no_interrupts;
            auto &tick = TickDevice::Current::get();
            member.moved_out = tick.replace_with(*own, nullptr);
            member.back_on_own = tick.group() == nullptr && tick.device() == own;
        }

        // Last touch of |member| by this thread.
        member.done.store(true, std::memory_order_release);
        return 0;
    }

    auto median_of(std::array<Duration, SAMPLES> values, usize const count) -> Duration {
        for (usize i = 1; i < count; ++i) {
            auto const value = values[i];
            auto j = i;
            while (j > 0 && values[j - 1] > value) {
                values[j] = values[j - 1];
                --j;
            }
            values[j] = value;
        }
        return values[count / 2];
    }

    auto tick_broadcast_command(int, char **) -> int {
        Tally tally;

        if (!TickDevice::Current::is_mode_oneshot()) {
            log::info("tickbcast.test: ok -- skipped, kernel.time.oneshot is off");
            return 0;
        }
        Find find;
        ClockEventDevice::for_each_enrolled(find_global, &find);
        if (find.best == nullptr) {
            log::info("tickbcast.test: ok -- skipped, no global device can be armed for one "
                      "instant");
            return 0;
        }
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 3) {
            log::info("tickbcast.test: ok -- skipped, needs two CPUs besides the boot CPU");
            return 0;
        }

        auto *const group = TickBroadcast::for_device(*find.best);
        verdict(tally, group != nullptr, "the global device has a broadcast group");
        if (group == nullptr) {
            return 1;
        }
        log::info("tickbcast.test:   serving from {} through a group", find.best->name());

        auto *const members = new (mm::GAF_KERNEL) std::array<Member, 2>{};
        if (members == nullptr) {
            log::error("tickbcast.test: FAILED -- out of memory");
            return 1;
        }

        auto const ipis_before = TickBroadcast::num_ipis_sent();
        // Differences, as in timer.test: tickreplace.test refuses deadlines on
        // purpose, and a count since boot would fail this test after it.
        auto const refused_before =
            TickBroadcast::num_refused_programs() + TimerQueue::num_refused_programs();
        task::JoinHandle handles[2];
        for (usize i = 0; i < members->size(); ++i) {
            auto &member = (*members)[i];
            member.cpu = static_cast<CpuNum>(num_cpus - 1 - i);
            member.global = find.best;
            member.group = group;

            auto thread = task::Thread::spawn("tickbcast", worker, &member, task::DEFAULT_PRIORITY);
            if (!thread) {
                log::error("tickbcast.test: FAILED -- cannot spawn a worker; leaking the case");
                return 1;
            }
            thread->set_cpu_affinity_hard(CpuMask::of(member.cpu));
            handles[i] = ustl::move(thread).start();
        }

        auto const give_up = SteadyClock::now() + GIVE_UP;
        for (auto &member: *members) {
            while (!member.done.load(std::memory_order_acquire)) {
                if (SteadyClock::now() >= give_up) {
                    log::error("tickbcast.test: FAILED -- CPU[{}] did not finish; leaking the "
                               "case",
                               member.cpu);
                    return 1;
                }
                auto _ = task::Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
            }
        }

        for (auto &member: *members) {
            log::info("tickbcast.test: CPU[{}]", member.cpu);
            verdict(tally, member.moved_in == Status::Ok && member.in_group,
                    "its tick moves to the global device");
            verdict(tally, member.fired == SAMPLES, "every timer fires while it is in the group");
            if (member.fired != 0) {
                auto const median = median_of(member.lateness, member.fired);
                log::info("tickbcast.test:   {} us timers fired a median of {} us late",
                          duration_cast<Microseconds>(REQUESTED).count(),
                          duration_cast<Microseconds>(median).count());
                verdict(tally, median < MEDIAN_LIMIT, "and the median lateness is under 5 ms");
            }
            verdict(tally, member.moved_out == Status::Ok && member.back_on_own,
                    "it goes back to its own device");
            verdict(tally, !group->has_member(member.cpu), "and leaves the group");
        }

        verdict(tally, group->device().has_event_handler(),
                "the group's handler is still on the device once its last member has left");
        verdict(tally, TickBroadcast::num_ipis_sent() > ipis_before,
                "the group told a CPU by IPI that its timer was due");
        verdict(tally,
                TickBroadcast::num_refused_programs() + TimerQueue::num_refused_programs() ==
                    refused_before,
                "no device refused a deadline");

        delete members;
        if (tally.failed != 0) {
            log::error("tickbcast.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("tickbcast.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(TickBroadcastTest, "tickbcast.test", ours::tick_broadcast_command,
             "serve two CPUs' ticks from the HPET through a broadcast group and back");
