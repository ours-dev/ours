/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `irqrcu.test`: task::IrqRcuReadGuard, task::IrqRcuPointer and
// task::irq_rcu_synchronize() (.agents/tasks/irq-multi-arch/PLAN.md, step 8).
// dbg image only: it is the one with a kernel shell. What it checks, in order:
//
//   - the pointer: nothing before publish(), the object under a guard after
//     it, retract() answering it once and readers finding nothing after;
//   - the guard: interrupts off for its life, nested guards keep them off
//     until the outermost ends, and one taken with interrupts already off
//     changes nothing;
//   - irq_rcu_synchronize() with no reader returns at once, twice;
//   - the barrier, on two CPUs: a reader that has interrupts off on one CPU
//     keeps a synchronize() called from another from returning, and the call
//     returns after the reader leaves, having seen it leave;
//   - the stress: readers on every other CPU load a published object and
//     look at it for a while, a writer retracts it, synchronizes and marks it
//     dead; a reader must never find it dead. With synchronize() returning
//     early this is what fails;
//   - with CPU hotplug built in, the highest online CPU taken offline: the
//     call still returns, and the CPU comes back.
//
// It needs two online CPUs for the barrier and the stress, and says so
// if there is one.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/status.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/irq_rcu.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/interrupt.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::IrqRcuPointer;
    using task::IrqRcuReadGuard;
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(5);
    /// How long a reader holds its section while the writer is known to be
    /// waiting for it.
    constexpr auto HOLD = Milliseconds(50);
    /// What a synchronize() with nothing to wait for may take. Far above an
    /// inter-processor interrupt under emulation, far below LOST_AFTER.
    constexpr auto PROMPT = Milliseconds(500);
    constexpr usize STRESS_ROUNDS = 400;
    /// How many spins a stress reader looks at the object for: long enough
    /// that a writer which does not wait for it frees the object under it.
    constexpr usize LOOK_FOR = 2000;
    constexpr usize MAX_READERS = 2;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("irqrcu.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("irqrcu.test: FAIL  {}", name);
    }

    auto elapsed_since(TimePoint<> const start) -> Duration {
        return time::SteadyClock::now() - start;
    }

    /// Waits for |flag|, up to LOST_AFTER.
    auto wait_for(std::atomic<bool> const &flag) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!flag.load(std::memory_order_acquire)) {
            if (time::SteadyClock::now() > give_up) {
                return false;
            }
            auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
        }
        return true;
    }

    auto spawn_pinned(char const *const name, Thread::StartRoutine const routine, void *const arg,
                      CpuNum const cpu) -> Thread * {
        auto *const thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        auto _ = thread->resume();
        return thread;
    }

    auto join_within(Thread *const thread) -> bool {
        return thread != nullptr &&
               thread->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
                   Status::Ok;
    }

    struct Probe {
        std::atomic<bool> alive{false};
    };

    auto check_pointer(Tally &tally) -> void {
        IrqRcuPointer<Probe> pointer;
        Probe probe;
        {
            IrqRcuReadGuard const guard;
            verdict(tally, pointer.read(guard) == nullptr, "nothing is found before publish()");
        }
        pointer.publish(&probe);
        {
            IrqRcuReadGuard const guard;
            verdict(tally, pointer.read(guard) == &probe && pointer.read_with_irqs_off() == &probe,
                    "the object is found under a guard after publish()");
        }
        verdict(tally, pointer.retract() == &probe, "retract() answers the object");
        {
            IrqRcuReadGuard const guard;
            verdict(tally, pointer.read(guard) == nullptr, "a reader finds nothing after it");
        }
        verdict(tally, pointer.retract() == nullptr, "and a second retract() has nothing");
    }

    auto check_guard(Tally &tally) -> void {
        verdict(tally, !arch::irqs_disabled(), "the command runs with interrupts on");
        auto inside = false;
        auto nested = false;
        auto after_inner = false;
        {
            IrqRcuReadGuard const outer;
            inside = arch::irqs_disabled();
            {
                IrqRcuReadGuard const inner;
                nested = arch::irqs_disabled();
            }
            after_inner = arch::irqs_disabled();
        }
        verdict(tally, inside, "a guard has interrupts off");
        verdict(tally, nested && after_inner, "a nested guard keeps them off, also after it ends");
        verdict(tally, !arch::irqs_disabled(), "and the outermost one gives them back");

        auto kept_off = false;
        {
            arch::AutoInterruptDisabler const off;
            {
                IrqRcuReadGuard const guard;
            }
            kept_off = arch::irqs_disabled();
        }
        verdict(tally, kept_off && !arch::irqs_disabled(),
                "a guard taken with interrupts off changes nothing when it ends");
    }

    auto check_idle(Tally &tally) -> void {
        auto const first = time::SteadyClock::now();
        task::irq_rcu_synchronize();
        auto const first_took = elapsed_since(first);
        auto const second = time::SteadyClock::now();
        task::irq_rcu_synchronize();
        auto const second_took = elapsed_since(second);
        log::info("irqrcu.test: no reader: {} us, then {} us", first_took.count() / 1000,
                  second_took.count() / 1000);
        verdict(tally, first_took < Duration(PROMPT) && second_took < Duration(PROMPT),
                "synchronize() with no reader returns at once, twice");
    }

    /// The barrier case: one reader keeps its section open until released,
    /// one writer calls synchronize().
    struct Barrier {
        std::atomic<bool> in_section{false};
        std::atomic<bool> release{false};
        std::atomic<bool> left{false};
        std::atomic<bool> returned{false};
        std::atomic<bool> left_before_return{false};
    };
    Barrier BARRIER;

    auto barrier_reader(void *) -> i32 {
        {
            IrqRcuReadGuard const guard;
            BARRIER.in_section.store(true, std::memory_order_release);
            while (!BARRIER.release.load(std::memory_order_acquire)) {
                arch::spin_hint();
            }
            // The last act inside the section.
            BARRIER.left.store(true, std::memory_order_release);
        }
        return 0;
    }

    auto barrier_writer(void *) -> i32 {
        task::irq_rcu_synchronize();
        BARRIER.left_before_return.store(BARRIER.left.load(std::memory_order_acquire),
                                         std::memory_order_relaxed);
        BARRIER.returned.store(true, std::memory_order_release);
        return 0;
    }

    auto check_barrier(Tally &tally, CpuNum const reader_cpu, CpuNum const writer_cpu) -> void {
        BARRIER.in_section.store(false, std::memory_order_relaxed);
        BARRIER.release.store(false, std::memory_order_relaxed);
        BARRIER.left.store(false, std::memory_order_relaxed);
        BARRIER.returned.store(false, std::memory_order_relaxed);
        BARRIER.left_before_return.store(false, std::memory_order_relaxed);

        auto *const reader =
            spawn_pinned("irqrcu.test/reader", barrier_reader, nullptr, reader_cpu);
        auto const reading = wait_for(BARRIER.in_section);
        verdict(tally, reader != nullptr && reading, "the reader is in its section");

        auto *const writer =
            spawn_pinned("irqrcu.test/writer", barrier_writer, nullptr, writer_cpu);
        auto _ = Thread::Current::sleep_for(HOLD, task::Interruptible::No);
        verdict(tally, !BARRIER.returned.load(std::memory_order_acquire),
                "synchronize() does not return while a reader has interrupts off");

        BARRIER.release.store(true, std::memory_order_release);
        verdict(tally, join_within(reader), "the reader ends its section");
        verdict(tally, join_within(writer) && BARRIER.returned.load(std::memory_order_acquire),
                "and then synchronize() returns");
        verdict(tally, BARRIER.left_before_return.load(std::memory_order_relaxed),
                "having seen the reader leave");
    }

    /// The stress case: the object readers look at, and what each reader saw.
    struct Stress {
        IrqRcuPointer<Probe> pointer;
        Probe probe;
        std::atomic<bool> stop{false};
        std::atomic<u64> seen[MAX_READERS]{};
        std::atomic<u64> dead[MAX_READERS]{};
    };
    Stress STRESS;

    /// One read: whether it found the object. The guard ends with the call,
    /// so the reader's loop has interrupts on between two reads.
    auto look_once(usize const index) -> bool {
        IrqRcuReadGuard const guard;
        auto *const probe = STRESS.pointer.read(guard);
        if (probe == nullptr) {
            return false;
        }
        STRESS.seen[index].fetch_add(1, std::memory_order_relaxed);
        for (usize i = 0; i < LOOK_FOR; ++i) {
            arch::spin_hint();
        }
        if (!probe->alive.load(std::memory_order_acquire)) {
            STRESS.dead[index].fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    auto stress_reader(void *const arg) -> i32 {
        auto const index = *static_cast<usize *>(arg);
        while (!STRESS.stop.load(std::memory_order_acquire)) {
            if (!look_once(index)) {
                for (usize i = 0; i < 64; ++i) {
                    arch::spin_hint();
                }
            }
        }
        return 0;
    }

    auto check_stress(Tally &tally, CpuMask const &reader_cpus) -> void {
        STRESS.stop.store(false, std::memory_order_relaxed);
        STRESS.probe.alive.store(false, std::memory_order_relaxed);
        for (usize i = 0; i < MAX_READERS; ++i) {
            STRESS.seen[i].store(0, std::memory_order_relaxed);
            STRESS.dead[i].store(0, std::memory_order_relaxed);
        }

        usize indexes[MAX_READERS]{};
        Thread *readers[MAX_READERS]{};
        auto num_readers = usize{0};
        reader_cpus.for_each([&](CpuNum const cpu) {
            if (num_readers < MAX_READERS) {
                indexes[num_readers] = num_readers;
                readers[num_readers] =
                    spawn_pinned("irqrcu.test/look", stress_reader, &indexes[num_readers], cpu);
                num_readers += 1;
            }
        });

        for (usize round = 0; round < STRESS_ROUNDS; ++round) {
            STRESS.probe.alive.store(true, std::memory_order_release);
            STRESS.pointer.publish(&STRESS.probe);
            auto _ = Thread::Current::sleep_for(Microseconds(200), task::Interruptible::No);
            auto *const gone = STRESS.pointer.retract();
            task::irq_rcu_synchronize();
            // What a writer does once the object is no longer in use.
            gone->alive.store(false, std::memory_order_release);
        }
        STRESS.stop.store(true, std::memory_order_release);

        auto joined = true;
        for (usize i = 0; i < num_readers; ++i) {
            joined &= join_within(readers[i]);
        }
        auto seen = u64{0};
        auto dead = u64{0};
        for (usize i = 0; i < num_readers; ++i) {
            seen += STRESS.seen[i].load(std::memory_order_relaxed);
            dead += STRESS.dead[i].load(std::memory_order_relaxed);
        }
        log::info("irqrcu.test: {} rounds, {} readers saw the object {} times, {} of them dead",
                  STRESS_ROUNDS, num_readers, seen, dead);
        verdict(tally, joined && num_readers > 0, "the readers start and end");
        verdict(tally, seen > 0, "the readers did look at the object");
        verdict(tally, dead == 0, "and none found it dead");
    }

    auto check_offline(Tally &tally, CpuNum const cpu) -> void {
        auto const deadline = time::Deadline::at(time::SteadyClock::now() + LOST_AFTER);
        auto const taken = task::take_cpu_offline(cpu, deadline);
        if (taken == Status::NotSupported) {
            log::info("irqrcu.test: no CPU hotplug in this build, the offline case is skipped");
            return;
        }
        verdict(tally, taken == Status::Ok && !Cpu::online_mask().test(cpu),
                "a CPU is taken offline");
        if (taken == Status::Ok) {
            auto const start = time::SteadyClock::now();
            task::irq_rcu_synchronize();
            verdict(tally, elapsed_since(start) < Duration(PROMPT),
                    "synchronize() returns with it away");
            verdict(tally,
                    task::bring_cpu_online(cpu) == Status::Ok && Cpu::online_mask().test(cpu),
                    "and the CPU comes back");
            task::irq_rcu_synchronize();
            verdict(tally, true, "and synchronize() returns again with it back");
        }
    }

    auto irq_rcu_test_command(int, char **) -> int {
        Tally tally;

        check_pointer(tally);
        check_guard(tally);
        check_idle(tally);

        auto const online = Cpu::online_mask();
        if (online.count() < 2) {
            log::info("irqrcu.test: one CPU only, the barrier and the stress are skipped");
        } else {
            auto lowest = INVALID_CPU_NUM;
            auto highest = INVALID_CPU_NUM;
            online.for_each([&](CpuNum const cpu) {
                if (lowest == INVALID_CPU_NUM) {
                    lowest = cpu;
                }
                highest = cpu;
            });
            check_barrier(tally, highest, lowest);

            auto readers = online;
            readers.reset(lowest);
            check_stress(tally, readers);
            check_offline(tally, highest);
        }

        if (tally.failed != 0) {
            log::error("irqrcu.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("irqrcu.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(IrqRcuTest, "irqrcu.test", ours::irq_rcu_test_command,
             "irq RCU: the guard, the pointer, and synchronize() against readers with interrupts "
             "off on other CPUs, also with one taken offline");
