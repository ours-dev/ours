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
//   - every destroy waits for the line, also one that finds the object
//     destroyed already (.agents/tasks/irq-multi-arch/PLAN.md, step 9),
//     counted on an object of the test's own; and, on three CPUs, with the
//     kernel's removal of the line held in its cancel hook, a destroy does not
//     return until the removal has let the object go: the interleaving whose
//     use after free the rule is there for;
//   - a virtual interrupt's Idle signal, up while the object is owed no ack
//     (.agents/tasks/irq-multi-arch/PLAN.md, step 4): up from the creation,
//     dropped by a trigger, a trigger that folds into a pending interrupt or
//     into one not yet acked leaves it down; raised by the ack of the last
//     interrupt, whether that ack is a wait that finds nothing pending, parked
//     on a second thread, or an interrupt_ack on a port, and by a destroy,
//     which keeps it up through a trigger that is then refused; an observer
//     attached while it is down is told when it goes up; and with triggers on
//     one CPU racing a taker's waits and acks on another, ROUNDS times, it is
//     up again once the taker has parked, as many times as it is asked;
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
#include <ours/cpu_mask.hpp>
#include <ours/interrupt.hpp>
#include <ours/irq/action.hpp>
#include <ours/irq/chip.hpp>
#include <ours/irq/desc_table.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/object/observer.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/object/virtual_interrupt_dispatcher.hpp>
#include <ours/port.hpp>
#include <ours/syscall.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <atomic>
#include <initializer_list>
#include <string_view>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/views/span.hpp>

namespace ours {
namespace {
    using object::InterruptDispatcher;
    using object::KernelHandle;
    using object::PortDispatcher;
    using object::VirtualInterruptDispatcher;
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

    /// A joinable thread running |routine| on |cpu|.
    auto spawn_on(char const *const name, Thread::StartRoutine const routine, void *const arg,
                  CpuNum const cpu) -> Thread * {
        auto *const thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        auto _ = thread->resume();
        return thread;
    }

