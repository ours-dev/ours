/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// IrqRcu.GuardPointerAndSynchronize: task::IrqRcuReadGuard, task::IrqRcuPointer
// and task::irq_rcu_synchronize() (.agents/tasks/irq-multi-arch/PLAN.md, step
// 8). dbg image only: it is the one with a kernel shell. What it checks, in
// order:
//
//   - the pointer: nothing before publish(), the object under a guard after
//     it, retract() answering it once and readers finding nothing after;
//   - the guard: interrupts off for its life, nested guards keep them off
//     until the outermost ends, and one taken with interrupts already off
//     changes nothing;
//   - irq_rcu_synchronize() with no reader returns at once, twice;
//   - the barrier, once for every online CPU but the writer's: a reader that
//     has interrupts off there keeps a synchronize() called from the writer's
//     CPU from returning for as long as the reader's section is open. The
//     reader decides, not a timer in the test: it starts its clock when
//     the writer says it is about to call, and looks at `returned` as the
//     last thing inside its section;
//   - the stress: a reader on every online CPU but the writer's loads an
//     object published for a while and looks at it, the writer retracts it,
//     synchronizes and marks it dead for good (a fresh object every round); a
//     reader must never find it dead, and every reader must have seen some;
//   - with CPU hotplug built in, a CPU other than the clock owner taken
//     offline: the call still returns, and once the CPU is back a reader
//     there is waited for.
//
// Why an in-kernel test: the guard's contract is about the real interrupt and
// preemption state of the CPU it is taken on, and synchronize() must be held
// by a reader with interrupts off on another CPU, and must wait for one taken
// offline -- several real CPUs' state at once, which a host test cannot stand
// up.
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
#include <ours/time/clock_source.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/interrupt.hpp>
#include <arch/spin_hint.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::IrqRcuPointer;
    using task::IrqRcuReadGuard;
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(5);
    /// How long a reader holds its section after the writer has said it is
    /// about to call synchronize().
    constexpr auto HOLD = Milliseconds(50);
    /// The longest a reader waits for the writer before it leaves anyway, so
    /// that a writer that never starts does not keep a CPU's interrupts off.
    constexpr auto GIVE_UP = Seconds(2);
    /// What a synchronize() with nothing to wait for may take. Far above an
    /// inter-processor interrupt under emulation, far below LOST_AFTER.
    constexpr auto PROMPT = Milliseconds(500);
    constexpr usize STRESS_ROUNDS = 400;
    /// How many spins a stress reader looks at the object for: long enough
    /// that a writer which does not wait for it marks the object dead under it.
    constexpr usize LOOK_FOR = 2000;

    auto elapsed_since(TimePoint<> const start) -> Duration {
        return time::SteadyClock::now() - start;
    }

    auto spawn_pinned(char const *const name, Thread::StartRoutine const routine, void *const arg,
                      CpuNum const cpu) -> Thread * {
        auto *const thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        // A thread that does not start is found by its join, which times out.
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

    auto check_pointer() -> void {
        IrqRcuPointer<Probe> pointer;
        Probe probe;
        {
            IrqRcuReadGuard const guard;
            EXPECT_NULL(pointer.read(guard), "nothing is found before publish()");
        }
        pointer.publish(&probe);
        {
            IrqRcuReadGuard const guard;
            EXPECT_TRUE(pointer.read(guard) == &probe && pointer.read_with_irqs_off() == &probe,
                        "the object is found under a guard after publish()");
        }
        EXPECT_EQ(pointer.retract(), &probe, "retract() answers the object");
        {
            IrqRcuReadGuard const guard;
            EXPECT_NULL(pointer.read(guard), "a reader finds nothing after it");
        }
        EXPECT_EQ(pointer.retract(), nullptr, "and a second retract() has nothing");
    }

    auto check_guard() -> void {
        EXPECT_TRUE(!arch::irqs_disabled(), "the command runs with interrupts on");
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
        EXPECT_TRUE(inside, "a guard has interrupts off");
        EXPECT_TRUE(nested && after_inner, "a nested guard keeps them off, also after it ends");
        EXPECT_TRUE(!arch::irqs_disabled(), "and the outermost one gives them back");

        auto kept_off = false;
        {
            arch::AutoInterruptDisabler const off;
            {
                IrqRcuReadGuard const guard;
            }
            kept_off = arch::irqs_disabled();
        }
        EXPECT_TRUE(kept_off && !arch::irqs_disabled(),
                    "a guard taken with interrupts off changes nothing when it ends");

        auto const count_before = Thread::Current::preemption_state().preemption_disabled_count();
        auto count_inside = count_before;
        {
            IrqRcuReadGuard const guard;
            count_inside = Thread::Current::preemption_state().preemption_disabled_count();
        }
        auto const count_after = Thread::Current::preemption_state().preemption_disabled_count();
        EXPECT_TRUE(count_inside == count_before + 1 && count_after == count_before,
                    "a guard in a thread also holds preemption off, and gives it back");
    }

    auto check_idle() -> void {
        auto const first = time::SteadyClock::now();
        task::irq_rcu_synchronize();
        auto const first_took = elapsed_since(first);
        auto const second = time::SteadyClock::now();
        task::irq_rcu_synchronize();
        auto const second_took = elapsed_since(second);
        log::info("irqrcu: no reader: {} us, then {} us", first_took.count() / 1000,
                  second_took.count() / 1000);
        EXPECT_TRUE(first_took < Duration(PROMPT) && second_took < Duration(PROMPT),
                    "synchronize() with no reader returns at once, twice");
    }

    /// The barrier case: one reader keeps its section open until the writer
    /// has been inside synchronize() for HOLD, one writer calls it.
    struct Barrier {
        std::atomic<bool> in_section{false};
        /// The writer is about to call; |entered_at| is written before it.
        std::atomic<bool> entered{false};
        TimePoint<> entered_at;
        std::atomic<bool> held_long_enough{false};
        std::atomic<bool> returned{false};
        /// The reader's last act inside its section: whether the call had
        /// returned by then.
        std::atomic<bool> returned_in_section{false};
    };
    Barrier BARRIER;

    auto barrier_reader(void *) -> i32 {
        IrqRcuReadGuard const guard;
        BARRIER.in_section.store(true, std::memory_order_release);
        auto const give_up = time::SteadyClock::now() + GIVE_UP;
        for (;;) {
            auto const now = time::SteadyClock::now();
            if (BARRIER.entered.load(std::memory_order_acquire) &&
                now >= BARRIER.entered_at + HOLD) {
                BARRIER.held_long_enough.store(true, std::memory_order_relaxed);
                break;
            }
            if (now > give_up) {
                break;
            }
            arch::spin_hint();
        }
        BARRIER.returned_in_section.store(BARRIER.returned.load(std::memory_order_acquire),
                                          std::memory_order_relaxed);
        return 0;
    }

    auto barrier_writer(void *) -> i32 {
        BARRIER.entered_at = time::SteadyClock::now();
        BARRIER.entered.store(true, std::memory_order_release);
        task::irq_rcu_synchronize();
        BARRIER.returned.store(true, std::memory_order_release);
        return 0;
    }

    auto wait_for_section() -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!BARRIER.in_section.load(std::memory_order_acquire)) {
            if (time::SteadyClock::now() > give_up) {
                return false;
            }
            auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
        }
        return true;
    }

    /// Whether the case held. A reader that never got into its section, or
    /// that gave up before the writer came, is a failure of the test.
    auto check_barrier(CpuNum const reader_cpu, CpuNum const writer_cpu) -> bool {
        log::info("irqrcu: barrier, reader on CPU {}, writer on CPU {}", reader_cpu, writer_cpu);
        BARRIER.in_section.store(false, std::memory_order_relaxed);
        BARRIER.entered.store(false, std::memory_order_relaxed);
        BARRIER.held_long_enough.store(false, std::memory_order_relaxed);
        BARRIER.returned.store(false, std::memory_order_relaxed);
        BARRIER.returned_in_section.store(false, std::memory_order_relaxed);

        auto *const reader =
            spawn_pinned("irqrcu.test/reader", barrier_reader, nullptr, reader_cpu);
        auto const reading = wait_for_section();
        auto *const writer =
            spawn_pinned("irqrcu.test/writer", barrier_writer, nullptr, writer_cpu);
        auto const joined_reader = join_within(reader);
        auto const joined_writer = join_within(writer);

        EXPECT_TRUE(reading && joined_reader && joined_writer,
                    "the reader is in its section and both threads end");
        EXPECT_TRUE(BARRIER.held_long_enough.load(std::memory_order_relaxed),
                    "the reader held its section for as long as the writer had been in the call");
        EXPECT_TRUE(!BARRIER.returned_in_section.load(std::memory_order_relaxed),
                    "synchronize() had not returned while the reader's section was open");
        EXPECT_TRUE(BARRIER.returned.load(std::memory_order_acquire),
                    "and it returns once the reader has left");
        return reading && joined_reader && joined_writer &&
               BARRIER.held_long_enough.load(std::memory_order_relaxed);
    }

    /// The stress case: a fresh object for every round, so a reader that is
    /// late finds it dead however long after the writer's mark it looks.
    struct Stress {
        IrqRcuPointer<Probe> pointer;
        Probe probes[STRESS_ROUNDS];
        std::atomic<bool> stop{false};
        std::atomic<u64> seen[MAX_CPU]{};
        std::atomic<u64> dead[MAX_CPU]{};
    };
    Stress STRESS;
    /// What each reader is told, by value in the thread's argument.
    CpuNum STRESS_CPUS[MAX_CPU];

    /// One read: whether it found the object.
    auto look_once(CpuNum const cpu) -> bool {
        IrqRcuReadGuard const guard;
        auto *const probe = STRESS.pointer.read(guard);
        if (probe == nullptr) {
            return false;
        }
        STRESS.seen[cpu].fetch_add(1, std::memory_order_relaxed);
        for (usize i = 0; i < LOOK_FOR; ++i) {
            if (!probe->alive.load(std::memory_order_acquire)) {
                STRESS.dead[cpu].fetch_add(1, std::memory_order_relaxed);
                break;
            }
            arch::spin_hint();
        }
        return true;
    }

    auto stress_reader(void *const arg) -> i32 {
        auto const cpu = *static_cast<CpuNum *>(arg);
        while (!STRESS.stop.load(std::memory_order_acquire)) {
            if (!look_once(cpu)) {
                for (usize i = 0; i < 64; ++i) {
                    arch::spin_hint();
                }
            }
        }
        return 0;
    }

    auto check_stress(CpuMask const &reader_cpus) -> bool {
        STRESS.stop.store(false, std::memory_order_relaxed);
        for (usize i = 0; i < MAX_CPU; ++i) {
            STRESS.seen[i].store(0, std::memory_order_relaxed);
            STRESS.dead[i].store(0, std::memory_order_relaxed);
        }
        for (auto &probe: STRESS.probes) {
            probe.alive.store(false, std::memory_order_relaxed);
        }

        Thread *readers[MAX_CPU]{};
        reader_cpus.for_each([&readers](CpuNum const cpu) {
            STRESS_CPUS[cpu] = cpu;
            readers[cpu] = spawn_pinned("irqrcu.test/look", stress_reader, &STRESS_CPUS[cpu], cpu);
        });

        for (usize round = 0; round < STRESS_ROUNDS; ++round) {
            auto &probe = STRESS.probes[round];
            probe.alive.store(true, std::memory_order_release);
            STRESS.pointer.publish(&probe);
            auto _ = Thread::Current::sleep_for(Microseconds(200), task::Interruptible::No);
            auto *const gone = STRESS.pointer.retract();
            task::irq_rcu_synchronize();
            // What a writer does once the object is no longer in use; never
            // undone, so a reader that is late finds it dead.
            gone->alive.store(false, std::memory_order_release);
        }
        STRESS.stop.store(true, std::memory_order_release);

        auto joined = true;
        auto every_reader_saw = true;
        auto seen = u64{0};
        auto dead = u64{0};
        reader_cpus.for_each([&](CpuNum const cpu) {
            joined &= join_within(readers[cpu]);
            auto const saw = STRESS.seen[cpu].load(std::memory_order_relaxed);
            every_reader_saw &= saw > 0;
            seen += saw;
            dead += STRESS.dead[cpu].load(std::memory_order_relaxed);
        });
        log::info("irqrcu: {} rounds, {} readers saw the object {} times, {} of them dead",
                  STRESS_ROUNDS, reader_cpus.count(), seen, dead);
        EXPECT_TRUE(joined, "the readers start and end");
        EXPECT_TRUE(
            every_reader_saw && seen >= STRESS_ROUNDS / 4,
            "every reader looked at the object, in at least a quarter of the rounds in all");
        EXPECT_EQ(dead, u64{0}, "and none found it dead");
        return joined;
    }

    auto check_offline(CpuNum const cpu, CpuNum const writer_cpu) -> void {
        auto const deadline = time::Deadline::at(time::SteadyClock::now() + LOST_AFTER);
        auto const taken = task::take_cpu_offline(cpu, deadline);
        if (taken == Status::NotSupported) {
            KTEST_SKIP("no CPU hotplug in this build, the offline case is skipped");
        }
        EXPECT_TRUE(taken == Status::Ok && !Cpu::online_mask().test(cpu), "a CPU is taken offline");
        if (taken != Status::Ok) {
            log::error("irqrcu:   take_cpu_offline answered {}", to_string(taken));
            return;
        }
        auto const start = time::SteadyClock::now();
        task::irq_rcu_synchronize();
        EXPECT_TRUE(elapsed_since(start) < Duration(PROMPT), "synchronize() returns with it away");
        auto const back = task::bring_cpu_online(cpu);
        EXPECT_TRUE(back == Status::Ok && Cpu::online_mask().test(cpu), "and the CPU comes back");
        if (back == Status::Ok) {
            // The CPU is waited for again: a reader there keeps the call out.
            check_barrier(cpu, writer_cpu);
        }
    }

    KTEST(IrqRcu, GuardPointerAndSynchronize) {
        check_pointer();
        check_guard();
        check_idle();

        auto const online = Cpu::online_mask();
        if (online.count() < 2) {
            KTEST_SKIP("one CPU only, the barrier, the stress and the offline case are skipped");
        }
        auto writer_cpu = INVALID_CPU_NUM;
        online.for_each([&writer_cpu](CpuNum const cpu) {
            if (writer_cpu == INVALID_CPU_NUM) {
                writer_cpu = cpu;
            }
        });
        auto readers = online;
        readers.reset(writer_cpu);

        // The test's thread runs on the writer's CPU for the cases below. A
        // reader holds its CPU with interrupts and preemption off, so a test
        // thread that shared that CPU would be parked behind it, and the
        // writer it has yet to start would wait for the reader to give up.
        // Affinity takes effect at the next sleep.
        auto *const self = Thread::Current::get();
        self->set_cpu_affinity_hard(CpuMask::of(writer_cpu));
        auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
        if (Cpu::id() != writer_cpu) {
            self->set_cpu_affinity_hard(cpu_possible_mask());
            FAIL("the test thread did not move to CPU {}", writer_cpu);
        }

        auto barriers_held = true;
        readers.for_each(
            [&](CpuNum const cpu) { barriers_held &= check_barrier(cpu, writer_cpu); });
        auto stress_ended = check_stress(readers);

        // The offline target is the highest CPU that is not the clock owner,
        // as hotplug.test picks it; and not while a thread left over from a
        // failed case may still be pinned somewhere.
        auto target = INVALID_CPU_NUM;
        online.for_each([&target](CpuNum const cpu) {
            if (cpu != time::ClockSource::Default::cpunum()) {
                target = cpu;
            }
        });
        if (!barriers_held || !stress_ended) {
            EXPECT_TRUE(false, "a case above left a thread behind; the offline case is not run");
        } else if (target == INVALID_CPU_NUM || target == writer_cpu) {
            log::info("irqrcu: no CPU to take offline but the writer's and the clock owner's, "
                      "the offline case is skipped");
        } else {
            check_offline(target, writer_cpu);
        }
        self->set_cpu_affinity_hard(cpu_possible_mask());
    }
} // namespace
} // namespace ours
