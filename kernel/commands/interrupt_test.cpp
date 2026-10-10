/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `interrupt.test [ROUNDS]`: the parts of an interrupt object's synchronous
// wait, and of its delivery through a port, that take a second thread
// (.agents/tasks/microkernel-interrupts/, steps 2 and 4). dbg image only: it
// is the one with a kernel shell, and with the assertions this relies on.
//
// The shell checks every answer one thread can see through the system calls,
// on both images. What it cannot do is park: a program has no second thread
// to wake it (there is no thread_start yet). So, on virtual interrupts and
// kernel threads:
//   - a trigger wakes a parked waiter, and its wait returns the trigger's
//     time;
//   - a second wait while one is parked answers BadState, and a destroy
//     wakes the parked one with Cancelled;
//   - the same while the first waiter has claimed the wait but not parked
//     yet, and a trigger has moved the state on from Waiting meanwhile: a
//     test hook holds the first waiter between begin_wait() and park(). The
//     second wait still answers BadState, and so does a bind; the first
//     takes the trigger's interrupt and parks in its next wait, and a destroy
//     wakes that one parked thread with Cancelled. Answered by the state
//     alone, the second wait took the interrupt and unsignalled the first
//     waiter's wakeup, so two threads could park where a destroy wakes one,
//     and the bind did the same to the first waiter on its own;
//   - a trigger on an interrupt bound to a port wakes a thread parked in
//     the port's dequeue with the Interrupt packet, and a destroy after that
//     packet was read answers NotFound;
//   - an interrupt triggered unbound, then sent by a bind, read, acked and
//     unbound, leaves no signal latched: the next wait parks until the next
//     trigger;
//   - two threads trigger and wait in turn, ROUNDS times, through two
//     interrupts, ping and pong: each wait must return the time of the
//     trigger it answers. On two CPUs the trigger lands sometimes before the
//     wait (the event latches it, and the wait unsignals it under the lock)
//     and sometimes after the waiter parked (the event wakes it); a latched
//     signal left standing would make a later wait answer BadState, and a
//     lost one would leave a waiter parked for good.
//
// A lost wakeup is told from a slow one by progress, not by a deadline for
// the whole run: the turns fail only once no round has finished for
// LOST_AFTER. A round on dev.dbg_1 took about 8 ms on 2026-10-04 (1000 in
// 7.9 s), and once about ten times that, with every reschedule IPI logged to
// the serial console: a fixed deadline for the whole run failed a correct
// run that way.
//
// A console command rather than a unit test for the reason mutex.stress
// gives: a wait's slow path is the scheduler.
#include <ours/cpu.hpp>
#include <ours/interrupt.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/object/virtual_interrupt_dispatcher.hpp>
#include <ours/port.hpp>
#include <ours/syscall.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>
#include <initializer_list>
#include <string_view>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using object::InterruptDispatcher;
    using object::KernelHandle;
    using object::PortDispatcher;
    using object::VirtualInterruptDispatcher;
    using task::JoinHandle;
    using task::Thread;

    constexpr usize DEFAULT_ROUNDS = 200;
    constexpr auto LOST_AFTER = Seconds(10);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    /// How often the turns' watcher looks at their progress: coarse, as it
    /// shares a CPU with one of the two threads.
    constexpr auto WATCH_INTERVAL = Milliseconds(100);
    /// The time the single trigger reports; any value a wait would not make
    /// up.
    constexpr auto TRIGGERED_AT = Instant{0x1A7E};
    /// The key the port check binds with; any value the packet would not
    /// carry by chance.
    constexpr auto PACKET_KEY = u64{0x1A7E'0000'0000'0004};

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("interrupt.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("interrupt.test: FAIL  {}", name);
    }

    /// Polls |condition| until it holds or LOST_AFTER has passed.
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

    /// Whether |thread| is parked. Read under its PI lock, which guards the
    /// state; |thread| must be one nobody can have freed.
    auto is_parked(Thread *const thread) -> bool {
        Guard<sync::SpinLock, lockdep::IrqSave> const guard{thread->get_pi_lock()};
        return thread->is_state_parked();
    }

    /// A started thread running |routine| on |cpu|, by its handle; empty if
    /// it could not be made.
    auto spawn_on(char const *const name, Thread::StartRoutine const routine, void *const arg,
                  CpuNum const cpu) -> JoinHandle {
        auto thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (!thread) {
            return {};
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        auto _ = thread->resume();
        return thread;
    }

    /// Whether |thread| ended within LOST_AFTER, reaped if so.
    auto joined(JoinHandle &thread) -> bool {
        i32 retcode = 0;
        return thread.join(&retcode, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
               Status::Ok;
    }

    auto make_interrupt(KernelHandle<InterruptDispatcher> *const out) -> bool {
        auto rights = Rights::None;
        return VirtualInterruptDispatcher::create(out, &rights, InterruptOptions::Virtual) ==
               Status::Ok;
    }

    /// One thread waiting on |interrupt|, and what its wait answered. On the
    /// heap, and freed only once the thread is reaped -- or leaked.
    struct Waiter {
        KernelHandle<InterruptDispatcher> interrupt;
        std::atomic<bool> waiting{false};
        /// Written by the waiter before it returns, read after the join.
        Status status = Status::Ok;
        Instant timestamp = -1;
    };

    auto waiter_routine(void *const arg) -> i32 {
        auto &waiter = *static_cast<Waiter *>(arg);
        waiter.waiting.store(true, std::memory_order_release);
        waiter.status = waiter.interrupt.dispatcher()->wait(&waiter.timestamp);
        return 0;
    }

    /// Makes a waiter on a new interrupt, runs |prepare| on that interrupt
    /// from this thread, and waits until the waiter has parked in wait(),
    /// with nothing pending. Empty if it did not get there; nothing is left
    /// behind then but what has been leaked and logged, and a thread that
    /// did not end, which runs on detached.
    template <typename Prepare>
    auto park_waiter(Tally &tally, CpuNum const cpu, Waiter **const out, Prepare const &prepare)
        -> JoinHandle {
        auto *const waiter = new (mm::GAF_KERNEL) Waiter{};
        if (waiter == nullptr || !make_interrupt(&waiter->interrupt)) {
            verdict(tally, false, "make a virtual interrupt");
            delete waiter;
            return {};
        }
        if (!prepare(*waiter->interrupt.dispatcher())) {
            verdict(tally, false, "bring the interrupt to the state the check starts from");
            delete waiter;
            return {};
        }
        auto thread = spawn_on("interrupt-test-waiter", waiter_routine, waiter, cpu);
        if (!thread) {
            verdict(tally, false, "spawn the waiter");
            delete waiter;
            return {};
        }
        auto const parked = poll_until([waiter, &thread] {
            return waiter->waiting.load(std::memory_order_acquire) && is_parked(thread.get());
        });
        verdict(tally, parked, "a wait with nothing pending parks");
        if (!parked) {
            // Whatever it is doing, a destroy ends its wait.
            auto _ = waiter->interrupt.dispatcher()->destroy();
            if (joined(thread)) {
                delete waiter;
            } else {
                log::error("interrupt.test:   leaking the waiter; its thread may still use it");
            }
            return {};
        }
        *out = waiter;
        return thread;
    }

    /// The same, on an interrupt fresh from its creation.
    auto park_waiter(Tally &tally, CpuNum const cpu, Waiter **const out) -> JoinHandle {
        return park_waiter(tally, cpu, out, [](InterruptDispatcher &) { return true; });
    }

    /// Reaps |thread| and answers whether it did; |waiter| is freed then, and
    /// leaked otherwise.
    auto finish_waiter(Tally &tally, JoinHandle &thread, Waiter *const waiter,
                       char const *const name) -> bool {
        auto const ended = joined(thread);
        verdict(tally, ended, name);
        if (!ended) {
            log::error("interrupt.test:   leaking the waiter; its thread may still use it");
        }
        return ended;
    }

    auto check_trigger_wakes(Tally &tally, CpuNum const cpu) -> void {
        Waiter *waiter = nullptr;
        auto thread = park_waiter(tally, cpu, &waiter);
        if (!thread) {
            return;
        }
        auto &interrupt = *waiter->interrupt.dispatcher();
        verdict(tally, interrupt.trigger(TRIGGERED_AT) == Status::Ok, "trigger");
        if (!finish_waiter(tally, thread, waiter, "a trigger wakes the parked waiter")) {
            return;
        }
        verdict(tally, waiter->status == Status::Ok && waiter->timestamp == TRIGGERED_AT,
                "the woken wait returns the trigger's time");
        delete waiter;
    }

    auto check_destroy_wakes(Tally &tally, CpuNum const cpu) -> void {
        Waiter *waiter = nullptr;
        auto thread = park_waiter(tally, cpu, &waiter);
        if (!thread) {
            return;
        }
        auto &interrupt = *waiter->interrupt.dispatcher();
        auto ignored = Instant{0};
        verdict(tally, interrupt.wait(&ignored) == Status::BadState,
                "a second wait while one is parked answers BadState");
        verdict(tally, interrupt.destroy() == Status::Ok, "destroy");
        if (!finish_waiter(tally, thread, waiter, "a destroy wakes the parked waiter")) {
            return;
        }
        verdict(tally, waiter->status == Status::Cancelled, "the woken wait answers Cancelled");
        delete waiter;
    }

    /// A waiter the test hook holds between begin_wait() and park(), and
    /// what its two waits answered. On the heap, and freed only once its
    /// thread is reaped -- or leaked.
    struct HeldWaiter {
        KernelHandle<InterruptDispatcher> interrupt;
        /// The object whose first waiter the hook holds; the hook lets every
        /// other wait go straight on.
        InterruptDispatcher const *target = nullptr;
        /// Set by the hook once it holds the first wait.
        std::atomic<bool> held{false};
        /// Set by the test thread to let the held wait go on to park().
        std::atomic<bool> release{false};
        /// Set by the waiter between its two waits, after writing what the
        /// first answered.
        std::atomic<bool> second_waiting{false};
        Status first_status = Status::Ok;
        Instant first_timestamp = -1;
        /// Written by the waiter before it returns, read after the join.
        Status second_status = Status::Ok;
    };

    /// The waiter the hook looks for; null while no check is holding one.
    // Namespace-scope objects are UPPER_CASE throughout this tree.
    // NOLINTNEXTLINE(readability-identifier-naming)
    constinit std::atomic<HeldWaiter *> HELD_WAITER{nullptr};

    /// The hook: holds the first wait on HELD_WAITER's object, after it has
    /// claimed the wait and before it parks, until the test thread lets it
    /// go. It sleeps rather than spins, so the waiter's CPU stays free.
    auto hold_before_park(InterruptDispatcher &interrupt) -> void {
        auto *const waiter = HELD_WAITER.load(std::memory_order_acquire);
        if (waiter == nullptr || waiter->target != &interrupt ||
            waiter->held.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        while (!waiter->release.load(std::memory_order_acquire)) {
            auto _ = Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
    }

    /// Two waits in a row: the hook holds the first; the second parks once
    /// the first has taken what was pending.
    auto held_waiter_routine(void *const arg) -> i32 {
        auto &waiter = *static_cast<HeldWaiter *>(arg);
        auto &interrupt = *waiter.interrupt.dispatcher();
        waiter.first_status = interrupt.wait(&waiter.first_timestamp);
        waiter.second_waiting.store(true, std::memory_order_release);
        auto ignored = Instant{0};
        waiter.second_status = interrupt.wait(&ignored);
        return 0;
    }

    /// Forces the interleaving in which two threads used to both park on one
    /// interrupt: the first waiter has claimed the wait and not parked yet,
    /// a trigger latches its signal and moves the state from Waiting to
    /// Triggered, and a second wait comes in. Taken by the state alone, the
    /// second wait took the interrupt and unsignalled the latch, the first
    /// then parked with nothing to wake it, and the second's next wait parked
    /// beside it; a destroy signals once and woke one of them.
    auto check_held_waiter(Tally &tally, CpuNum const cpu) -> void {
        KernelHandle<PortDispatcher> port;
        auto rights = Rights::None;
        if (PortDispatcher::create(PortCreateOptions::BindToInterrupt, &port, &rights) !=
            Status::Ok) {
            verdict(tally, false, "make a port to bind to");
            return;
        }
        auto *const waiter = new (mm::GAF_KERNEL) HeldWaiter{};
        if (waiter == nullptr || !make_interrupt(&waiter->interrupt)) {
            verdict(tally, false, "make a virtual interrupt");
            delete waiter;
            return;
        }
        auto &interrupt = *waiter->interrupt.dispatcher();
        waiter->target = &interrupt;
        HELD_WAITER.store(waiter, std::memory_order_release);
        InterruptDispatcher::set_before_park_hook_for_test(hold_before_park);
        auto const unhook = [] {
            InterruptDispatcher::set_before_park_hook_for_test(nullptr);
            HELD_WAITER.store(nullptr, std::memory_order_release);
        };

        auto thread = spawn_on("interrupt-test-held", held_waiter_routine, waiter, cpu);
        if (!thread) {
            verdict(tally, false, "spawn the waiter");
            unhook();
            delete waiter;
            return;
        }
        auto const held =
            poll_until([waiter] { return waiter->held.load(std::memory_order_acquire); });
        verdict(tally, held, "the hook holds the first wait between begin_wait() and park()");

        verdict(tally, interrupt.trigger(TRIGGERED_AT) == Status::Ok,
                "trigger while the first waiter is held");
        auto ignored = Instant{0};
        auto const second = interrupt.wait(&ignored);
        if (second != Status::BadState) {
            log::error("interrupt.test:   the second wait answered {}", to_string(second));
        }
        verdict(tally, second == Status::BadState,
                "a second wait answers BadState though the trigger moved the state on");
        // Bound, nothing would signal the held waiter's event again, and a
        // bind takes back the signal the trigger latched for it.
        verdict(tally, interrupt.bind(port.dispatcher(), PACKET_KEY) == Status::BadState,
                "and so does a bind, which would leave the held waiter parked for good");

        waiter->release.store(true, std::memory_order_release);
        auto const parked = poll_until([waiter, &thread] {
            return waiter->second_waiting.load(std::memory_order_acquire) &&
                   is_parked(thread.get());
        });
        verdict(tally, parked, "the held waiter goes on, and parks in its next wait");
        if (parked) {
            verdict(tally,
                    waiter->first_status == Status::Ok && waiter->first_timestamp == TRIGGERED_AT,
                    "its first wait took the interrupt the trigger raised while it was held");
            verdict(tally, interrupt.wait(&ignored) == Status::BadState,
                    "a wait while it is parked answers BadState");
        }

        verdict(tally, interrupt.destroy() == Status::Ok, "destroy");
        auto const ended = joined(thread);
        verdict(tally, ended, "a destroy wakes the one parked waiter");
        // Taken out once the waiter has ended, so that no wait of it can
        // still be in the hook. Nothing else waits on an interrupt object
        // while the kernel shell runs this test, so after this nothing reads
        // |waiter| through the hook.
        unhook();
        if (!ended) {
            log::error("interrupt.test:   leaking the held waiter; its thread may still use it");
            return;
        }
        verdict(tally, waiter->second_status == Status::Cancelled,
                "and its wait answers Cancelled");
        delete waiter;
    }

    /// One thread parked in a port's dequeue, and what it read. On the heap,
    /// and freed only once the thread is reaped -- or leaked.
    struct PortWaiter {
        KernelHandle<PortDispatcher> port;
        std::atomic<bool> waiting{false};
        /// Written by the waiter before it returns, read after the join.
        Status status = Status::Ok;
        PortPacket packet{};
    };

    auto port_waiter_routine(void *const arg) -> i32 {
        auto &waiter = *static_cast<PortWaiter *>(arg);
        waiter.waiting.store(true, std::memory_order_release);
        waiter.status = waiter.port.dispatcher()->dequeue(INSTANT_INFINITE, &waiter.packet);
        return 0;
    }

    /// The interrupt's packet goes onto the port's interrupt queue under its
    /// own spin lock and wakes the waiter through the port's semaphore: the
    /// path a physical line's handler takes in interrupt context, here from
    /// a thread.
    auto check_packet_wakes_port_waiter(Tally &tally, CpuNum const cpu) -> void {
        KernelHandle<InterruptDispatcher> interrupt;
        auto *const waiter = new (mm::GAF_KERNEL) PortWaiter{};
        auto rights = Rights::None;
        if (waiter == nullptr || !make_interrupt(&interrupt) ||
            PortDispatcher::create(PortCreateOptions::BindToInterrupt, &waiter->port, &rights) !=
                Status::Ok) {
            verdict(tally, false, "make a virtual interrupt and a port to bind it to");
            delete waiter;
            return;
        }
        verdict(tally,
                interrupt.dispatcher()->bind(waiter->port.dispatcher(), PACKET_KEY) == Status::Ok,
                "bind");

        auto thread = spawn_on("interrupt-test-port", port_waiter_routine, waiter, cpu);
        if (!thread) {
            verdict(tally, false, "spawn the port waiter");
            delete waiter;
            return;
        }
        auto const parked = poll_until([waiter, &thread] {
            return waiter->waiting.load(std::memory_order_acquire) && is_parked(thread.get());
        });
        verdict(tally, parked, "a dequeue on an empty port parks");

        // Sent whether or not it parked: either way the packet ends the
        // dequeue.
        verdict(tally, interrupt.dispatcher()->trigger(TRIGGERED_AT) == Status::Ok,
                "trigger the bound interrupt");
        auto const ended = joined(thread);
        verdict(tally, ended, "the packet wakes the parked dequeue");
        if (!ended) {
            log::error("interrupt.test:   leaking the port waiter; its thread may still use it");
            return;
        }
        auto const &packet = waiter->packet;
        verdict(tally,
                waiter->status == Status::Ok && packet.type == PacketType::Interrupt &&
                    packet.key == PACKET_KEY && packet.status == Status::Ok &&
                    packet.interrupt.timestamp == TRIGGERED_AT,
                "the dequeue reads the Interrupt packet, with the bind's key and the time");
        verdict(tally, interrupt.dispatcher()->destroy() == Status::NotFound,
                "a destroy after the packet was read, before its ack, answers NotFound");
        delete waiter;
    }

    /// A trigger with no one parked latches the event's signal for the next
    /// wait. A bind that then sends that interrupt to the port has taken it,
    /// and the latch has to go with it: left standing, the first wait after
    /// an unbind wakes at once, finds the Waiting it set itself, and answers
    /// BadState instead of parking until the next trigger.
    auto check_bind_takes_the_latch(Tally &tally, CpuNum const cpu) -> void {
        KernelHandle<PortDispatcher> port;
        auto rights = Rights::None;
        if (PortDispatcher::create(PortCreateOptions::BindToInterrupt, &port, &rights) !=
            Status::Ok) {
            verdict(tally, false, "make a port to bind to");
            return;
        }
        // Triggered unbound, sent by the bind, read, acked, unbound: Idle,
        // with nothing pending.
        auto const prepare = [&port](InterruptDispatcher &interrupt) {
            auto packet = PortPacket{};
            return interrupt.trigger(TRIGGERED_AT) == Status::Ok &&
                   interrupt.bind(port.dispatcher(), PACKET_KEY) == Status::Ok &&
                   port.dispatcher()->dequeue(0, &packet) == Status::Ok &&
                   interrupt.ack() == Status::Ok &&
                   interrupt.unbind(port.dispatcher()) == Status::Ok;
        };
        Waiter *waiter = nullptr;
        auto thread = park_waiter(tally, cpu, &waiter, prepare);
        if (!thread) {
            return;
        }
        auto &interrupt = *waiter->interrupt.dispatcher();
        verdict(tally, interrupt.trigger(TRIGGERED_AT + 1) == Status::Ok,
                "trigger after the unbind");
        if (!finish_waiter(tally, thread, waiter, "a trigger after an unbind wakes the waiter")) {
            return;
        }
        verdict(tally, waiter->status == Status::Ok && waiter->timestamp == TRIGGERED_AT + 1,
                "the wait after an unbind returns the next trigger's time");
        delete waiter;
    }

    /// Two interrupts and the two threads that take turns on them. On the
    /// heap, and freed only after both threads are reaped.
    struct Turns {
        KernelHandle<InterruptDispatcher> ping;
        KernelHandle<InterruptDispatcher> pong;
        usize rounds = 0;
        std::atomic<u32> ready{0};
        std::atomic<bool> go{false};
        /// Waits and triggers either thread has finished as expected: what
        /// the watcher reads to tell a slow run from a stuck one.
        std::atomic<usize> steps{0};
        /// Threads that have left their rounds, done or given up.
        std::atomic<u32> finished{0};
        /// Waits or triggers that answered something else than expected.
        std::atomic<usize> bad{0};
    };

    /// Both threads arrive, then start together.
    auto start_together(Turns &turns) -> void {
        turns.ready.fetch_add(1, std::memory_order_acq_rel);
        while (!turns.go.load(std::memory_order_acquire)) {
            arch::spin_hint();
        }
    }

    /// Destroys both interrupts, so that a thread parked waiting for the
    /// other's next trigger wakes with Cancelled rather than waiting for ever.
    auto destroy_both(Turns &turns) -> void {
        auto _ = turns.ping.dispatcher()->destroy();
        auto _ = turns.pong.dispatcher()->destroy();
    }

    /// Counts a failure, then ends the other thread's rounds too.
    auto give_up(Turns &turns) -> void {
        turns.bad.fetch_add(1, std::memory_order_relaxed);
        destroy_both(turns);
    }

    /// Records one wait or trigger, |ok| if it answered what was expected;
    /// whether the rounds go on.
    auto step(Turns &turns, bool const ok) -> bool {
        if (!ok) {
            give_up(turns);
            return false;
        }
        turns.steps.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /// Triggers ping with the round's time, then waits for pong to answer
    /// with the same.
    auto ping_rounds(Turns &turns) -> bool {
        for (usize round = 0; round < turns.rounds; ++round) {
            auto const at = static_cast<Instant>(round + 1);
            if (!step(turns, turns.ping.dispatcher()->trigger(at) == Status::Ok)) {
                return false;
            }
            auto got = Instant{-1};
            auto const status = turns.pong.dispatcher()->wait(&got);
            if (!step(turns, status == Status::Ok && got == at)) {
                return false;
            }
        }
        return true;
    }

    /// Waits for ping, then answers on pong with the time it took.
    auto pong_rounds(Turns &turns) -> bool {
        for (usize round = 0; round < turns.rounds; ++round) {
            auto const at = static_cast<Instant>(round + 1);
            auto got = Instant{-1};
            auto const status = turns.ping.dispatcher()->wait(&got);
            if (!step(turns, status == Status::Ok && got == at)) {
                return false;
            }
            if (!step(turns, turns.pong.dispatcher()->trigger(got) == Status::Ok)) {
                return false;
            }
        }
        return true;
    }

    /// Runs one side's rounds and says it has left them. |turns| is not
    /// touched after that: the watcher may stop watching then, and it is
    /// freed only after this thread is reaped.
    auto take_turns(void *const arg, bool (*const rounds)(Turns &)) -> i32 {
        auto &turns = *static_cast<Turns *>(arg);
        start_together(turns);
        auto const ok = rounds(turns);
        turns.finished.fetch_add(1, std::memory_order_release);
        if (!ok) {
            return 1;
        }
        return 0;
    }

    auto pinger_routine(void *const arg) -> i32 {
        return take_turns(arg, ping_rounds);
    }

    auto ponger_routine(void *const arg) -> i32 {
        return take_turns(arg, pong_rounds);
    }

    /// Waits until both threads have left their rounds, for as long as they
    /// keep taking steps. False once no step has been taken for LOST_AFTER:
    /// a waiter left parked by a lost wakeup, or a thread that never ran.
    auto watch_turns(Turns const &turns) -> bool {
        auto seen = turns.steps.load(std::memory_order_relaxed);
        auto last_step = time::SteadyClock::now();
        while (turns.finished.load(std::memory_order_acquire) < 2) {
            auto _ = Thread::Current::sleep_for(WATCH_INTERVAL, task::Interruptible::No);
            auto const now = time::SteadyClock::now();
            if (auto const steps = turns.steps.load(std::memory_order_relaxed); steps != seen) {
                seen = steps;
                last_step = now;
            } else if (now - last_step >= LOST_AFTER) {
                return false;
            }
        }
        return true;
    }

    auto check_turns(Tally &tally, usize const rounds, CpuNum const pinger_cpu,
                     CpuNum const ponger_cpu) -> void {
        auto *const turns = new (mm::GAF_KERNEL) Turns{};
        if (turns == nullptr || !make_interrupt(&turns->ping) || !make_interrupt(&turns->pong)) {
            verdict(tally, false, "make two virtual interrupts");
            delete turns;
            return;
        }
        turns->rounds = rounds;

        auto pinger = spawn_on("interrupt-test-ping", pinger_routine, turns, pinger_cpu);
        auto ponger = spawn_on("interrupt-test-pong", ponger_routine, turns, ponger_cpu);
        if (!pinger || !ponger) {
            verdict(tally, false, "spawn the two threads");
            // The one that did start is waiting for |go|: a destroyed pair
            // ends its first wait or trigger at once.
            give_up(*turns);
            turns->go.store(true, std::memory_order_release);
            for (auto *const thread: {&pinger, &ponger}) {
                if (*thread && !joined(*thread)) {
                    log::error("interrupt.test:   leaking the turns; a thread may still use them");
                    return;
                }
            }
            delete turns;
            return;
        }

        if (!poll_until([turns] { return turns->ready.load(std::memory_order_acquire) == 2; })) {
            log::error("interrupt.test:   the two threads never both started");
        }
        auto const started = time::SteadyClock::now();
        turns->go.store(true, std::memory_order_release);

        auto const kept_going = watch_turns(*turns);
        auto const elapsed = time::SteadyClock::now() - started;
        verdict(tally, kept_going, "no waiter stays parked once its trigger came");
        if (!kept_going) {
            // Whichever thread is parked wakes with Cancelled.
            give_up(*turns);
        }
        // Both, whatever the first answers. A thread left unreaped runs on
        // detached once its handle goes; the turns it may still use are
        // leaked, and logged.
        auto const pinger_joined = joined(pinger);
        auto const ponger_joined = joined(ponger);
        verdict(tally, pinger_joined && ponger_joined, "both threads end");
        if (!pinger_joined || !ponger_joined) {
            log::error("interrupt.test:   leaking the turns; a thread may still use them");
            return;
        }
        auto const bad = turns->bad.load(std::memory_order_relaxed);
        log::info("interrupt.test:   {} round(s) on CPUs {} and {} in {} ms, {} bad", rounds,
                  pinger_cpu, ponger_cpu, duration_cast<Milliseconds>(elapsed).count(), bad);
        verdict(tally, bad == 0, "every wait returns the time of the trigger it answers");
        delete turns;
    }

    auto parse_rounds(int const argc, char **const argv) -> usize {
        if (argc < 2) {
            return DEFAULT_ROUNDS;
        }
        usize rounds = 0;
        for (auto const c: std::string_view(argv[1])) {
            if (c < '0' || c > '9') {
                return 0;
            }
            rounds = rounds * 10 + static_cast<usize>(c - '0');
        }
        return rounds;
    }

    auto interrupt_test_command(int argc, char **argv) -> int {
        auto const rounds = parse_rounds(argc, argv);
        if (rounds == 0) {
            log::error("interrupt.test: usage: interrupt.test [ROUNDS]");
            return 1;
        }

        auto const num_cpus = Cpu::num_online_cpus();
        auto const first_cpu = CpuNum{0};
        auto second_cpu = CpuNum{0};
        if (num_cpus > 1) {
            second_cpu = CpuNum{1};
        } else {
            log::warn("interrupt.test: one CPU online; the threads take turns rather than race");
        }

        Tally tally;
        check_trigger_wakes(tally, second_cpu);
        check_destroy_wakes(tally, second_cpu);
        check_held_waiter(tally, second_cpu);
        check_packet_wakes_port_waiter(tally, second_cpu);
        check_bind_takes_the_latch(tally, second_cpu);
        check_turns(tally, rounds, first_cpu, second_cpu);

        if (tally.failed != 0) {
            log::error("interrupt.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("interrupt.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(InterruptTest, "interrupt.test", ours::interrupt_test_command,
             "interrupt.test [ROUNDS]: an interrupt object's wait and port delivery against "
             "trigger and destroy");
