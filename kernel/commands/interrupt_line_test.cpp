/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `interruptline.test`: interrupt objects on a physical line
// (.agents/tasks/microkernel-interrupts/PLAN.md, step 11). dbg image only: it
// is the one with a kernel shell, and with the assertions this relies on.
//
// The line is ISA 3, COM2's: QEMU has no COM2 with a single -serial, the
// platform programs the line masked, and no kernel driver is on it. Nothing
// raises it, so the test does, in two ways:
//   - fire(): a dispatch of the line from a thread, through
//     irq::handle_irq_for_test(), the flow a real interrupt runs without its
//     end of interrupt, with preemption off as interrupt context has it;
//   - raise(): an IPI to this CPU on the line's vector, which comes in through
//     the interrupt entry as one from the IO-APIC does, end of interrupt
//     through the line's chip included, and so runs the object's handler in
//     interrupt context.
// Neither goes through the IO-APIC, whose mask an IPI goes around and nothing
// reads back. So that a level line stays masked until the ack is not seen
// here (docs/repairs/261005/object.md, item 1); what is seen is that a
// create sets the line's mode, or leaves it, as it was asked to.
//
// The checks:
//   - the timer's line refuses an object, with a mode or without, and keeps
//     its own mode;
//   - an object takes the free line: an interrupt reaches it, and a second
//     create on the line answers AlreadyBound;
//   - an interrupt, dispatched or raised, wakes a parked waiter, whose wait
//     returns a time taken while the interrupt came in;
//   - bound to a port, an interrupt sends one packet, and those before the
//     ack fold into the next, which the ack sends; raised, the same;
//   - a vector no line has, raised by IPI, is ended by the interrupt entry,
//     since no line's flow can end it: the free line's interrupt, raised
//     after it and in a lower priority class, still comes in;
//   - a destroy wakes a parked waiter with Cancelled and lets the line go: a
//     dispatch then finds nobody on it, and a create takes it again; the
//     last handle going does the same, and closing a handle to an object
//     destroyed already leaves the line's new owner alone;
//   - a mode sets how the line signals, and the default mode leaves it so;
//   - two creates on the free line from two CPUs at once, one asking for an
//     edge and the other for a level, round after round: one takes the line,
//     the other answers AlreadyBound, and the line signals as the one that
//     took it asked. Setting the mode and taking the line are one step under
//     the line's lock (L1); as two steps, the loser's mode could land between
//     the winner's, and the winner would hold a line in the loser's mode;
//   - create refuses an option a physical line does not take, a mode that
//     names none, and a number no line has;
//   - a destroy returns while another CPU's interrupts keep reaching the
//     object, round after round. The handler holds the line's lock (L1) and
//     takes the object's (L2); destroy() sets Destroyed under L2, lets it go,
//     and only then takes L1 to detach, so each waits for the other in one
//     order only.
//
// A console command rather than a unit test: the handler's path is the
// interrupt entry, the irq layer and the scheduler's wakeups, on real CPUs.
#include <ours/arch/apic.hpp>
#include <ours/cpu.hpp>
#include <ours/interrupt.hpp>
#include <ours/irq/mod.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/object/interrupt_event_dispatcher.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/platform/isa.hpp>
#include <ours/port.hpp>
#include <ours/syscall.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <atomic>

