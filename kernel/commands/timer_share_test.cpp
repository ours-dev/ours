/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `timershare.test`: how much of a CPU a thread keeps when a periodic timer whose
// callback is slower than its period runs on that CPU.
//
// A timer of period P whose callback takes C ends each callback with
// forward(P), which puts the next deadline on the first multiple of P after the
// callback ended. The thread the interrupt came from then has
//
//     g = P - (C mod P)          (0 < g <= P)
//
// before the next interrupt, and keeps the share
//
//     s = g / (C + g)
//
// of the CPU, interrupt entry and exit left out
// (docs/architecture/timer/callback-cpu-share.md). This command measures s.
//
// One thread is pinned to one CPU, with preemption off so that only an
// interrupt can take the CPU from it. It does a fixed amount of work and times
// it with the TSC, once with nothing else on the CPU and once with the timer
// armed on the same CPU. The ratio of the two times is the share the timer left
// it. The timer is armed by the thread itself because a timer goes on the queue
// of the CPU that activated it.
//
// Neither the work nor the timing reads the clock source: the HPET is an MMIO
// read that the other CPUs' logging contends for, and a loop that reads it
// measures that contention. The callback burns its C on the TSC for the same
// reason, and records the C it really took and the spacing it really got,
// because the formula is only a fair comparison for those.
//
// The callback also gives up by itself a little after the work should have
// ended. A guard-less timer can leave the thread so little CPU that it never
// gets to stop the timer, and a measurement that cannot end is not one.
//
// A console command rather than a unit test for the reason mutex.stress gives:
// what is measured is the scheduler and the interrupt path together. It is
// named *.test so that scripts/kshell_run.py waits for its verdict line and
// not for the shell's prompt, which the dbg image's logging threads break up;
// the name has no dot or underscore for the reason vmaseal.test's does not.
//
// What it asserts is only that the thread is not starved: the formula's share
// is a number to compare with, not one to hold the kernel to; the guard that
// holds the device off after a slow pass (TimerQueue::m_hold_off_until)
// promises a half, which is what `big` holds it to.
//
// `timershare.test big` measures much slower callbacks, 0.5 to 4 ms. The
// emulated timer cannot interrupt faster than about every 160 us, so the gap
// the thread gets without the guard is that and not the formula's g, and the
// share is g_eff / (C + g_eff) for that g_eff: 24 %, 11 %, 4 % and 4 % at
// those four points. A callback some tens of times the gap leaves the thread
// what a callback some tens of times the gap leaves it on a device that
// interrupts every few microseconds, so these points show the starvation that
// a real machine would show at a much smaller C, without being its numbers.
// With the guard the device waits as long as the pass took, so the thread is
// left at least as much as the callbacks took: half the CPU, less what the
// emulated delivery adds, and the paired ratios are noisy by a few points.
#include <ours/arch/x86/apic.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>
#include <ours/time/timer_queue.hpp>

#include <array>
#include <atomic>
#include <iterator>
#include <string_view>