    /// Whether |thread| ended within LOST_AFTER, reaped if so.
    auto joined(Thread *const thread) -> bool {
        i32 retcode = 0;
        return thread->join(&retcode, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
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
    /// with nothing pending. Null if it did not get there; nothing is left
    /// behind then but what has been leaked and logged.
    template <typename Prepare>
    auto park_waiter(Tally &tally, CpuNum const cpu, Waiter **const out, Prepare const &prepare)
        -> Thread * {
        auto *const waiter = new (mm::GAF_KERNEL) Waiter{};
        if (waiter == nullptr || !make_interrupt(&waiter->interrupt)) {
            verdict(tally, false, "make a virtual interrupt");
            delete waiter;
            return nullptr;
        }
        if (!prepare(*waiter->interrupt.dispatcher())) {
            verdict(tally, false, "bring the interrupt to the state the check starts from");
            delete waiter;
            return nullptr;
        }
        auto *const thread = spawn_on("interrupt-test-waiter", waiter_routine, waiter, cpu);
        if (thread == nullptr) {
            verdict(tally, false, "spawn the waiter");
            delete waiter;
            return nullptr;
        }
        auto const parked = poll_until([waiter, thread] {
            return waiter->waiting.load(std::memory_order_acquire) && is_parked(thread);
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
            return nullptr;
        }
        *out = waiter;
        return thread;
    }

    /// The same, on an interrupt fresh from its creation.
    auto park_waiter(Tally &tally, CpuNum const cpu, Waiter **const out) -> Thread * {
        return park_waiter(tally, cpu, out, [](InterruptDispatcher &) { return true; });
    }

    /// Reaps |thread| and answers whether it did; |waiter| is freed then, and
    /// leaked otherwise.
    auto finish_waiter(Tally &tally, Thread *const thread, Waiter *const waiter,
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
        auto *const thread = park_waiter(tally, cpu, &waiter);
        if (thread == nullptr) {
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
        auto *const thread = park_waiter(tally, cpu, &waiter);
        if (thread == nullptr) {
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

        auto *const thread = spawn_on("interrupt-test-held", held_waiter_routine, waiter, cpu);
        if (thread == nullptr) {
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
        auto const parked = poll_until([waiter, thread] {
            return waiter->second_waiting.load(std::memory_order_acquire) && is_parked(thread);
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

        auto *const thread = spawn_on("interrupt-test-port", port_waiter_routine, waiter, cpu);
        if (thread == nullptr) {
            verdict(tally, false, "spawn the port waiter");
            delete waiter;
            return;
        }
        auto const parked = poll_until([waiter, thread] {
            return waiter->waiting.load(std::memory_order_acquire) && is_parked(thread);
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
        auto *const thread = park_waiter(tally, cpu, &waiter, prepare);
        if (thread == nullptr) {
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

    /// Whether |interrupt| has Idle up.
    auto idle(InterruptDispatcher &interrupt) -> bool {
        return !!(interrupt.signals() & Signals::Idle);
    }

    /// Counts the times its object tells it Idle is up.
    class IdleObserver final: public object::SignalObserver {
      public:
        std::atomic<usize> matched{0};

        auto on_match(Signals) -> void override {
            matched.fetch_add(1, std::memory_order_relaxed);
        }

        auto on_cancel(Signals) -> void override {}
    };

    /// Whether |port| holds an Interrupt packet for |interrupt|'s bind.
    auto read_packet(PortDispatcher &port) -> bool {
        auto packet = PortPacket{};
        return port.dequeue(Instant{0}, &packet) == Status::Ok &&
               packet.type == PacketType::Interrupt && packet.key == PACKET_KEY;
    }

    /// The Idle signal at each step of an interrupt's life that one thread can
    /// reach, then the wait that is the ack, which takes a second.
    auto check_idle_signal(Tally &tally, CpuNum const cpu) -> void {
        KernelHandle<InterruptDispatcher> handle;
        if (!make_interrupt(&handle)) {
            verdict(tally, false, "make a virtual interrupt");
            return;
        }
        auto &interrupt = *handle.dispatcher();
        auto ignored = Instant{0};

        verdict(tally, idle(interrupt), "a new interrupt owes no ack: Idle is up");
        verdict(tally, interrupt.trigger(TRIGGERED_AT) == Status::Ok && !idle(interrupt),
                "a trigger drops Idle");
        verdict(tally, interrupt.trigger(TRIGGERED_AT + 1) == Status::Ok && !idle(interrupt),
                "a trigger that folds into the pending one leaves it down");
        verdict(tally, interrupt.wait(&ignored) == Status::Ok && !idle(interrupt),
                "a wait that takes the interrupt leaves it down: its ack is owed");
        verdict(tally, interrupt.trigger(TRIGGERED_AT + 2) == Status::Ok && !idle(interrupt),
                "a trigger while the last is not acked leaves it down");
        verdict(tally, interrupt.wait(&ignored) == Status::Ok && !idle(interrupt),
                "a wait that acks the last and takes the pending one leaves it down");
        verdict(tally, interrupt.destroy() == Status::Ok && idle(interrupt),
                "a destroy raises Idle");
        verdict(tally, interrupt.trigger(TRIGGERED_AT) == Status::Cancelled && idle(interrupt),
                "and a trigger it refuses leaves it up");

        // The wait that is the ack: the interrupt is taken on this thread, so
        // the waiter's wait finds NeedAck with nothing pending, clears the
        // debt, and parks.
        Waiter *waiter = nullptr;
        auto *const thread = park_waiter(tally, cpu, &waiter, [](InterruptDispatcher &fresh) {
            auto taken = Instant{0};
            return fresh.trigger(TRIGGERED_AT) == Status::Ok && fresh.wait(&taken) == Status::Ok &&
                   !idle(fresh);
        });
        if (thread == nullptr) {
            return;
        }
        auto &second = *waiter->interrupt.dispatcher();
        verdict(tally, idle(second), "a wait that acks the last and parks raises Idle");
        verdict(tally, second.trigger(TRIGGERED_AT + 3) == Status::Ok,
                "trigger the parked waiter's interrupt");
        if (finish_waiter(tally, thread, waiter, "and it wakes the waiter")) {
            verdict(tally, waiter->status == Status::Ok && !idle(second),
                    "the woken wait takes the interrupt, and Idle is down");
            delete waiter;
        }
    }

    /// The same on a port, with an observer told when Idle goes up.
    auto check_idle_signal_on_a_port(Tally &tally) -> void {
        KernelHandle<InterruptDispatcher> handle;
        KernelHandle<PortDispatcher> port;
        auto rights = Rights::None;
        if (!make_interrupt(&handle) || PortDispatcher::create(PortCreateOptions::BindToInterrupt,
                                                               &port, &rights) != Status::Ok) {
            verdict(tally, false, "make a virtual interrupt and a port to bind it to");
            return;
        }
        auto &interrupt = *handle.dispatcher();
        verdict(tally,
                interrupt.bind(port.dispatcher(), PACKET_KEY) == Status::Ok && idle(interrupt),
                "bind: nothing is owed yet, Idle is up");

        IdleObserver observer;
        verdict(tally, interrupt.trigger(TRIGGERED_AT) == Status::Ok && !idle(interrupt),
                "a trigger sends the packet and drops Idle");
        verdict(tally,
                interrupt.attach_observer(&observer, &observer, Signals::Idle) == Status::Ok &&
                    observer.matched.load() == 0,
                "an observer for Idle attached while it is down is not told");
        verdict(tally, interrupt.trigger(TRIGGERED_AT + 1) == Status::Ok && !idle(interrupt),
                "a trigger while the packet is not acked folds, and Idle stays down");
        verdict(tally, read_packet(*port.dispatcher()) && !idle(interrupt),
                "reading the packet does not raise Idle");
        verdict(tally,
                interrupt.ack() == Status::Ok && !idle(interrupt) && observer.matched.load() == 0,
                "an ack that sends the folded interrupt as the next packet leaves it down");
        verdict(tally, read_packet(*port.dispatcher()), "read the next packet");
        verdict(tally, interrupt.ack() == Status::Ok && idle(interrupt),
                "the ack that leaves nothing more to deliver raises Idle");
        verdict(tally, observer.matched.load() == 1, "and the observer is told, once");
        auto remaining = Signals::None;
        auto _ = interrupt.detach_observer(&observer, &remaining);
        verdict(tally, interrupt.destroy() == Status::Ok && idle(interrupt), "destroy");
    }

    /// A thread that waits on |interrupt| over and over, which takes each
    /// interrupt and acks it with the wait after, until the wait answers
    /// Cancelled.
    struct Taker {
        KernelHandle<InterruptDispatcher> interrupt;
        std::atomic<usize> taken{0};
    };

    auto taker_routine(void *const arg) -> i32 {
        auto &taker = *static_cast<Taker *>(arg);
        auto at = Instant{0};
        while (taker.interrupt.dispatcher()->wait(&at) == Status::Ok) {
            taker.taken.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    /// Idle follows the state when triggers and acks race. A driver that does
    /// not wait for Idle between its triggers (an edge pin) fires at one CPU
    /// while a taker on another takes the interrupts and acks them, ROUNDS
    /// times. Whatever the interleaving, once the driver has stopped and the
    /// taker has acked the last interrupt and parked, nothing is owed, and
    /// Idle must be up; and a trigger then has to bring it down and the ack
    /// after it back up, each time. A signal that was dropped by a trigger and
    /// then raised by an ack that landed before the drop would stay down here
    /// for ever.
    auto check_idle_follows_the_state(Tally &tally, usize const rounds, CpuNum const taker_cpu)
        -> void {
        auto *const taker = new (mm::GAF_KERNEL) Taker{};
        if (taker == nullptr || !make_interrupt(&taker->interrupt)) {
            verdict(tally, false, "make a virtual interrupt");
            delete taker;
            return;
        }
        auto &interrupt = *taker->interrupt.dispatcher();
        auto *const thread = spawn_on("interrupt-test-taker", taker_routine, taker, taker_cpu);
        if (thread == nullptr) {
            verdict(tally, false, "spawn the taker");
            delete taker;
            return;
        }

        for (auto round = usize{0}; round < rounds; ++round) {
            if (interrupt.trigger(TRIGGERED_AT + static_cast<Instant>(round)) != Status::Ok) {
                verdict(tally, false, "trigger while the taker takes and acks");
                break;
            }
            // Sometimes let the taker run ahead, sometimes not.
            for (auto spin = usize{0}; spin < (round % 7) * 40; ++spin) {
                arch::spin_hint();
            }
        }
        auto const settled =
            poll_until([thread, &interrupt] { return is_parked(thread) && idle(interrupt); });
        verdict(tally, settled,
                "after racing triggers and acks, once the taker has parked, Idle is up");

        auto again = true;
        for (auto round = usize{0}; round < 3 && again; ++round) {
            again =
                interrupt.trigger(TRIGGERED_AT) == Status::Ok &&
                poll_until([thread, &interrupt] { return is_parked(thread) && idle(interrupt); });
        }
        verdict(tally, again, "and a trigger and the ack after it take it down and up again");

        verdict(tally, interrupt.destroy() == Status::Ok && idle(interrupt),
                "destroy the interrupt");
        if (joined(thread)) {
            delete taker;
        } else {
            verdict(tally, false, "the taker ends once the interrupt is destroyed");
            log::error("interrupt.test:   leaking the taker; its thread may still use it");
        }
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

        auto *const pinger = spawn_on("interrupt-test-ping", pinger_routine, turns, pinger_cpu);
        auto *const ponger = spawn_on("interrupt-test-pong", ponger_routine, turns, ponger_cpu);
        if (pinger == nullptr || ponger == nullptr) {
            verdict(tally, false, "spawn the two threads");
            // The one that did start is waiting for |go|: a destroyed pair
            // ends its first wait or trigger at once.
            give_up(*turns);
            turns->go.store(true, std::memory_order_release);
            for (auto *const thread: {pinger, ponger}) {
                if (thread != nullptr && !joined(thread)) {
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
        // Both, whatever the first answers: a thread left unreaped is leaked
        // with the turns, and logged.
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

    /// A physical kind of interrupt with no line behind it: it counts how many
    /// times a destroy asked it to let its line go, and lets the test play the
    /// kernel taking the line away (line_revoked()).
    ///
    /// With a |table| of the test's own and its |action| on a line of it, the
    /// detach goes to that line, and the action's cancel hook is the object's
    /// line_revoked() followed by a wait for |gate|: the removal held in the
    /// middle of telling the object, which is what the chain case needs.
    class RecordingInterrupt final: public InterruptDispatcher {
      public:
        RecordingInterrupt()
            : InterruptDispatcher(Flags::None)
            , action(&RecordingInterrupt::on_event, this, irq::IrqFlags::None, "interrupt.test",
                     &RecordingInterrupt::on_cancel) {}

        using InterruptDispatcher::line_revoked;

        irq::DescTable *table = nullptr;
        irq::Action action;
        std::atomic<u32> detaches{0};
        /// The hook has told the object, and waits for the gate.
        std::atomic<bool> hooked{false};
        std::atomic<bool> gate{false};
        /// The hook's last act, with release.
        std::atomic<bool> hook_done{false};

      private:
        static auto on_event(VIrqNum, void *) -> irq::IrqReturn {
            return irq::IrqReturn::Handled;
        }

        static auto on_cancel(VIrqNum, void *const context) -> void {
            auto &self = *static_cast<RecordingInterrupt *>(context);
            self.line_revoked();
            self.hooked.store(true, std::memory_order_release);
            auto const give_up = time::SteadyClock::now() + Seconds(5);
            while (!self.gate.load(std::memory_order_acquire) &&
                   time::SteadyClock::now() < give_up) {
                arch::spin_hint();
            }
            self.hook_done.store(true, std::memory_order_release);
        }

        auto mask_interrupt() -> void override {}
        auto unmask_interrupt() -> void override {}
        auto deactivate_interrupt() -> void override {}
        auto unregister_interrupt_handler() -> void override {
            detaches.fetch_add(1, std::memory_order_relaxed);
            if (table != nullptr) {
                auto _ = table->detach(action); // NotFound once the removal has the action off.
            }
        }
    };

    /// The rule the kernel taking a line away rests on: every destroy() waits
    /// for the line, also one that finds the object destroyed already. The
    /// removal destroys the object from the line's side and is still using it
    /// until it has let the line go, so a destroy that returned at once could
    /// let the caller free the object under it. The wait is the object's
    /// detach (unregister_interrupt_handler()), which this counts.
    auto check_destroy_waits_for_the_line(Tally &tally) -> void {
        auto revoked =
            ustl::Arc<RecordingInterrupt>::adopt(new (mm::GAF_KERNEL) RecordingInterrupt());
        auto plain =
            ustl::Arc<RecordingInterrupt>::adopt(new (mm::GAF_KERNEL) RecordingInterrupt());
        if (!revoked || !plain) {
            verdict(tally, false, "allocate the objects");
            return;
        }

        auto const first = plain->destroy();
        verdict(tally, first == Status::Ok && plain->detaches.load() == 1,
                "a destroy detaches once");
        auto const again = plain->destroy();
        verdict(tally, again == Status::Ok && plain->detaches.load() == 2,
                "and a second destroy waits for the line again, though the object is destroyed");

        Instant timestamp = 0;
        revoked->line_revoked();
        verdict(tally, revoked->wait(&timestamp) == Status::Cancelled,
                "an object whose line was taken away is destroyed: a wait answers Cancelled");
        verdict(tally, revoked->detaches.load() == 0,
                "and the revoke detached nothing: the removal has unlinked the action itself");
        auto const destroyed = revoked->destroy();
        verdict(tally, destroyed == Status::Ok && revoked->detaches.load() == 1,
                "its own destroy answers Ok and still waits for the line");
        revoked->line_revoked();
        verdict(tally,
                revoked->wait(&timestamp) == Status::Cancelled && revoked->detaches.load() == 1,
                "a second revoke changes nothing");
    }

    /// What the chain case's two threads need.
    struct Chain {
        irq::DescTable *table = nullptr;
        VIrqNum virq = irq::INVALID_V_IRQ_NUM;
        RecordingInterrupt *object = nullptr;
        std::atomic<bool> destroy_returned{false};
        std::atomic<bool> hook_done_at_return{false};
        std::atomic<Status> destroy_status{Status::BadState};
    };
    Chain CHAIN;

    auto chain_remover(void *) -> i32 {
        auto _ = CHAIN.table->revoke_line(CHAIN.virq);
        return 0;
    }

    auto chain_destroyer(void *) -> i32 {
        auto const status = CHAIN.object->destroy();
        CHAIN.hook_done_at_return.store(CHAIN.object->hook_done.load(std::memory_order_acquire),
                                        std::memory_order_relaxed);
        CHAIN.destroy_status.store(status, std::memory_order_relaxed);
        CHAIN.destroy_returned.store(true, std::memory_order_release);
        return 0;
    }

    /// The use after free this is all for, held still: the removal has told
    /// the object (Destroyed) and is stuck in its hook with the line's lock
    /// held, still to touch the action embedded in the object. The owner
    /// destroys the object meanwhile, on another CPU, and must not get back
    /// until the removal is done: if it did, it could free the object under
    /// the removal. Three CPUs are used: the removal, the destroy, and the
    /// command thread that opens the gate; the first two spin with interrupts
    /// off, so the third cannot share a CPU with either.
    auto check_destroy_waits_for_the_removal(Tally &tally) -> void {
        auto const online = Cpu::online_mask();
        if (online.count() < 3) {
            log::info("interrupt.test: fewer than three CPUs, the destroy against a removal "
                      "held in its hook is skipped");
            return;
        }
        CpuNum cpus[3]{};
        auto found = usize{0};
        online.for_each([&cpus, &found](CpuNum const cpu) {
            if (found < 3) {
                cpus[found] = cpu;
                found += 1;
            }
        });

        auto *const self = Thread::Current::get();
        self->set_cpu_affinity_hard(CpuMask::of(cpus[0]));
        auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);

        auto *const table = new (mm::GAF_KERNEL) irq::DescTable();
        auto *const object = new (mm::GAF_KERNEL) RecordingInterrupt();
        std::array<HIrqNum, 1> const numbers{HIrqNum{9}};
        constexpr irq::Chip NO_OP_CHIP = {.mask = [](irq::Data const &) -> void {},
                                          .issue_eoi = [](irq::Data const &) -> void {},
                                          .name = "interrupt-test"};
        if (table == nullptr || object == nullptr ||
            table->init(ustl::views::Span<HIrqNum const>(numbers), NO_OP_CHIP) != Status::Ok) {
            verdict(tally, false, "make the chain case's line table and object");
            self->set_cpu_affinity_hard(cpu_possible_mask());
            return;
        }
        object->table = table;
        CHAIN.table = table;
        CHAIN.virq = table->virq_of(HIrqNum{9});
        CHAIN.object = object;
        CHAIN.destroy_returned.store(false, std::memory_order_relaxed);
        CHAIN.hook_done_at_return.store(false, std::memory_order_relaxed);
        CHAIN.destroy_status.store(Status::BadState, std::memory_order_relaxed);

        auto const attached = table->attach(CHAIN.virq, object->action) == Status::Ok;
        auto *const remover =
            Thread::spawn("interrupt.test/revoke", chain_remover, nullptr, task::DEFAULT_PRIORITY);
        if (!attached || remover == nullptr) {
            verdict(tally, false, "attach the object and start the removal");
            self->set_cpu_affinity_hard(cpu_possible_mask());
            return;
        }
        remover->set_cpu_affinity_hard(CpuMask::of(cpus[1]));
        auto _ = remover->resume();
        auto const hooked =
            poll_until([object] { return object->hooked.load(std::memory_order_acquire); });

        auto *const destroyer = Thread::spawn("interrupt.test/destroy", chain_destroyer, nullptr,
                                              task::DEFAULT_PRIORITY);
        if (!hooked || destroyer == nullptr) {
            verdict(tally, false, "the removal reaches the object's hook");
            object->gate.store(true, std::memory_order_release);
            self->set_cpu_affinity_hard(cpu_possible_mask());
            return;
        }
        destroyer->set_cpu_affinity_hard(CpuMask::of(cpus[2]));
        auto _ = destroyer->resume();

        // The gate is closed: a destroy that waits for the line cannot have
        // come back, however long it has had.
        auto _ = Thread::Current::sleep_for(Milliseconds(50), task::Interruptible::No);
        auto const returned_early = CHAIN.destroy_returned.load(std::memory_order_acquire);
        object->gate.store(true, std::memory_order_release);

        auto const ended =
            poll_until([] { return CHAIN.destroy_returned.load(std::memory_order_acquire); });
        auto const remover_ended =
            remover->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
            Status::Ok;
        auto const destroyer_ended =
            destroyer->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
            Status::Ok;

        verdict(tally, !returned_early,
                "a destroy of an object whose line is being taken away does not return while the "
                "removal is in its hook");
        verdict(tally,
                ended && CHAIN.destroy_status.load() == Status::Ok &&
                    CHAIN.hook_done_at_return.load(),
                "and returns Ok once the removal is done with the object");
        verdict(tally, remover_ended && destroyer_ended, "both threads end");
        self->set_cpu_affinity_hard(cpu_possible_mask());
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
        check_idle_signal(tally, second_cpu);
        check_idle_signal_on_a_port(tally);
        check_idle_follows_the_state(tally, rounds, second_cpu);
        check_turns(tally, rounds, first_cpu, second_cpu);
        check_destroy_waits_for_the_line(tally);
        check_destroy_waits_for_the_removal(tally);

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