#include <arch/spin_hint.hpp>
#include <arch/x86/interrupt.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using object::InterruptDispatcher;
    using object::InterruptEventDispatcher;
    using object::KernelHandle;
    using object::PortDispatcher;
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(10);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    /// The key the port checks bind with; any value a packet would not carry
    /// by chance.
    constexpr auto PACKET_KEY = u64{0x1A7E'0000'0000'0011};
    /// A number past every pin of q35's one IO-APIC, so no line has it.
    constexpr auto NO_SUCH_LINE = u32{200};
    constexpr usize TEARDOWN_ROUNDS = 200;
    constexpr usize MODE_RACE_ROUNDS = 200;
    /// How many wrong rounds of the mode race are logged one by one.
    constexpr usize MODE_RACE_LOGGED = 5;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("interruptline.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("interruptline.test: FAIL  {}", name);
    }

    /// Whether |got| is |want|, logging both when it is not.
    auto answered(Tally &tally, Status const got, Status const want, char const *const name)
        -> void {
        if (got != want) {
            log::error("interruptline.test:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        verdict(tally, got == want, name);
    }

    /// The free line, by the number the irq layer names it: q35 has no
    /// override for ISA 3, so it is GSI 3.
    auto free_line() -> VIrqNum {
        return platform::remap_isa_irq(ISA_IRQ_SERIAL2);
    }

    auto timer_line() -> VIrqNum {
        return platform::remap_isa_irq(ISA_IRQ_PIT);
    }

    auto now() -> Instant {
        return time::SteadyClock::now().time_since_epoch().count();
    }

    /// An object on the free line, named by its ISA number as a driver
    /// would, with |mode| for its mode.
    auto make(KernelHandle<InterruptDispatcher> *const out,
              InterruptOptions const mode = InterruptOptions::ModeDefault) -> Status {
        auto rights = Rights::None;
        return InterruptEventDispatcher::create(out, &rights, ISA_IRQ_SERIAL2,
                                                InterruptOptions::RemapIrq | mode);
    }

    /// Plays one interrupt on the free line from this thread: the dispatch a
    /// real one makes, with preemption off as interrupt context has it, so
    /// that a wakeup's reschedule waits until the line's lock is dropped. No
    /// end of interrupt: nothing is in service to end.
    auto fire() -> irq::IrqReturn {
        task::AutoPreemptDisabler const no_preemption;
        return irq::handle_irq_for_test(free_line());
    }

    /// Raises the free line's vector on this CPU by IPI. The platform gave
    /// every line the vector PlatformIrqMin + its number, so this comes in
    /// through the interrupt entry as the line's own interrupt would.
    auto raise() -> void {
        auto const vector = static_cast<arch::IrqVec>(
            static_cast<usize>(arch::IrqVec::PlatformIrqMin) + free_line());
        apic_send_ipi_self(vector, ApicDeliveryMode::Fixed);
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

    /// Whether |thread| ended within LOST_AFTER, reaped if so.
    auto joined(Thread *const thread) -> bool {
        i32 retcode = 0;
        return thread->join(&retcode, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
               Status::Ok;
    }

    /// One thread waiting on an object, and what its wait answered. On the
    /// heap, and freed only once the thread is reaped -- or leaked.
    struct Waiter {
        ustl::Arc<InterruptDispatcher> interrupt;
        std::atomic<bool> waiting{false};
        /// Written by the waiter before it returns, read after the join.
        Status status = Status::Ok;
        Instant timestamp = -1;
    };

    auto waiter_routine(void *const arg) -> i32 {
        auto &waiter = *static_cast<Waiter *>(arg);
        waiter.waiting.store(true, std::memory_order_release);
        waiter.status = waiter.interrupt->wait(&waiter.timestamp);
        return 0;
    }

    /// Starts a thread waiting on |interrupt| and waits until it has parked.
    /// Null if it did not get there; whatever is left then is destroyed or
    /// leaked, and logged.
    auto park_waiter(Tally &tally, ustl::Arc<InterruptDispatcher> interrupt, Waiter **const out)
        -> Thread * {
        auto *const waiter = new (mm::GAF_KERNEL) Waiter{};
        if (waiter == nullptr) {
            verdict(tally, false, "allocate a waiter");
            return nullptr;
        }
        waiter->interrupt = interrupt;
        auto *const thread =
            Thread::spawn("interruptline-waiter", waiter_routine, waiter, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            verdict(tally, false, "spawn a waiter");
            delete waiter;
            return nullptr;
        }
        auto _ = thread->resume();
        auto const parked = poll_until([waiter, thread] {
            return waiter->waiting.load(std::memory_order_acquire) && is_parked(thread);
        });
        if (!parked) {
            verdict(tally, false, "a wait with nothing pending parks");
            // Whatever it is doing, a destroy ends its wait.
            auto _ = interrupt->destroy();
            if (joined(thread)) {
                delete waiter;
            } else {
                log::error("interruptline.test:   leaking a waiter; its thread may still use it");
            }
            return nullptr;
        }
        *out = waiter;
        return thread;
    }

    /// Reaps the waiter's thread and frees the waiter; false, with both
    /// leaked, if the thread never ended.
    auto reap(Thread *const thread, Waiter *const waiter) -> bool {
        if (!joined(thread)) {
            log::error("interruptline.test:   leaking a waiter; its thread may still use it");
            return false;
        }
        delete waiter;
        return true;
    }

    auto check_timer_line(Tally &tally) -> void {
        auto const before = irq::trigger_mode(timer_line());
        KernelHandle<InterruptDispatcher> interrupt;
        auto rights = Rights::None;
        auto const plain = InterruptEventDispatcher::create(&interrupt, &rights, ISA_IRQ_PIT,
                                                            InterruptOptions::RemapIrq);
        auto const level = InterruptEventDispatcher::create(&interrupt, &rights, ISA_IRQ_PIT,
                                                            InterruptOptions::RemapIrq |
                                                                InterruptOptions::ModeLevelHigh);
        auto const after = irq::trigger_mode(timer_line());

        answered(tally, plain, Status::AlreadyBound, "the timer's line refuses an object");
        answered(tally, level, Status::AlreadyBound,
                 "and one that would set its mode, before setting it");
        verdict(tally,
                before.is_ok() && after.is_ok() && *before == irq::TriggerMode::Edge &&
                    *after == irq::TriggerMode::Edge,
                "the timer's line is still edge-triggered after both");
    }

    auto check_take_line(Tally &tally) -> void {
        KernelHandle<InterruptDispatcher> first;
        KernelHandle<InterruptDispatcher> second;
        answered(tally, make(&first), Status::Ok, "an object takes the free line");
        if (!first.dispatcher()) {
            return;
        }
        answered(tally, make(&second), Status::AlreadyBound,
                 "a second object on the line answers AlreadyBound");
        answered(tally, make(&second, InterruptOptions::ModeEdgeHigh), Status::AlreadyBound,
                 "so does one that would set the line's mode");

        auto const from = now();
        auto const result = fire();
        auto const to = now();
        auto at = Instant{-1};
        auto const waited = first.dispatcher()->wait(&at);
        verdict(tally, result == irq::IrqReturn::Handled, "a dispatch on the line is handled");
        verdict(tally, waited == Status::Ok && at >= from && at <= to,
                "by the first object: a wait takes it, with a time from while it came in");
        answered(tally, first.dispatcher()->trigger(1), Status::BadState,
                 "an object on a line cannot be triggered by hand");
    }

    auto check_wakes_waiter(Tally &tally, bool const by_ipi, char const *const name) -> void {
        KernelHandle<InterruptDispatcher> interrupt;
        if (make(&interrupt) != Status::Ok) {
            verdict(tally, false, "take the line for a waiter");
            return;
        }
        Waiter *waiter = nullptr;
        auto *const thread = park_waiter(tally, interrupt.dispatcher(), &waiter);
        if (thread == nullptr) {
            return;
        }
        auto const from = now();
        if (by_ipi) {
            raise();
        } else {
            auto _ = fire();
        }
        auto const ended = joined(thread);
        auto const to = now();
        verdict(tally, ended, name);
        if (!ended) {
            // A destroy ends the wait; then it is reaped or leaked.
            auto _ = interrupt.dispatcher()->destroy();
            auto _ = reap(thread, waiter);
            return;
        }
        verdict(tally,
                waiter->status == Status::Ok && waiter->timestamp >= from &&
                    waiter->timestamp <= to,
                "the woken wait returns a time from while the interrupt came in");
        delete waiter;
    }

    /// Takes one packet off |port| within LOST_AFTER: what a port reader of
    /// an interrupt raised by IPI sees, the IPI being taken a few
    /// instructions after it is sent.
    auto read_within(PortDispatcher &port, PortPacket *const packet) -> bool {
        return poll_until([&port, packet] { return port.dequeue(0, packet) == Status::Ok; });
    }

    auto is_packet(PortPacket const &packet, Instant const from, Instant const to) -> bool {
        return packet.type == PacketType::Interrupt && packet.key == PACKET_KEY &&
               packet.status == Status::Ok && packet.interrupt.timestamp >= from &&
               packet.interrupt.timestamp <= to;
    }

    auto check_port(Tally &tally) -> void {
        KernelHandle<InterruptDispatcher> interrupt;
        KernelHandle<PortDispatcher> port;
        auto rights = Rights::None;
        if (make(&interrupt) != Status::Ok ||
            PortDispatcher::create(PortCreateOptions::BindToInterrupt, &port, &rights) !=
                Status::Ok ||
            interrupt.dispatcher()->bind(port.dispatcher(), PACKET_KEY) != Status::Ok) {
            verdict(tally, false, "take the line and bind it to a port");
            return;
        }
        auto &object = *interrupt.dispatcher();
        auto &queue = *port.dispatcher();

        auto packet = PortPacket{};
        auto const first_from = now();
        auto _ = fire();
        auto const first_to = now();
        verdict(tally,
                queue.dequeue(0, &packet) == Status::Ok && is_packet(packet, first_from, first_to),
                "bound, an interrupt sends a packet with the key and its time");

        auto const fold_from = now();
        auto _ = fire();
        auto const fold_to = now();
        auto _ = fire();
        verdict(tally, queue.dequeue(0, &packet) == Status::TimeOut,
                "two more before the ack send nothing");
        answered(tally, object.ack(), Status::Ok, "the ack");
        verdict(tally,
                queue.dequeue(0, &packet) == Status::Ok && is_packet(packet, fold_from, fold_to),
                "sends them as one packet, with the first one's time");
        answered(tally, object.ack(), Status::Ok, "and the next ack has nothing to send");

        auto const raised_from = now();
        raise();
        auto const read = read_within(queue, &packet);
        auto const raised_to = now();
        verdict(tally, read && is_packet(packet, raised_from, raised_to),
                "an interrupt raised through the interrupt entry sends its packet too");
        answered(tally, object.ack(), Status::Ok, "which is acked");
        verdict(tally, queue.dequeue(0, &packet) == Status::TimeOut, "leaving the port empty");
    }

    // A platform vector, and so in the interrupt entry's platform range,
    // whatever the number of IO-APIC pins.
    static_assert(static_cast<usize>(arch::IrqVec::PlatformIrqMin) + NO_SUCH_LINE <=
                  static_cast<usize>(arch::IrqVec::PlatformIrqMax));

    /// A vector no line has, raised by IPI on this CPU, then the free line's.
    /// No line's flow ends the first, so the interrupt entry has to; left in
    /// service it would hold back on this CPU every vector of its priority
    /// class and below, the free line's among them, and the second would
    /// never come in. Preemption stays off across the two sends, so both
    /// reach the same CPU.
    auto check_vector_without_line(Tally &tally) -> void {
        KernelHandle<InterruptDispatcher> interrupt;
        KernelHandle<PortDispatcher> port;
        auto rights = Rights::None;
        if (make(&interrupt) != Status::Ok ||
            PortDispatcher::create(PortCreateOptions::BindToInterrupt, &port, &rights) !=
                Status::Ok ||
            interrupt.dispatcher()->bind(port.dispatcher(), PACKET_KEY) != Status::Ok) {
            verdict(tally, false,
                    "take the line and bind it to a port, beside a vector no line has");
            return;
        }
        auto const unowned = static_cast<arch::IrqVec>(
            static_cast<usize>(arch::IrqVec::PlatformIrqMin) + NO_SUCH_LINE);

        auto packet = PortPacket{};
        auto const from = now();
        {
            task::AutoPreemptDisabler const same_cpu;
            apic_send_ipi_self(unowned, ApicDeliveryMode::Fixed);
            raise();
        }
        auto const read = read_within(*port.dispatcher(), &packet);
        auto const to = now();
        verdict(tally, irq::spurious_count(NO_SUCH_LINE).is_none(),
                "a vector past every line has no line");
        verdict(tally, read && is_packet(packet, from, to),
                "raised before the line's own, it is ended by the interrupt entry: the line's "
                "interrupt still comes in");
    }

    auto check_destroy(Tally &tally) -> void {
        KernelHandle<InterruptDispatcher> interrupt;
        if (make(&interrupt) != Status::Ok) {
            verdict(tally, false, "take the line for the destroy");
            return;
        }
        Waiter *waiter = nullptr;
        auto *const thread = park_waiter(tally, interrupt.dispatcher(), &waiter);
        if (thread == nullptr) {
            return;
        }
        answered(tally, interrupt.dispatcher()->destroy(), Status::Ok, "a destroy");
        if (!joined(thread)) {
            log::error("interruptline.test:   leaking a waiter; its thread may still use it");
            verdict(tally, false, "wakes the parked waiter");
            return;
        }
        verdict(tally, waiter->status == Status::Cancelled,
                "wakes the parked waiter with Cancelled");
        delete waiter;

        auto const spurious_before = irq::spurious_count(free_line());
        auto const result = fire();
        auto const spurious_after = irq::spurious_count(free_line());
        verdict(tally,
                result == irq::IrqReturn::None && spurious_before.is_some() &&
                    spurious_after.is_some() && *spurious_after == *spurious_before + 1,
                "and lets the line go: a dispatch finds nobody on it");

        // The handle is still open; the line is free all the same.
        KernelHandle<InterruptDispatcher> next;
        answered(tally, make(&next), Status::Ok, "a create takes the line again");
        // Closing the destroyed object's last handle destroys nothing more:
        // the new owner keeps its line.
        interrupt = KernelHandle<InterruptDispatcher>();
        KernelHandle<InterruptDispatcher> third;
        answered(tally, make(&third), Status::AlreadyBound,
                 "closing the destroyed object's handle leaves the new owner the line");

        // The last handle going destroys the object, which lets the line go.
        next = KernelHandle<InterruptDispatcher>();
        answered(tally, make(&third), Status::Ok, "the last handle going lets the line go");
    }

    auto check_modes(Tally &tally) -> void {
        auto mode_after = [](InterruptOptions const mode) -> ustl::Option<irq::TriggerMode> {
            KernelHandle<InterruptDispatcher> interrupt;
            if (make(&interrupt, mode) != Status::Ok) {
                return ustl::NONE;
            }
            auto const read = irq::trigger_mode(free_line());
            if (read.is_err()) {
                return ustl::NONE;
            }
            return *read;
        };
        auto const level = mode_after(InterruptOptions::ModeLevelHigh);
        auto const kept = mode_after(InterruptOptions::ModeDefault);
        // Back to how the platform programmed the line, edge and active high,
        // which is how irq.test expects to find it too.
        auto const edge = mode_after(InterruptOptions::ModeEdgeHigh);

        verdict(tally, level.is_some() && *level == irq::TriggerMode::Level,
                "a level mode makes the line level-triggered");
        verdict(tally, kept.is_some() && *kept == irq::TriggerMode::Level,
                "the default mode leaves it so");
        verdict(tally, edge.is_some() && *edge == irq::TriggerMode::Edge,
                "an edge mode makes it edge-triggered again");
    }

    auto mode_name(irq::TriggerMode const mode) -> char const * {
        if (mode == irq::TriggerMode::Level) {
            return "level";
        }
        return "edge";
    }

    struct ModeRace;

    /// One of the two threads racing to create an object on the free line,
    /// the mode it asks for, and what its create answered this round.
    struct Racer {
        ModeRace *race = nullptr;
        InterruptOptions mode = InterruptOptions::ModeDefault;
        irq::TriggerMode asked = irq::TriggerMode::Edge;
        /// Written by the racer before it counts itself finished, and read
        /// and emptied by the test thread once both have, before the next
        /// round starts.
        Status status = Status::Ok;
        KernelHandle<InterruptDispatcher> interrupt;
    };

    /// The two racers, and the rounds the test thread starts and judges. On
    /// the heap: a racer that never stops still points at it, so on that
    /// path it is leaked.
    struct ModeRace {
        /// The round the test thread has started, counted from 1. Each racer
        /// creates once every time it changes.
        std::atomic<usize> round{0};
        /// How many racers have finished the current round.
        std::atomic<usize> finished{0};
        std::atomic<bool> stop{false};
        std::array<Racer, 2> racers;
    };

    /// Creates once a round, as soon as the round starts. It spins for the
    /// start rather than sleeps, so that the two racers leave it as close
    /// together as their CPUs allow and their creates overlap: the race is
    /// over the few instructions between one create's mode step and its
    /// attach, which a wakeup's latency would step over.
    auto race_loop(void *const arg) -> i32 {
        auto &racer = *static_cast<Racer *>(arg);
        auto &race = *racer.race;
        auto seen = usize{0};
        for (;;) {
            auto round = race.round.load(std::memory_order_acquire);
            while (round == seen) {
                if (race.stop.load(std::memory_order_acquire)) {
                    return 0;
                }
                arch::spin_hint();
                round = race.round.load(std::memory_order_acquire);
            }
            seen = round;
            auto rights = Rights::None;
            racer.status =
                InterruptEventDispatcher::create(&racer.interrupt, &rights, ISA_IRQ_SERIAL2,
                                                 InterruptOptions::RemapIrq | racer.mode);
            race.finished.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    /// Stops the racers that were started and reaps them; false, with
    /// |race| leaked, if one never ended.
    auto stop_racers(ModeRace *const race, std::array<Thread *, 2> const &threads) -> bool {
        race->stop.store(true, std::memory_order_release);
        auto all_joined = true;
        for (auto *const thread: threads) {
            if (thread != nullptr && !joined(thread)) {
                all_joined = false;
            }
        }
        if (!all_joined) {
            log::error("interruptline.test:   a racer did not stop; leaking the race");
            return false;
        }
        delete race;
        return true;
    }

    auto check_mode_race(Tally &tally) -> void {
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            log::warn("interruptline.test:   skipped the mode race: it needs two online CPUs, "
                      "have {}",
                      num_cpus);
            return;
        }
        auto *const race = new (mm::GAF_KERNEL) ModeRace{};
        if (race == nullptr) {
            verdict(tally, false, "allocate the mode race");
            return;
        }
        race->racers[0].race = race;
        race->racers[0].mode = InterruptOptions::ModeEdgeHigh;
        race->racers[0].asked = irq::TriggerMode::Edge;
        race->racers[1].race = race;
        race->racers[1].mode = InterruptOptions::ModeLevelHigh;
        race->racers[1].asked = irq::TriggerMode::Level;

        // Two CPUs besides this one where there are, so that neither racer
        // shares its CPU with the test thread that starts the rounds.
        auto const here = Cpu::id();
        auto cpus = std::array<CpuNum, 2>{static_cast<CpuNum>((here + 1) % num_cpus),
                                          static_cast<CpuNum>((here + 2) % num_cpus)};
        if (num_cpus == 2) {
            cpus[1] = here;
        }
        auto threads = std::array<Thread *, 2>{};
        for (usize i = 0; i < threads.size(); ++i) {
            threads[i] = Thread::spawn("interruptline-race", race_loop, &race->racers[i],
                                       task::DEFAULT_PRIORITY);
            if (threads[i] == nullptr) {
                verdict(tally, false, "spawn the racers");
                auto _ = stop_racers(race, threads);
                return;
            }
            threads[i]->set_cpu_affinity_hard(CpuMask::of(cpus[i]));
            auto _ = threads[i]->resume();
        }

        auto judged = usize{0};
        auto not_one_winner = usize{0};
        auto wrong_mode = usize{0};
        auto wins = std::array<usize, 2>{};
        for (usize round = 1; round <= MODE_RACE_ROUNDS; ++round) {
            race->finished.store(0, std::memory_order_relaxed);
            race->round.store(round, std::memory_order_release);
            if (!poll_until(
                    [race] { return race->finished.load(std::memory_order_acquire) == 2; })) {
                log::error("interruptline.test:   round {} never finished", round);
                break;
            }
            judged += 1;

            auto &first = race->racers[0];
            auto &second = race->racers[1];
            auto const first_won =
                first.status == Status::Ok && second.status == Status::AlreadyBound;
            auto const second_won =
                second.status == Status::Ok && first.status == Status::AlreadyBound;
            if (!first_won && !second_won) {
                not_one_winner += 1;
                if (not_one_winner <= MODE_RACE_LOGGED) {
                    log::error("interruptline.test:   round {}: the {} create answered {}, the {} "
                               "create {}",
                               round, mode_name(first.asked), to_string(first.status),
                               mode_name(second.asked), to_string(second.status));
                }
            } else {
                auto winner = usize{0};
                if (second_won) {
                    winner = 1;
                }
                wins[winner] += 1;
                auto const asked = race->racers[winner].asked;
                auto const now_mode = irq::trigger_mode(free_line());
                if (now_mode.is_err() || *now_mode != asked) {
                    wrong_mode += 1;
                    if (wrong_mode <= MODE_RACE_LOGGED) {
                        auto now_name = "unreadable";
                        if (now_mode.is_ok()) {
                            now_name = mode_name(*now_mode);
                        }
                        log::error("interruptline.test:   round {}: the {} create took the line, "
                                   "which is {}",
                                   round, mode_name(asked), now_name);
                    }
                }
            }
            // The winner's last handle going destroys its object, which lets
            // the line go for the next round; the loser holds nothing.
            first.interrupt = KernelHandle<InterruptDispatcher>();
            second.interrupt = KernelHandle<InterruptDispatcher>();
        }

        if (!stop_racers(race, threads)) {
            verdict(tally, false, "the racers stop");
            return;
        }
        // Back to how the platform programmed the line, edge and active high,
        // which is how irq.test expects to find it.
        auto const edge_mode = [] {
            KernelHandle<InterruptDispatcher> restore;
            return make(&restore, InterruptOptions::ModeEdgeHigh) == Status::Ok;
        }();

        log::info("interruptline.test:   {} rounds of the mode race on CPUs {} and {}: edge took "
                  "the line {} times, level {}; {} left it in the other's mode, {} had no one "
                  "winner",
                  judged, cpus[0], cpus[1], wins[0], wins[1], wrong_mode, not_one_winner);
        verdict(tally, judged == MODE_RACE_ROUNDS, "every round of the mode race finishes");
        verdict(tally, not_one_winner == 0,
                "each round, one create takes the line and the other answers AlreadyBound");
        verdict(tally, wrong_mode == 0,
                "and the line signals as the create that took it asked, never as the other");
        verdict(tally, edge_mode, "the line is put back to edge after the race");
    }

    auto check_refusals(Tally &tally) -> void {
        KernelHandle<InterruptDispatcher> interrupt;
        auto rights = Rights::None;
        answered(tally,
                 InterruptEventDispatcher::create(&interrupt, &rights, ISA_IRQ_SERIAL2,
                                                  InterruptOptions::Virtual),
                 Status::InvArgs, "a physical line refuses Virtual");
        answered(tally, make(&interrupt, static_cast<InterruptOptions>(5U << 1)), Status::InvArgs,
                 "and a mode that names none");
        answered(tally,
                 InterruptEventDispatcher::create(&interrupt, &rights, NO_SUCH_LINE,
                                                  InterruptOptions::RemapIrq),
                 Status::NoResource, "a number no line has answers NoResource, past the remap");
        answered(tally,
                 InterruptEventDispatcher::create(&interrupt, &rights, NO_SUCH_LINE,
                                                  InterruptOptions::ModeEdgeHigh),
                 Status::NoResource, "with a mode as well");
    }

    /// The thread raising the line on another CPU, and the test thread
    /// destroying objects on it. On the heap: a raiser that never stops still
    /// points at it, so on that path it is leaked.
    struct Teardown {
        std::atomic<bool> stop{false};
        std::atomic<usize> raised{0};
    };

    auto raise_loop(void *const arg) -> i32 {
        auto &teardown = *static_cast<Teardown *>(arg);
        while (!teardown.stop.load(std::memory_order_acquire)) {
            raise();
            teardown.raised.fetch_add(1, std::memory_order_relaxed);
            arch::spin_hint();
        }
        return 0;
    }

    auto check_teardown(Tally &tally) -> void {
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            log::warn("interruptline.test:   skipped the teardown race: it needs two online CPUs, "
                      "have {}",
                      num_cpus);
            return;
        }
        auto *const teardown = new (mm::GAF_KERNEL) Teardown{};
        if (teardown == nullptr) {
            verdict(tally, false, "allocate the teardown race");
            return;
        }
        auto *const raiser =
            Thread::spawn("interruptline-raise", raise_loop, teardown, task::DEFAULT_PRIORITY);
        if (raiser == nullptr) {
            verdict(tally, false, "spawn the raiser");
            delete teardown;
            return;
        }
        // Another CPU than this one, so that its interrupts race the destroys
        // rather than wait for them.
        raiser->set_cpu_affinity_hard(CpuMask::of(static_cast<CpuNum>((Cpu::id() + 1) % num_cpus)));
        auto _ = raiser->resume();

        auto made = usize{0};
        auto reached = usize{0};
        for (usize round = 0; round < TEARDOWN_ROUNDS; ++round) {
            KernelHandle<InterruptDispatcher> interrupt;
            if (make(&interrupt) != Status::Ok) {
                continue;
            }
            made += 1;
            // Let the raiser's interrupts reach the object, so the destroy
            // below has a handler to race; the wait takes one, parking until
            // one comes should none have yet.
            auto at = Instant{0};
            auto const raised = teardown->raised.load(std::memory_order_relaxed);
            if (poll_until([&] {
                    return teardown->raised.load(std::memory_order_relaxed) > raised + 1;
                }) &&
                interrupt.dispatcher()->wait(&at) == Status::Ok) {
                reached += 1;
            }
            // The handle goes at the end of the round: destroy(), with the
            // raiser's interrupts still coming in.
        }

        teardown->stop.store(true, std::memory_order_release);
        if (!joined(raiser)) {
            log::error("interruptline.test:   the raiser did not stop; leaking its state");
            verdict(tally, false, "the raiser stops");
            return;
        }
        log::info("interruptline.test:   {} rounds, {} objects reached by {} interrupts",
                  TEARDOWN_ROUNDS, reached, teardown->raised.load(std::memory_order_relaxed));
        delete teardown;
        verdict(tally, made == TEARDOWN_ROUNDS,
                "every round's object takes the line the last one let go");
        verdict(tally, reached == TEARDOWN_ROUNDS,
                "another CPU's interrupts reach every one before it is destroyed");
    }

    auto interruptline_test_command(int, char **) -> int {
        Tally tally;
        check_timer_line(tally);
        check_take_line(tally);
        check_wakes_waiter(tally, false, "a dispatch wakes a parked waiter");
        check_wakes_waiter(tally, true, "an interrupt raised by IPI wakes a parked waiter");
        check_port(tally);
        check_vector_without_line(tally);
        check_destroy(tally);
        check_modes(tally);
        check_mode_race(tally);
        check_refusals(tally);
        check_teardown(tally);

        // Whatever happened above, the line is free again: every object was
        // destroyed with its last handle.
        KernelHandle<InterruptDispatcher> last;
        answered(tally, make(&last), Status::Ok, "the line is free at the end");

        if (tally.failed != 0) {
            log::error("interruptline.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("interruptline.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(InterruptLineTest, "interruptline.test", ours::interruptline_test_command,
             "interrupt objects on ISA 3: create, delivery, ack, destroy, and destroy racing "
             "another CPU's interrupts");
