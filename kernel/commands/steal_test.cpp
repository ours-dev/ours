/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `steal.test`: a CPU with nothing to run takes a queued thread from a busier
// CPU (Scheduler::Current::steal(), called by the idle thread before it halts),
// and where the NUMA node boundary falls. With one CPU holding the queue (the
// victim) and another idle and halted (the thief), each of a pair of CPUs on
// one node and a pair on two (when the machine has them):
//
//   - three threads that may run on either CPU, started pinned to the victim
//     and then let go: the thief is woken by a reschedule request and one of
//     them must run there, taken from the victim's queue, since nothing sleeps
//     or wakes to place one. This is the round that shows the thief can look
//     when woken, on either side of the boundary;
//   - two such threads, one running and one waiting: taken if the CPUs are on
//     one node, left alone if on two, where a second waiting thread is asked
//     for;
//   - three threads that may run only on the victim: the thief looks, the
//     queue is deep enough, and none of them may be taken. Only a thread the
//     thief is allowed to run is eligible, and nothing else decides this round;
//   - eight threads that may run on either: the thief takes its share of the
//     mean load of the CPUs it looks at, which is a number of threads that
//     depends on how many CPUs those are (two of eight over four CPUs, one over
//     eight), not one.
//
// The thief's own counts of looks and takes (Scheduler::Current::
// num_steal_looks(), num_steals()) tell a thief that looked and declined from
// one that was never woken, which no thread's position can.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/ipi_service.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/sched/domains.hpp>
#include <ours/task/sched/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::Thread;
    using task::sched::Scheduler;

    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    /// Long enough for the threads to be queued behind the one running, and
    /// for the other CPU to have halted.
    constexpr auto SETTLE = Milliseconds(100);
    /// How long a take that must not happen is waited for. The thief looks
    /// once, when the reschedule request wakes it, and what it does is decided
    /// within microseconds; this is a margin, and the thief's count of looks
    /// is what says it did look.
    constexpr auto NOT_TAKEN_FOR = Milliseconds(300);
    /// How long a round that takes several threads is watched: each thread the
    /// thief took gets a turn of a slice (64 ms), and up to four share the CPU.
    constexpr auto MANY_WATCH = Milliseconds(600);
    /// The most threads a round starts: 20 queue up behind the one running in the
    /// round that fills a batch.
    constexpr usize MAX_SPINNERS = 20;
    /// A spinner's CPUs are one bit each of a 32-bit word.
    static_assert(MAX_CPU <= 32, "a spinner records the CPUs it ran on in a u32");

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("steal.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("steal.test: FAIL  {}", name);
    }

    /// One spinning thread and the CPUs it has been seen on, one bit each.
    struct Spinner {
        std::atomic<bool> const *stop = nullptr;
        std::atomic<u32> cpus_seen{0};
    };

    /// What the spinners share. It is on the heap and freed only once every
    /// spinner is joined, so a spinner never reads a state that is gone, and a
    /// round never shares one with the next.
    struct State {
        std::atomic<bool> stop{false};
        Spinner spinners[MAX_SPINNERS];

        State() {
            for (auto &spinner: spinners) {
                spinner.stop = &stop;
            }
        }
    };

    /// The CPU is read with interrupts off, so that it is the CPU the read
    /// happened on.
    auto spinner_routine(void *const arg) -> i32 {
        auto &spinner = *static_cast<Spinner *>(arg);
        while (!spinner.stop->load(std::memory_order_acquire)) {
            arch::AutoInterruptDisabler const no_interrupts;
            spinner.cpus_seen.fetch_or(u32{1} << Cpu::id(), std::memory_order_relaxed);
        }
        return 0;
    }

    auto seen_on(Spinner const &spinner, CpuNum const cpu) -> bool {
        return (spinner.cpus_seen.load(std::memory_order_relaxed) & (u32{1} << cpu)) != 0;
    }

    struct Pair {
        CpuNum victim = INVALID_CPU_NUM;
        CpuNum thief = INVALID_CPU_NUM;
        bool same_node = false;
    };

    /// What one round does and what it saw.
    struct Round {
        /// Threads that start pinned to the victim and are then allowed on the
        /// thief, and threads that stay pinned to the victim.
        usize movable = 0;
        usize pinned = 0;
        /// Threads of the highest priority pinned to |heavy_cpu|, a third CPU:
        /// load that cannot move, and a lot of it.
        usize heavy = 0;
        CpuNum heavy_cpu = INVALID_CPU_NUM;
        /// How long to watch the thief for a spinner; the watch ends early
        /// when |stop_at_first| and one of the movable ones has been seen.
        Duration watch = LOST_AFTER;
        bool stop_at_first = true;

        bool set_up = false;
        bool movable_seen_on_thief = false;
        /// How many of the movable ones were seen on the thief by the end of
        /// the watch.
        usize movable_seen_count = 0;
        bool pinned_seen_on_thief = false;
        u64 looks = 0;
        u64 steals = 0;
    };

    /// Runs |round| on |pair|: the spinners start on the victim, the thief is
    /// woken by a reschedule request, and the thief is watched.
    auto run_round(Tally &tally, Pair const pair, Round &round) -> void {
        auto const total = round.movable + round.pinned + round.heavy;
        auto *const state = new (mm::ALLOC_KERNEL) State();
        if (state == nullptr) {
            log::error("steal.test: FAILED -- no memory for the test's state");
            return;
        }

        Thread *threads[MAX_SPINNERS]{};
        auto spawned = true;
        for (usize i = 0; i < total; ++i) {
            auto const is_heavy = i >= round.movable + round.pinned;
            auto priority = task::DEFAULT_PRIORITY;
            auto cpu = pair.victim;
            if (is_heavy) {
                priority = task::HIGHEST_PRIORITY;
                cpu = round.heavy_cpu;
            }
            threads[i] =
                Thread::spawn("steal.test/spin", spinner_routine, &state->spinners[i], priority);
            if (threads[i] == nullptr) {
                spawned = false;
                continue;
            }
            threads[i]->set_cpu_affinity_hard(CpuMask::of(cpu));
            // The thread is new, so it cannot already be running: resume()
            // only says whether it was, and nothing here depends on that.
            auto _ = threads[i]->resume();
        }
        verdict(tally, spawned, "the spinners start on one CPU");

        // The sleep result says only whether it was cut short, and the
        // interruptible mode is off, so it cannot be.
        auto _ = Thread::Current::sleep_for(SETTLE, task::Interruptible::No);
        auto kept_off = true;
        for (usize i = 0; i < total; ++i) {
            kept_off &= !seen_on(state->spinners[i], pair.thief);
        }
        verdict(tally, kept_off, "none runs on the other CPU while its affinity keeps it off");

        for (usize i = 0; i < round.movable; ++i) {
            if (threads[i] != nullptr) {
                threads[i]->set_cpu_affinity_hard(CpuMask::of(pair.victim) |
                                                  CpuMask::of(pair.thief));
            }
        }

        // The thief is idle and halted, with no tick: it looks at the queues
        // when something wakes it, so the test wakes it, as the reschedule
        // request a real wakeup would send.
        auto const looks_before = Scheduler::Current::num_steal_looks(pair.thief);
        auto const steals_before = Scheduler::Current::num_steals(pair.thief);
        IpiService::send_resched(pair.thief);

        auto const give_up = time::SteadyClock::now() + round.watch;
        while (time::SteadyClock::now() < give_up) {
            for (usize i = 0; i < round.movable; ++i) {
                round.movable_seen_on_thief |= seen_on(state->spinners[i], pair.thief);
            }
            if (round.movable_seen_on_thief && round.stop_at_first) {
                break;
            }
            auto _ = Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        for (usize i = 0; i < round.movable; ++i) {
            if (seen_on(state->spinners[i], pair.thief)) {
                round.movable_seen_count += 1;
            }
        }
        for (usize i = round.movable; i < total; ++i) {
            round.pinned_seen_on_thief |= seen_on(state->spinners[i], pair.thief);
        }
        round.looks = Scheduler::Current::num_steal_looks(pair.thief) - looks_before;
        round.steals = Scheduler::Current::num_steals(pair.thief) - steals_before;

        state->stop.store(true, std::memory_order_release);
        auto joined = true;
        for (usize i = 0; i < total; ++i) {
            joined &= threads[i] != nullptr &&
                      threads[i]->join(nullptr, time::Deadline::at(time::SteadyClock::now() +
                                                                   LOST_AFTER)) == Status::Ok;
        }
        verdict(tally, joined, "the spinners are joined");
        if (joined) {
            delete state;
        }
        round.set_up = joined && spawned;
    }

    /// The first two CPUs, from the highest, that are on one node, and the
    /// first two that are on two: the test thread is most likely on CPU 0, and
    /// it is better kept off the pair. Returns how many pairs were found.
    auto find_pairs(Pair (&pairs)[2]) -> usize {
        CpuNum cpus[MAX_CPU];
        usize count = 0;
        Cpu::online_mask().for_each([&](CpuNum const cpu) { cpus[count++] = cpu; });

        auto found_same = false;
        auto found_other = false;
        for (usize i = count; i-- > 0;) {
            for (usize j = i; j-- > 0;) {
                auto const same =
                    mm::CpuNodeMap::node_of(cpus[i]) == mm::CpuNodeMap::node_of(cpus[j]);
                if (same && !found_same) {
                    pairs[0] = Pair{.victim = cpus[i], .thief = cpus[j], .same_node = true};
                    found_same = true;
                }
                if (!same && !found_other) {
                    pairs[1] = Pair{.victim = cpus[i], .thief = cpus[j], .same_node = false};
                    found_other = true;
                }
            }
        }

        usize found = 0;
        if (found_same) {
            pairs[found++] = pairs[0];
        }
        if (found_other) {
            pairs[found++] = pairs[1];
        }
        return found;
    }

    /// |total| threads that may run on the victim or the thief queue up on the
    /// victim, the thief is woken, and what it takes is checked against what
    /// the mean of the CPUs it looks at allows. Nobody else can take them (they
    /// may run only on those two), so the count is the algorithm's.
    ///
    /// The mean is over the thief and every CPU of the tiers up to the one with
    /// the victim, and the load is |total| threads, or one more for the instant
    /// the test thread itself is runnable. Each thread weighs the same, so the
    /// first is taken, and one more for each whole thread the mean has left,
    /// up to the tier's batch: total / cpus, at least 1 and at most the batch.
    auto many_round(Tally &tally, Pair const pair, usize const total) -> void {
        static_assert(MAX_SPINNERS >= 20, "the largest round starts twenty threads");
        // Long enough for every thread taken to have had a turn on the thief.
        Round many{.movable = total, .watch = MANY_WATCH, .stop_at_first = false};
        run_round(tally, pair, many);

        usize cpus = 1;
        usize batch = 1;
        for (auto const &domain: task::sched::domains_of(pair.thief).view()) {
            cpus += (domain.span & Cpu::online_mask()).count();
            if (domain.span.test(pair.victim)) {
                batch = domain.max_batch;
                break;
            }
        }
        auto const share = [&](usize const threads) -> usize {
            auto taken = threads / cpus;
            if (taken > batch) {
                taken = batch;
            }
            if (taken < 1) {
                taken = 1;
            }
            return taken;
        };
        log::info("steal.test: took {} of {}, the mean over {} CPUs allows {} to {}, batch {}",
                  many.steals, total, cpus, share(total), share(total + 1), batch);
        verdict(tally, many.steals >= share(total) && many.steals <= share(total + 1),
                "the idle CPU takes its share of the mean");
        verdict(tally, many.movable_seen_count == many.steals,
                "and the threads it took are the ones seen running on it");
    }

    /// A third CPU of the thief's view, other than the victim: the first one
    /// in the tiers up to the one with the victim. INVALID_CPU_NUM on a machine
    /// of two.
    auto third_cpu(Pair const pair) -> CpuNum {
        for (auto const &domain: task::sched::domains_of(pair.thief).view()) {
            auto others = domain.span & Cpu::online_mask();
            others.set(pair.victim, false);
            auto found = INVALID_CPU_NUM;
            others.for_each([&](CpuNum const cpu) {
                if (found == INVALID_CPU_NUM) {
                    found = cpu;
                }
            });
            if (found != INVALID_CPU_NUM) {
                return found;
            }
            if (domain.span.test(pair.victim)) {
                break;
            }
        }
        return INVALID_CPU_NUM;
    }

    /// Three queue up on the victim, and a third CPU runs one thread of the
    /// highest priority, which weighs as many default threads as the machine has
    /// CPUs and more. Nothing waits behind it, so that CPU is not a victim, but
    /// the mean of the CPUs the thief looks at is above the victim's load (with
    /// no more than eight of them), so there is no amount to take. The thief
    /// takes the one thread that waits all the same, and no more: an idle CPU
    /// does not halt next to a waiting thread because other load raised the
    /// mean. (Four such threads would make that CPU the busiest, a victim whose
    /// threads cannot move, and hide this one: repair item 23.)
    auto heavy_round(Tally &tally, Pair const pair) -> void {
        auto const third = third_cpu(pair);
        if (third == INVALID_CPU_NUM) {
            log::info("steal.test: no third CPU in the thief's view, the heavy round is not run");
            return;
        }
        Round heavy{.movable = 3,
                    .heavy = 1,
                    .heavy_cpu = third,
                    .watch = NOT_TAKEN_FOR,
                    .stop_at_first = false};
        run_round(tally, pair, heavy);
        log::info("steal.test: with a heavy thread on CPU {}, took {} of 3", third, heavy.steals);
        verdict(tally, heavy.steals == 1,
                "the idle CPU takes the one waiting thread although the mean is above its victim");
    }

    auto test_pair(Tally &tally, Pair const pair) -> void {
        char const *node = "another";
        if (pair.same_node) {
            node = "the same";
        }
        log::info("steal.test: CPU {} has the queue, CPU {} is idle, on {} node", pair.victim,
                  pair.thief, node);

        // Three queue up behind the one running: worth taking from any node.
        Round deep{.movable = 3};
        run_round(tally, pair, deep);
        verdict(tally, deep.movable_seen_on_thief, "an idle CPU takes a thread that waits");
        verdict(tally, deep.steals >= 1, "and counts it");

        // One waits behind the one running: taken on the same node only.
        Round shallow{.movable = 2};
        if (!pair.same_node) {
            shallow.watch = NOT_TAKEN_FOR;
            shallow.stop_at_first = false;
        }
        run_round(tally, pair, shallow);
        verdict(tally, shallow.looks >= 1, "the idle CPU looked at the one that waits");
        if (pair.same_node) {
            verdict(tally, shallow.movable_seen_on_thief, "and took it, from its own node");
        } else {
            verdict(tally, !shallow.movable_seen_on_thief && shallow.steals == 0,
                    "and left it, from another node");
        }

        // Threads that may not run on the thief: it looks, and takes none.
        Round pinned{.pinned = 3, .watch = NOT_TAKEN_FOR, .stop_at_first = false};
        run_round(tally, pair, pinned);
        verdict(tally, pinned.looks >= 1, "the idle CPU looked at a queue of pinned threads");
        verdict(tally, !pinned.pinned_seen_on_thief && pinned.steals == 0, "and took none of them");

        // Eight queue up behind the one running: the thief takes its share of the
        // mean, which is less than a full batch.
        many_round(tally, pair, 8);
        // Twenty: more than a full batch is the mean's share on a machine of up
        // to five CPUs, and the batch is what is taken.
        many_round(tally, pair, 20);

        heavy_round(tally, pair);
    }

    auto steal_test_command(int, char **) -> int {
        if (Cpu::online_mask().count() < 2) {
            log::info("steal.test: ok -- 0 checks, one CPU only");
            return 0;
        }

        Pair pairs[2];
        auto const found = find_pairs(pairs);
        Tally tally;
        for (usize i = 0; i < found; ++i) {
            test_pair(tally, pairs[i]);
        }

        if (tally.failed != 0) {
            log::error("steal.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("steal.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(StealTest, "steal.test", ours::steal_test_command,
             "check that an idle CPU takes a queued thread from a busier one, by node and by "
             "affinity");