#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using time::Deadline;
    using time::SteadyClock;
    using time::Timer;

    constexpr u64 NSEC_PER_SEC = 1'000'000'000;

    constexpr u64 PERIOD_NS = 10'000;
    /// A callback of two periods, then four and a half and ten. 20 and 45 us
    /// are in the table of the note above; 100 us is where the share is at most
    /// P / (C + P), about 9 %.
    constexpr u64 COSTS_NS[] = {20'000, 45'000, 100'000};
    /// The slower callbacks of `big`: four points, about a second each.
    constexpr u64 BIG_COSTS_NS[] = {500'000, 1'000'000, 2'000'000, 4'000'000};
    /// How long the fixed work should take with nothing on the CPU.
    constexpr u64 TARGET_NS = 25'000'000;
    /// The probe that sizes the fixed work.
    constexpr u64 PROBE_SPINS = 200'000;
    /// The callback stops itself when the work has taken this many times
    /// TARGET_NS since the timer was armed.
    constexpr u64 GIVE_UP_TARGETS = 4;
    /// For `big`, where the thread keeps under a tenth of its CPU: it needs
    /// ten times the work's time and more to finish, and a callback that gave
    /// up before then would leave the rest of the work unloaded and the share
    /// too high.
    constexpr u64 BIG_GIVE_UP_TARGETS = 40;
    /// Each point is measured this many times, each time as an unloaded timing
    /// followed at once by a loaded one, and the share is that pair's ratio. The
    /// speed of the host drifts between repeats, so the unloaded timing of one
    /// repeat is no reference for the loaded timing of another: once the
    /// fastest of five unloaded timings was divided into the median of five
    /// loaded ones, and a CPU the host had briefly sped up looked starved. The
    /// shares are reported at their best and at their median.
    ///
    /// A command that runs for more than about four seconds on the dbg image
    /// is not seen to finish by scripts/kshell_run.py, which loses its prompt
    /// in the log lines of the threads that log forever (mutex.stress 4500
    /// does the same). Three points of this size take about two.
    constexpr usize REPEATS = 5;
    /// Reads of the clock source that are timed to say what one costs.
    constexpr u64 READ_PROBES = 1000;
    constexpr auto LOST_AFTER = Seconds(30);
    /// The least share, in tenths of a per cent, the thread may be left at the
    /// median of its repeats. Well below what the interrupts leave it here
    /// (above 40 %) and far above the starvation this guards against.
    constexpr u64 STARVED_BELOW_TENTHS = 250;
    /// The same for `big`, where the guard's rule applies: at least a half,
    /// with room for the noise of the paired ratios. The runs measured 55 % to
    /// 70 % with the guard and 4 % to 29 % without.
    constexpr u64 BIG_STARVED_BELOW_TENTHS = 450;
    constexpr auto POLL_INTERVAL = Milliseconds(5);

    /// What the callback needs and what it leaves behind. Written only by the
    /// callback, on the thread's CPU, with interrupts off, and read by the
    /// thread after cancel() has waited the callback out.
    struct Load {
        u64 cost_tsc = 0;
        u64 give_up_tsc = 0;
        std::atomic<bool> stop{false};

        u64 fires = 0;
        u64 first_entry_tsc = 0;
        u64 last_entry_tsc = 0;
        /// Entry to the end of forward(), summed over the fires.
        u64 busy_tsc = 0;
    };

    struct Run {
        u64 cost_ns = 0;
        u64 give_up_targets = GIVE_UP_TARGETS;
        u64 tsc_hz = 0;
        /// Every repeat's unloaded and loaded timing.
        std::array<u64, REPEATS> base_tsc{};
        std::array<u64, REPEATS> loaded_tsc{};
        u64 read_tsc = 0;
        /// Summed over the repeats.
        u64 fires = 0;
        u64 busy_tsc = 0;
        u64 span_tsc = 0;
        u64 span_fires = 0;
        std::atomic<bool> done{false};
    };

    /// Insertion sort: REPEATS is a handful, and this kernel's libc++ has no
    /// out-of-line std::sort for plain integers to link against.
    auto sort_ascending(std::array<u64, REPEATS> &values) -> void {
        for (usize i = 1; i < values.size(); ++i) {
            auto const value = values[i];
            auto j = i;
            while (j > 0 && values[j - 1] > value) {
                values[j] = values[j - 1];
                --j;
            }
            values[j] = value;
        }
    }

    auto tsc_of_ns(u64 const ns, u64 const hz) -> u64 {
        return ns * hz / NSEC_PER_SEC;
    }

    auto ns_of_tsc(u64 const tsc, u64 const hz) -> u64 {
        return tsc * NSEC_PER_SEC / hz;
    }

    /// Does |spins| spins and returns the TSC ticks they took. Runs with
    /// preemption off (share_thread() sees to it), so that the only thing that
    /// takes the CPU from the loop is an interrupt. The dbg image keeps threads
    /// that log forever, and when one of them shared this CPU the two timings
    /// differed by what the scheduler happened to give each, not by the timer.
    auto time_work(u64 const spins) -> u64 {
        auto const start = arch::Tick::get().count;
        for (u64 i = 0; i < spins; ++i) {
            arch::spin_hint();
        }
        return arch::Tick::get().count - start;
    }

    auto on_period(Load *const load, Timer *const self) -> Timer::HandlerResult {
        auto const entered = arch::Tick::get().count;
        if (load->stop.load(std::memory_order_acquire) || entered >= load->give_up_tsc) {
            return Timer::HandlerResult::Handled;
        }
        if (load->fires == 0) {
            load->first_entry_tsc = entered;
        }
        load->last_entry_tsc = entered;

        while (arch::Tick::get().count - entered < load->cost_tsc) {
            arch::spin_hint();
        }

        // As the sampler and the tick do: after the work, so the missed
        // periods are skipped and the next deadline is ahead of the end.
        auto _ = self->forward(Nanoseconds(PERIOD_NS));

        load->fires += 1;
        load->busy_tsc += arch::Tick::get().count - entered;
        return Timer::HandlerResult::Restart;
    }

    auto share_thread(void *arg) -> i32 {
        auto &run = *static_cast<Run *>(arg);

        // For the whole measurement and not only each timing: between two
        // timings the scheduler would hand this CPU to the dbg image's
        // logging thread for a full slice, and the command took three seconds
        // to do about one second of measuring.
        task::AutoPreemptDisabler const no_preemption;

        // What one read of the clock source costs on this CPU, now, with the
        // other CPUs doing what they do.
        {
            auto const start = arch::Tick::get().count;
            for (u64 i = 0; i < READ_PROBES; ++i) {
                auto _ = SteadyClock::now();
            }
            run.read_tsc = (arch::Tick::get().count - start) / READ_PROBES;
        }

        // Sizes the work so that it takes about TARGET_NS on this CPU.
        auto const probe_tsc = time_work(PROBE_SPINS);
        auto const target_tsc = tsc_of_ns(TARGET_NS, run.tsc_hz);
        auto const spins = PROBE_SPINS * target_tsc / probe_tsc;

        for (usize rep = 0; rep < REPEATS; ++rep) {
            run.base_tsc[rep] = time_work(spins);

            Load load;
            load.cost_tsc = tsc_of_ns(run.cost_ns, run.tsc_hz);
            load.give_up_tsc = arch::Tick::get().count + run.give_up_targets * target_tsc;
            auto *const pl = &load;
            Timer timer;
            timer.activate(Deadline::at(SteadyClock::now() + Nanoseconds(PERIOD_NS)),
                           [pl](Timer *const self) { return on_period(pl, self); });
            auto const loaded = time_work(spins);

            load.stop.store(true, std::memory_order_release);
            // Waits for a callback still running; after it, |load| is ours again.
            auto _ = timer.cancel();
            run.loaded_tsc[rep] = loaded;
            run.fires += load.fires;
            run.busy_tsc += load.busy_tsc;
            if (load.fires > 1) {
                run.span_tsc += load.last_entry_tsc - load.first_entry_tsc;
                run.span_fires += load.fires - 1;
            }
        }

        // Last touch of |run| by this thread.
        run.done.store(true, std::memory_order_release);
        return 0;
    }

    /// The share the formula gives for a callback of |cost_ns|, in tenths of a
    /// per cent as a plain integer, so that no floating point is needed in the
    /// kernel.
    auto formula_tenths(u64 const period_ns, u64 const cost_ns) -> u64 {
        auto const gap = period_ns - (cost_ns % period_ns);
        return 1000 * gap / (cost_ns + gap);
    }

    auto measure(u64 const cost_ns, u64 const give_up_targets, CpuNum const cpu, u64 &median_tenths)
        -> bool {
        auto *const run = new (mm::GAF_KERNEL) Run{};
        if (run == nullptr) {
            log::error("timershare.test: out of memory");
            return false;
        }
        auto const passes_before = time::TimerQueue::num_held_passes(cpu);
        auto const held_before = time::TimerQueue::held_off_ns(cpu);
        run->cost_ns = cost_ns;
        run->give_up_targets = give_up_targets;
        run->tsc_hz = apic_tsc_hz();
        if (run->tsc_hz == 0) {
            log::error("timershare.test: the TSC's rate is not known");
            delete run;
            return false;
        }

        auto *const thread =
            task::Thread::spawn("timer-share", share_thread, run, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            log::error("timershare.test: cannot spawn the measuring thread");
            delete run;
            return false;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        // detach() and resume() separately: detach_and_resume() is declared
        // but has no definition yet.
        auto _ = thread->detach();
        auto _ = thread->resume();

        auto const give_up = SteadyClock::now() + LOST_AFTER;
        while (!run->done.load(std::memory_order_acquire)) {
            if (SteadyClock::now() >= give_up) {
                log::error(
                    "timershare.test: the measuring thread did not finish; leaking its case");
                return false;
            }
            auto _ = task::Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }

        // Tenths of a per cent, and tenths of a microsecond.
        std::array<u64, REPEATS> shares{};
        for (usize rep = 0; rep < REPEATS; ++rep) {
            shares[rep] = 1000 * run->base_tsc[rep] / run->loaded_tsc[rep];
        }
        sort_ascending(shares);
        sort_ascending(run->base_tsc);
        auto const kept_best = shares.back();
        auto const kept_median = shares[REPEATS / 2];
        median_tenths = kept_median;
        u64 callback_ns = 0;
        u64 spacing_ns = 0;
        if (run->fires != 0) {
            callback_ns = ns_of_tsc(run->busy_tsc / run->fires, run->tsc_hz);
        }
        if (run->span_fires != 0) {
            spacing_ns = ns_of_tsc(run->span_tsc / run->span_fires, run->tsc_hz);
        }
        auto const formula = formula_tenths(PERIOD_NS, callback_ns);
        // What the thread has between the end of one callback and the entry of
        // the next: the formula's g on a device that is as fast as asked, the
        // device's own floor on one that is not.
        u64 gap_us = 0;
        if (spacing_ns > callback_ns) {
            gap_us = (spacing_ns - callback_ns) / 1000;
        }
        auto const read_ns = ns_of_tsc(run->read_tsc, run->tsc_hz);
        auto const alone_us = ns_of_tsc(run->base_tsc[REPEATS / 2], run->tsc_hz) / 1000;
        log::info(
            "timershare.test: P {} us, C asked {} us, took {}.{} us: the thread kept {}.{} % of "
            "its CPU at best and {}.{} % at the median (formula for that C: {}.{} %), one "
            "callback every {}.{} us; a clock read takes {}.{} us; the work alone takes {} us",
            PERIOD_NS / 1000, cost_ns / 1000, callback_ns / 1000, (callback_ns % 1000) / 100,
            kept_best / 10, kept_best % 10, kept_median / 10, kept_median % 10, formula / 10,
            formula % 10, spacing_ns / 1000, (spacing_ns % 1000) / 100, read_ns / 1000,
            (read_ns % 1000) / 100, alone_us);
        // A line of its own: the logger takes a limited number of arguments.
        // The passes the queue held the device off after, on that CPU: the
        // timer's own and the tick's, which are few beside it.
        auto const passes = time::TimerQueue::num_held_passes(cpu) - passes_before;
        u64 average_held_us = 0;
        if (passes != 0) {
            average_held_us = (time::TimerQueue::held_off_ns(cpu) - held_before) / passes / 1000;
        }
        log::info("timershare.test:   C {} us leaves the thread {} us between two callbacks; {} "
                  "passes held the device off for {} us on average",
                  cost_ns / 1000, gap_us, passes, average_held_us);
        delete run;
        return true;
    }

    auto timer_share_command(int const argc, char **const argv) -> int {
        auto const big = argc > 1 && std::string_view(argv[1]) == "big";
        if (argc > 1 && !big) {
            log::error("timershare.test: FAILED -- usage: timershare.test [big]");
            return 1;
        }

        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            log::error("timershare.test: FAILED -- needs a second CPU for the thread to be "
                       "measured on");
            return 1;
        }
        // Any CPU but the command's own; the thread is pinned there.
        auto const cpu = static_cast<CpuNum>(num_cpus - 1);

        // The same loop for both lists, over a pointer and a count.
        u64 const *costs = std::data(COSTS_NS);
        usize num_costs = std::size(COSTS_NS);
        auto give_up_targets = GIVE_UP_TARGETS;
        auto floor_tenths = STARVED_BELOW_TENTHS;
        if (big) {
            costs = std::data(BIG_COSTS_NS);
            num_costs = std::size(BIG_COSTS_NS);
            give_up_targets = BIG_GIVE_UP_TARGETS;
            floor_tenths = BIG_STARVED_BELOW_TENTHS;
        }

        usize measured = 0;
        usize starved = 0;
        for (usize i = 0; i < num_costs; ++i) {
            u64 median_tenths = 0;
            if (measure(costs[i], give_up_targets, cpu, median_tenths)) {
                measured += 1;
                if (median_tenths < floor_tenths) {
                    starved += 1;
                }
            }
        }
        if (measured != num_costs) {
            log::error("timershare.test: FAILED -- {} of {} runs measured", measured, num_costs);
            return 1;
        }
        if (starved != 0) {
            log::error("timershare.test: FAILED -- the thread kept under {}.{} % of its CPU in {} "
                       "run(s)",
                       floor_tenths / 10, floor_tenths % 10, starved);
            return 1;
        }
        log::info("timershare.test: ok -- {} runs, the thread never kept under {}.{} %", measured,
                  floor_tenths / 10, floor_tenths % 10);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(TimerShare, "timershare.test", ours::timer_share_command,
             "measure the CPU a thread keeps under a periodic timer slower than its period");
