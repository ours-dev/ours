/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `hotplug.test`: the highest online CPU taken offline, halted, and brought
// back through the AP trampoline, twice (task::take_cpu_offline(),
// task::bring_cpu_online(); steps 2 and 3 of .agents/tasks/cpu-hotplug/
// PLAN.md). Without CPU hotplug built in, only that both answer
// NotSupported. What it checks, in order:
//
//   - the refusals: a CPU that is not possible (InvArgs), the boot CPU
//     (NotSupported), bringing back a CPU that is online (BadState);
//   - threads spinning across every CPU, some of them on the one going
//     offline, and a thread asleep there whose timer is on its queue;
//   - once it is offline: it is neither online nor active, its idle thread
//     says so, no spinner runs there and every one still runs, the sleeper
//     wakes on time elsewhere (its timer moved with the queue), and a
//     sync_exec() over every possible CPU returns;
//   - once it is back: online, active, a thread pinned to it runs there and
//     sleeps and wakes there (its timers run again), and a Dpc enqueued there
//     runs there (its workers run again);
//   - the same a second time;
//   - a thread running on the CPU as it goes offline, moved to a CPU that is
//     idle, runs there at once, not at that CPU's next interrupt
//     (check_moved_to_idle());
//   - a sampling session stopped while the CPU is away starts again at once
//     after it is back (check_sampler());
//   - a CPU asked to go offline while a thread holds it with preemption off
//     misses the deadline, and the transition is withdrawn: it goes on
//     running (check_offline_withdrawn());
//   - a CPU started with its deadline already passed is withdrawn on its way
//     in, halts again when it arrives, stays offline, and a second start
//     brings it back (check_start_withdrawn()).
//
// No thread pinned to the CPU is alive while it goes offline: one that woke
// there would fail selection's assertion, as the design says it must.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/dpc.hpp>
#include <ours/ipi_service.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/object/sampler_dispatcher.hpp>
#include <ours/sampler.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    using task::IdleThread;
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    /// How long the spinners run with the CPU offline before they are first
    /// looked at.
    constexpr auto OFFLINE_FOR = Milliseconds(100);
    /// How long the sleeper sleeps, and how late it may wake. Long enough for
    /// the CPU to go offline meanwhile: that waits for its Dpc workers to
    /// get the CPU, behind the spinners on it.
    constexpr auto SLEEP_FOR = Seconds(1);
    constexpr auto WAKE_LATE_BY = Milliseconds(500);
    /// Spinners per online CPU, so that some are on every CPU.
    constexpr usize SPINNERS_PER_CPU = 2;
    constexpr usize MAX_SPINNERS = SPINNERS_PER_CPU * MAX_CPU;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> bool {
        if (ok) {
            tally.passed += 1;
            log::info("hotplug.test: pass  {}", name);
            return true;
        }
        tally.failed += 1;
        log::error("hotplug.test: FAIL  {}", name);
        return false;
    }

    auto deadline_after(Duration const delta) -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + delta);
    }

    template <typename Condition>
    auto poll_until(Condition const &condition) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!condition()) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            auto _ = Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        return true;
    }

    /// One spinner's record: how far it has got, whether it ran on the CPU
    /// before it went offline, and whether it ran there while it was.
    struct Spinner {
        std::atomic<u64> rounds{0};
        std::atomic<bool> ran_there{false};
        std::atomic<bool> ran_there_offline{false};
    };

    /// The test's state, on the heap and freed only once every thread that
    /// reads it has been joined.
    struct State {
        CpuNum target = INVALID_CPU_NUM;
        std::atomic<bool> stop{false};
        std::atomic<bool> offline{false};
        usize num_spinners = 0;
        Spinner spinners[MAX_SPINNERS];
        Thread *spinner_threads[MAX_SPINNERS]{};

        /// The sleeper: asleep on the target, with its timer there.
        std::atomic<bool> asleep_there{false};
        std::atomic<bool> woke{false};
        std::atomic<CpuNum> woke_on{INVALID_CPU_NUM};
        TimePoint<> slept_at;
        TimePoint<> woke_at;

        /// The pinned probe run after the CPU is back.
        std::atomic<CpuNum> pinned_ran_on{INVALID_CPU_NUM};
        std::atomic<CpuNum> pinned_woke_on{INVALID_CPU_NUM};
        std::atomic<u32> dpc_runs{0};
        std::atomic<CpuNum> dpc_ran_on{INVALID_CPU_NUM};
    };

    struct SpinnerArg {
        State *state;
        Spinner *spinner;
    };
    SpinnerArg SPINNER_ARGS[MAX_SPINNERS];

    /// The CPU and the flag are read with interrupts off, so in one instant:
    /// read one after the other with them on, a spinner could read the
    /// target as its CPU, be moved off it as the CPU went offline, and read
    /// the flag set, having run there only before. The flag is set only once
    /// the CPU is offline and cleared before it comes back.
    auto spinner_routine(void *const arg) -> i32 {
        auto &[state, spinner] = *static_cast<SpinnerArg *>(arg);
        while (!state->stop.load(std::memory_order_acquire)) {
            {
                arch::AutoInterruptDisabler const no_interrupts;
                if (Cpu::id() == state->target) {
                    spinner->ran_there.store(true, std::memory_order_relaxed);
                    if (state->offline.load(std::memory_order_acquire)) {
                        spinner->ran_there_offline.store(true, std::memory_order_relaxed);
                    }
                }
            }
            spinner->rounds.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    /// Started pinned to the target, so that it is there; it widens its own
    /// affinity before it sleeps, so that it is not pinned when the CPU goes
    /// offline, while its sleep's timer is on the target's queue.
    auto sleeper_routine(void *const arg) -> i32 {
        auto &state = *static_cast<State *>(arg);
        auto *const self = Thread::Current::get();
        self->set_cpu_affinity_hard(cpu_possible_mask());
        if (Cpu::id() != state.target) {
            return 1;
        }
        state.slept_at = time::SteadyClock::now();
        state.asleep_there.store(true, std::memory_order_release);
        auto _ = Thread::Current::sleep_for(SLEEP_FOR, task::Interruptible::No);
        state.woke_at = time::SteadyClock::now();
        state.woke_on.store(Cpu::id(), std::memory_order_relaxed);
        state.woke.store(true, std::memory_order_release);
        return 0;
    }

    auto dpc_routine(Dpc *const dpc) -> void {
        auto &state = *dpc->arg<State>();
        state.dpc_ran_on.store(Cpu::id(), std::memory_order_relaxed);
        state.dpc_runs.fetch_add(1, std::memory_order_release);
    }

    /// Pinned to the target once it is back: where it runs, where it wakes
    /// after a sleep, and a Dpc it enqueues there.
    struct PinnedArg {
        State *state;
        Dpc *dpc;
    };

    auto pinned_routine(void *const arg) -> i32 {
        auto &[state, dpc] = *static_cast<PinnedArg *>(arg);
        state->pinned_ran_on.store(Cpu::id(), std::memory_order_relaxed);
        auto _ = Thread::Current::sleep_for(Milliseconds(10), task::Interruptible::No);
        state->pinned_woke_on.store(Cpu::id(), std::memory_order_relaxed);
        auto _ = DpcRunner::enqueue(*dpc, DpcRunner::Level::Normal);
        return 0;
    }

    auto spawn(char const *const name, Thread::StartRoutine const routine, void *const arg,
               ustl::Option<CpuNum> const pinned) -> Thread * {
        auto *const thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        if (pinned.is_some()) {
            thread->set_cpu_affinity_hard(CpuMask::of(pinned.value()));
        }
        auto _ = thread->resume();
        return thread;
    }

    auto join(Thread *const thread, i32 *const retcode = nullptr) -> bool {
        return thread != nullptr && thread->join(retcode, deadline_after(LOST_AFTER)) == Status::Ok;
    }

    auto count_task(void *const context) -> void {
        static_cast<std::atomic<u32> *>(context)->fetch_add(1, std::memory_order_relaxed);
    }

    /// A thread pinned to the target runs there and wakes there from a sleep,
    /// and a Dpc it enqueues there runs there: the CPU's timers and Dpc
    /// workers run.
    auto check_runs_there(Tally &tally, State &state) -> void {
        auto const target = state.target;
        state.dpc_runs.store(0, std::memory_order_relaxed);
        auto *const dpc = new (mm::ALLOC_KERNEL) Dpc(dpc_routine, &state);
        auto arg = PinnedArg{&state, dpc};
        Thread *pinned = nullptr;
        if (dpc != nullptr) {
            pinned =
                spawn("hotplug.test/pinned", pinned_routine, &arg, ustl::Option<CpuNum>(target));
        }
        auto const joined = join(pinned);
        verdict(tally,
                joined && state.pinned_ran_on.load(std::memory_order_relaxed) == target &&
                    state.pinned_woke_on.load(std::memory_order_relaxed) == target,
                "a thread pinned to it runs there, and wakes there from a sleep");
        auto const dpc_ran =
            poll_until([&state] { return state.dpc_runs.load(std::memory_order_acquire) == 1; }) &&
            state.dpc_ran_on.load(std::memory_order_relaxed) == target;
        verdict(tally, dpc_ran, "a Dpc enqueued there runs there");
        if (dpc_ran) {
            delete dpc;
        }
    }

    /// One round trip. False when the CPU could not be brought back, after
    /// which nothing more is tried.
    auto round_trip(Tally &tally, State &state, u32 const round) -> bool {
        auto const target = state.target;
        log::info("hotplug.test: round {}: CPU {}", round, target);

        // The sleeper, asleep on the target.
        state.asleep_there.store(false, std::memory_order_relaxed);
        state.woke.store(false, std::memory_order_relaxed);
        auto *const sleeper =
            spawn("hotplug.test/sleeper", sleeper_routine, &state, ustl::Option<CpuNum>(target));
        verdict(tally, sleeper != nullptr && poll_until([&state] {
                           return state.asleep_there.load(std::memory_order_acquire);
                       }),
                "a thread falls asleep on the CPU");

        auto const offline = task::take_cpu_offline(target, deadline_after(LOST_AFTER));
        auto const offline_at = time::SteadyClock::now();
        state.offline.store(true, std::memory_order_release);
        verdict(tally, offline == Status::Ok, "the CPU goes offline");
        verdict(tally, offline_at < state.slept_at + SLEEP_FOR,
                "while the thread sleeps there, its timer on the CPU's queue");
        verdict(tally,
                !Cpu::is_online(target) && !task::Scheduler::peek_active_mask().test(target) &&
                    IdleThread::Current::get(target).state() == IdleThread::State::Offline &&
                    IdleThread::Current::get(target).has_halted(),
                "and is neither online nor active, and its idle thread says Offline and halted");

        verdict(tally,
                join(sleeper) && state.woke_on.load(std::memory_order_relaxed) != target &&
                    state.woke_at - state.slept_at >= SLEEP_FOR &&
                    state.woke_at - state.slept_at < SLEEP_FOR + WAKE_LATE_BY,
                "the thread asleep there wakes on time, elsewhere");

        // Each spinner on its own: one left queued on the offline CPU makes
        // no progress, however much the others make. A spinner waits for its
        // turn behind the others on its CPU, and with three on one CPU that is
        // two slices, and four if a discipline lets each run two in a row: more
        // than OFFLINE_FOR, and a thread stays on the CPU it was placed on, so
        // the ones that have to share are the same throughout. The test
        // therefore waits for every spinner to move, as long as LOST_AFTER: a
        // stranded one is the one that never does.
        u64 rounds_before[MAX_SPINNERS];
        for (usize i = 0; i < state.num_spinners; ++i) {
            rounds_before[i] = state.spinners[i].rounds.load(std::memory_order_relaxed);
        }
        auto _ = Thread::Current::sleep_for(OFFLINE_FOR, task::Interruptible::No);
        auto const all_went_on = poll_until([&state, &rounds_before] {
            for (usize i = 0; i < state.num_spinners; ++i) {
                if (state.spinners[i].rounds.load(std::memory_order_relaxed) <= rounds_before[i]) {
                    return false;
                }
            }
            return true;
        });
        auto ran_there = false;
        for (usize i = 0; i < state.num_spinners; ++i) {
            ran_there |= state.spinners[i].ran_there_offline.load(std::memory_order_relaxed);
        }
        verdict(tally, !ran_there, "no spinner runs on it while it is offline");
        verdict(tally, all_went_on, "and every spinner goes on elsewhere");

        std::atomic<u32> answered{0};
        IpiService::sync_exec(cpu_possible_mask(), count_task, &answered);
        verdict(tally,
                answered.load(std::memory_order_relaxed) ==
                    static_cast<u32>(Cpu::online_mask().count()),
                "a sync_exec() over every possible CPU returns, answered by the online ones");

        state.offline.store(false, std::memory_order_release);
        auto const online = task::bring_cpu_online(target);
        if (!verdict(tally, online == Status::Ok, "the CPU comes back")) {
            return false;
        }
        verdict(tally,
                Cpu::is_online(target) && task::Scheduler::peek_active_mask().test(target) &&
                    IdleThread::Current::get(target).state() == IdleThread::State::Running,
                "and is online and active again");

        check_runs_there(tally, state);
        return true;
    }

    /// The mover's record: when it last ran on the target, and when and where
    /// it first ran anywhere else.
    struct Mover {
        CpuNum target = INVALID_CPU_NUM;
        std::atomic<bool> on_target{false};
        TimePoint<> last_on_target;
        TimePoint<> first_elsewhere;
        std::atomic<CpuNum> elsewhere{INVALID_CPU_NUM};
    };
    Mover MOVER;

    /// Spins on the target until it finds itself on another CPU. The CPU and
    /// the time are read with interrupts off, so in one instant.
    auto mover_routine(void *const arg) -> i32 {
        auto &mover = *static_cast<Mover *>(arg);
        for (;;) {
            arch::AutoInterruptDisabler const no_interrupts;
            auto const cpu = Cpu::id();
            auto const now = time::SteadyClock::now();
            if (cpu == mover.target) {
                mover.last_on_target = now;
                mover.on_target.store(true, std::memory_order_release);
                continue;
            }
            mover.first_elsewhere = now;
            mover.elsewhere.store(cpu, std::memory_order_release);
            return 0;
        }
    }

    /// How long a thread moved off a CPU going offline may wait to run on the
    /// idle CPU it is moved to. That CPU is woken by the move, so the wait is
    /// an IPI and the rest of the offline; left to its next interrupt, it
    /// would be up to a second, its lockup heartbeat's period.
    constexpr auto MOVED_RUNS_WITHIN = Milliseconds(50);

    /// A thread running on the target, free to run on one other CPU, which is
    /// idle, is moved there as the target goes offline (Scheduler::Current::
    /// deactivate()). With no tick, an idle CPU sleeps until something wakes
    /// it, so the move has to.
    auto check_moved_to_idle(Tally &tally, CpuNum const target) -> void {
        auto helper = INVALID_CPU_NUM;
        Cpu::online_mask().for_each([&helper, target](CpuNum const cpu) {
            if (cpu != target && cpu != time::ClockSource::Default::cpunum() && cpu != Cpu::id()) {
                helper = cpu;
            }
        });
        if (helper == INVALID_CPU_NUM) {
            log::info("hotplug.test: the move to an idle CPU is skipped -- no CPU to spare");
            return;
        }

        MOVER.target = target;
        MOVER.on_target.store(false, std::memory_order_relaxed);
        MOVER.elsewhere.store(INVALID_CPU_NUM, std::memory_order_relaxed);
        auto *const mover = spawn("hotplug.test/move", mover_routine, &MOVER, target);
        auto const there = mover != nullptr && poll_until([] {
                               return MOVER.on_target.load(std::memory_order_acquire);
                           });
        if (!verdict(tally, there, "a thread runs on the CPU, free to move to one idle CPU")) {
            return;
        }
        mover->set_cpu_affinity_hard(CpuMask::of(target) | CpuMask::of(helper));

        auto const offline = task::take_cpu_offline(target, deadline_after(LOST_AFTER));
        auto const moved =
            offline == Status::Ok && poll_until([] {
                return MOVER.elsewhere.load(std::memory_order_acquire) != INVALID_CPU_NUM;
            });
        auto waited = Duration{};
        if (moved) {
            waited = MOVER.first_elsewhere - MOVER.last_on_target;
            log::info("hotplug.test: moved to CPU {}, off the CPU for {} us",
                      MOVER.elsewhere.load(std::memory_order_relaxed), waited.count() / 1000);
        }
        verdict(tally,
                moved && MOVER.elsewhere.load(std::memory_order_relaxed) == helper &&
                    waited <= MOVED_RUNS_WITHIN,
                "the CPU goes offline, and the thread runs on the idle CPU at once");
        verdict(tally, join(mover), "the moved thread is joined");
        verdict(tally, task::bring_cpu_online(target) == Status::Ok, "the CPU comes back");
    }

    /// A sampling session across a CPU going offline: stopped while the CPU
    /// is away and started again as soon as it is back. The period is the
    /// longest there is, so that none of the session's timers fires in
    /// between, which is checked. The CPU's
    /// timer is cancelled as the CPU leaves (sampler::Session::on_cpu_offline()); had
    /// it moved with the CPU's queue, the stop would not reach it, an offline
    /// CPU being no target of its IPI, and the second start would find it
    /// still queued on the CPU that took it, which Timer::activate() asserts.
    auto check_sampler(Tally &tally, CpuNum const target) -> void {
        constexpr auto CONFIG = SamplerConfig{
            .period = SAMPLER_MAX_PERIOD,
            .buffer_size = sizeof(SamplerRecord),
            .targets = SamplerTargets::Kernel,
            .reserved = 0,
        };
        object::KernelHandle<object::SamplerDispatcher> session;
        auto rights = Rights::None;
        auto const made = object::SamplerDispatcher::create(CONFIG, &session, &rights);
        if (made == Status::AlreadyExists) {
            log::info("hotplug.test: the sampler check is skipped -- another session holds the "
                      "claim");
            return;
        }
        auto const started_at = time::SteadyClock::now();
        if (!verdict(tally, made == Status::Ok && session.dispatcher()->start() == Status::Ok,
                     "a sampling session starts")) {
            return;
        }
        verdict(tally, task::take_cpu_offline(target, deadline_after(LOST_AFTER)) == Status::Ok,
                "the CPU goes offline while it samples");
        verdict(tally, session.dispatcher()->stop() == Status::Ok,
                "the session stops while the CPU is away");
        verdict(tally, task::bring_cpu_online(target) == Status::Ok, "the CPU comes back");
        verdict(tally, time::SteadyClock::now() - started_at < Nanoseconds(SAMPLER_MAX_PERIOD),
                "all within one period, before any of its timers fired");
        verdict(tally,
                session.dispatcher()->start() == Status::Ok &&
                    session.dispatcher()->stop() == Status::Ok,
                "and the session starts again at once, arming the CPU, and stops");
    }

    /// How long a transition is given in the checks that make it miss.
    constexpr auto WITHDRAW_AFTER = Milliseconds(50);

    /// The blocker's record: whether it holds the target, and when to let go.
    struct Blocker {
        std::atomic<bool> holding{false};
        std::atomic<bool> release{false};
    };
    Blocker BLOCKER;

    /// Pinned to the target, it holds it with preemption off until released:
    /// nothing else runs there meanwhile, the idle thread included.
    auto blocker_routine(void *const arg) -> i32 {
        auto &blocker = *static_cast<Blocker *>(arg);
        task::AutoPreemptDisabler const no_preemption;
        blocker.holding.store(true, std::memory_order_release);
        while (!blocker.release.load(std::memory_order_acquire)) {
            arch::spin_hint();
        }
        return 0;
    }

    /// A CPU asked to go offline whose idle thread cannot run before the
    /// deadline: the transition is withdrawn, and the CPU goes on as it was
    /// (IdleThread::transition()). Asked of the idle thread directly, under
    /// the hotplug lock, as take_cpu_offline() asks it: through
    /// take_cpu_offline() the blocker would hold up the Dpc workers' stop
    /// first, and the transition would not be asked for at all.
    auto check_offline_withdrawn(Tally &tally, State &state) -> void {
        auto const target = state.target;
        BLOCKER.holding.store(false, std::memory_order_relaxed);
        BLOCKER.release.store(false, std::memory_order_relaxed);
        auto *const blocker =
            spawn("hotplug.test/block", blocker_routine, &BLOCKER, ustl::Option<CpuNum>(target));
        auto const holding = blocker != nullptr && poll_until([] {
                                 return BLOCKER.holding.load(std::memory_order_acquire);
                             });
        if (!verdict(tally, holding, "a thread holds the CPU with preemption off")) {
            BLOCKER.release.store(true, std::memory_order_release);
            join(blocker);
            return;
        }

        auto &idler = IdleThread::Current::get(target);
        auto asked = Status::Ok;
        {
            Guard<sync::Mutex> guard{task::cpu_hotplug_lock()};
            asked = idler.transition(IdleThread::State::Offline, deadline_after(WITHDRAW_AFTER));
        }
        verdict(tally, asked == Status::TimeOut,
                "asked to go offline, the CPU misses the deadline");
        verdict(tally,
                idler.state() == IdleThread::State::Running && !idler.has_pending_transition() &&
                    Cpu::is_online(target) && task::Scheduler::peek_active_mask().test(target),
                "and the transition is withdrawn: online, active, and asked nothing");

        BLOCKER.release.store(true, std::memory_order_release);
        verdict(tally, join(blocker), "the thread that held it is joined");
        check_runs_there(tally, state);
    }

    /// A start whose deadline has passed by the time the start-up IPIs are
    /// sent: it is withdrawn while the CPU is on its way in. The CPU arrives,
    /// finds nothing asked of it, and halts again (IdleThread::come_back());
    /// it stays offline, and a second start brings it back.
    auto check_start_withdrawn(Tally &tally, State &state) -> void {
        auto const target = state.target;
        if (!verdict(tally,
                     task::take_cpu_offline(target, deadline_after(LOST_AFTER)) == Status::Ok,
                     "the CPU goes offline")) {
            return;
        }

        auto &idler = IdleThread::Current::get(target);
        auto started = Status::Ok;
        {
            Guard<sync::Mutex> guard{task::cpu_hotplug_lock()};
            started = idler.transition(IdleThread::State::Running,
                                       time::Deadline::at(time::SteadyClock::now()));
        }
        if (started == Status::Ok) {
            // The CPU claimed its return before the withdrawal was tried,
            // which takes this thread being held up between the start-up
            // IPIs and the next few instructions. Put back as
            // bring_cpu_online() would have.
            log::info("hotplug.test: the CPU arrived before its start could be withdrawn; that "
                      "case is not made");
            DpcRunner::start_workers(target);
            return;
        }
        verdict(tally, started == Status::TimeOut,
                "started with its deadline already passed, the CPU misses it");
        verdict(tally, poll_until([&idler] { return idler.has_halted(); }),
                "and, arriving late, halts again");
        verdict(tally,
                !Cpu::is_online(target) && !task::Scheduler::peek_active_mask().test(target) &&
                    idler.state() == IdleThread::State::Offline,
                "and stays offline");
        if (!verdict(tally, task::bring_cpu_online(target) == Status::Ok,
                     "a second start brings it back")) {
            return;
        }
        check_runs_there(tally, state);
    }

    auto hotplug_test_command(int, char **) -> int {
        Tally tally;

        auto target = INVALID_CPU_NUM;
        Cpu::online_mask().for_each([&target](CpuNum const cpu) {
            if (cpu != time::ClockSource::Default::cpunum()) {
                target = cpu;
            }
        });
        if (target == INVALID_CPU_NUM) {
            log::info("hotplug.test: ok -- no CPU but the boot CPU, nothing to take offline");
            return 0;
        }

#ifndef CONFIG_CPU_HOTPLUG
        verdict(tally,
                task::take_cpu_offline(target, deadline_after(LOST_AFTER)) ==
                        Status::NotSupported &&
                    task::bring_cpu_online(target) == Status::NotSupported,
                "without CPU hotplug built in, both answer NotSupported");
        if (tally.failed != 0) {
            log::error("hotplug.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("hotplug.test: ok -- {} checks, cpu_hotplug=false", tally.passed);
        return 0;
#endif
        verdict(tally,
                task::take_cpu_offline(MAX_CPU, deadline_after(LOST_AFTER)) == Status::InvArgs,
                "a CPU that is not possible is InvArgs");
        verdict(tally,
                task::take_cpu_offline(time::ClockSource::Default::cpunum(),
                                       deadline_after(LOST_AFTER)) == Status::NotSupported,
                "the boot CPU is NotSupported");
        verdict(tally, task::bring_cpu_online(target) == Status::BadState,
                "bringing back a CPU that is online is BadState");

        auto *const state = new (mm::ALLOC_KERNEL) State();
        if (state == nullptr) {
            log::error("hotplug.test: FAILED -- no memory for the test's state");
            return 1;
        }
        state->target = target;
        state->num_spinners = SPINNERS_PER_CPU * Cpu::online_mask().count();
        auto spawned = true;
        for (usize i = 0; i < state->num_spinners; ++i) {
            SPINNER_ARGS[i] = SpinnerArg{state, &state->spinners[i]};
            state->spinner_threads[i] =
                spawn("hotplug.test/spin", spinner_routine, &SPINNER_ARGS[i], ustl::NONE);
            spawned &= state->spinner_threads[i] != nullptr;
        }
        verdict(tally, spawned, "spinners start on every CPU");
        auto _ = Thread::Current::sleep_for(OFFLINE_FOR, task::Interruptible::No);
        auto some_there = false;
        for (usize i = 0; i < state->num_spinners; ++i) {
            some_there |= state->spinners[i].ran_there.load(std::memory_order_relaxed);
        }
        verdict(tally, some_there, "some of them on the CPU to be taken offline");

        auto back = round_trip(tally, *state, 1);
        if (back) {
            back = round_trip(tally, *state, 2);
        }

        state->stop.store(true, std::memory_order_release);
        auto joined = true;
        for (usize i = 0; i < state->num_spinners; ++i) {
            joined &= join(state->spinner_threads[i]);
        }
        verdict(tally, joined, "the spinners are joined");
        // With nothing else running: the CPU moved to has to be idle, and the
        // offline and back fit in one sampling period.
        if (back) {
            check_moved_to_idle(tally, target);
            check_sampler(tally, target);
            check_offline_withdrawn(tally, *state);
            check_start_withdrawn(tally, *state);
        }

        if (tally.failed != 0) {
            log::error("hotplug.test: FAILED -- {} of {} checks; the test's state is leaked",
                       tally.failed, tally.passed + tally.failed);
            return 1;
        }
        delete state;
        log::info("hotplug.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(
    HotplugTest, "hotplug.test", ours::hotplug_test_command,
    "take the highest CPU offline and bring it back, twice, with threads and timers on it");
