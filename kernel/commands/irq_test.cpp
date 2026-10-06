/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `irq.test`: the irq layer's registration rules and its detach barrier.
//
//   - A second registration on the PIT's line answers AlreadyBound, shared
//     or not.
//   - A Permanent line refuses every other attach and refuses to let its
//     action go, and its dispatch still reaches the action. This runs on
//     a line descriptor of the test's own, outside the line table, because a
//     Permanent line is bound for good: the PIT's own action is private to
//     time.cpp, and binding a real line would take it from everyone else
//     until the next boot.
//   - An idle line takes and gives back an action many times over; while
//     it holds an exclusive one, another answers AlreadyBound; two shared
//     ones may hold it together, and once both are gone an exclusive one may
//     take it again, which only works if detach clears the Shared the first
//     attach set.
//   - A dispatch answers Handled when an action handled it and None on an
//     empty line, and the empty line counts the interrupt as spurious.
//   - detach() is a barrier: with a second CPU dispatching the line in a
//     loop, every detach returns with no handler in flight, and no handler
//     runs after it.
//   - mask(), unmask() and configure() reach a line's chip and answer for a
//     line that does not exist, and configure() is refused on a line someone
//     is on, the PIT's above all; trigger_mode() reads back what configure()
//     set.
//
// The dispatches are driven from thread context through
// irq::handle_irq_generic(), the function a real interrupt reaches, rather
// than by raising the line: the idle line stays masked at the IO-APIC, and
// nothing on QEMU's q35 raises it. The locking is the same either way; what
// a real interrupt adds -- the vector, the EOI -- is not what is tested.
//
// A console command rather than a unit test because the irq layer has no
// host test target, and the barrier needs two CPUs and the real spin lock.
#include <ours/arch/apic.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/action.hpp>
#include <ours/irq/desc.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/platform/isa.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    /// The PIT's line, by the number time.cpp attaches it with: its GSI, as
    /// every line is named. On q35 the MADT sends ISA 0 to GSI 2.
    auto pit_line() -> VIrqNum {
        return apic_io_isa_to_global(ISA_IRQ_PIT);
    }

    /// COM2's line, ISA 3. QEMU has no COM2 with a single -serial, the
    /// platform programs the line masked, and no kernel driver attaches to
    /// it, so it is free. q35 has no override for it: it is GSI 3.
    auto idle_line() -> VIrqNum {
        return apic_io_isa_to_global(ISA_IRQ_SERIAL2);
    }

    /// The number of the test's own Permanent line, which the line table
    /// does not reach: it only names the line to its actions.
    constexpr auto OWN_LINE = irq::INVALID_V_IRQ_NUM - 1;

    constexpr usize ROUNDS = 1000;
    constexpr usize BARRIER_ROUNDS = 300;

    /// Spin-hint iterations a handler holds the line for in the barrier
    /// check, so a detach often arrives while one is in flight.
    constexpr usize HOLD_SPINS = 200;

    /// How long to watch for a handler running after its detach.
    constexpr usize SETTLE_SPINS = 2000;

    /// Generous for the reason mutex.stress's STUCK_AFTER is: QEMU's TCG.
    constexpr auto LOST_AFTER = Seconds(5);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("irq.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("irq.test: FAIL  {}", name);
    }

    /// What a test action's handler records: its context.
    struct Probe {
        std::atomic<usize> calls{0};
        std::atomic<bool> in_flight{false};
        usize hold_spins = 0;
    };

    auto probe_handler(VIrqNum, void *const context) -> irq::IrqReturn {
        auto &probe = *static_cast<Probe *>(context);
        probe.in_flight.store(true, std::memory_order_relaxed);
        for (usize k = 0; k < probe.hold_spins; ++k) {
            arch::spin_hint();
        }
        probe.calls.fetch_add(1, std::memory_order_relaxed);
        probe.in_flight.store(false, std::memory_order_release);
        return irq::IrqReturn::Handled;
    }

    /// The spurious count of a line the test knows exists.
    auto spurious_on(VIrqNum const line) -> u64 {
        auto const count = irq::spurious_count(line);
        if (count.is_none()) {
            return 0;
        }
        return *count;
    }

    /// An action must be off every line before it is destroyed; its
    /// SafeLink hook traps otherwise. A check that attached by mistake comes
    /// through here before it returns.
    auto drop_if_attached(irq::Action &action) -> void {
        if (action.is_attached()) {
            auto _ = irq::detach(action); // Best effort: the check already failed.
        }
    }

    auto check_pit_line_is_taken(Tally &tally) -> void {
        Probe probe;
        irq::Action exclusive{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};
        irq::Action shared{&probe_handler, &probe, irq::IrqFlags::Shared, "irq-test"};

        auto const exclusive_status = irq::attach(pit_line(), exclusive);
        auto const shared_status = irq::attach(pit_line(), shared);
        drop_if_attached(exclusive);
        drop_if_attached(shared);

        log::info("irq.test:   a second PIT registration answered {}, a shared one {}",
                  to_string(exclusive_status), to_string(shared_status));
        verdict(tally, exclusive_status == Status::AlreadyBound,
                "a second registration on the PIT's line answers AlreadyBound");
        verdict(tally, shared_status == Status::AlreadyBound, "so does a shared one");
    }

    /// The actions of the test's own Permanent line, with the context they
    /// share. On the heap, like the line: whatever is still linked at the end
    /// -- the Permanent action always, anything the line wrongly kept --
    /// must outlive the line, which cannot let it go, and an action's hook
    /// traps if it is destroyed while linked.
    struct PermanentCase {
        Probe probe;
        Probe other_probe;
        irq::Action permanent{&probe_handler, &probe, irq::IrqFlags::Permanent, "irq-test"};
        irq::Action shared{&probe_handler, &other_probe, irq::IrqFlags::Shared, "irq-test"};
        irq::Action exclusive{&probe_handler, &other_probe, irq::IrqFlags::None, "irq-test"};
        irq::Action permanent_shared{&probe_handler, &other_probe,
                                     irq::IrqFlags::Permanent | irq::IrqFlags::Shared, "irq-test"};
    };

    /// On a line descriptor of the test's own, outside the line table, so no
    /// interrupt reaches it. The line and its case are leaked, one pair per
    /// run: the Permanent action can never come off.
    auto check_permanent_line(Tally &tally) -> void {
        auto *const line = irq::Desc::create(OWN_LINE, OWN_LINE, irq::IrqFlags::None, "irq-test");
        auto *const c = new (mm::GAF_KERNEL) PermanentCase{};
        if (line == nullptr || c == nullptr) {
            verdict(tally, false, "allocate the Permanent line");
            return;
        }

        // A Permanent action never joins an occupied line, even one whose
        // occupant shares, with a request that says Shared too.
        auto const occupied_status = line->attach(c->shared);
        auto const join_status = line->attach(c->permanent_shared);
        auto const leave_status = line->detach(c->shared);

        auto const attach_status = line->attach(c->permanent);
        auto const exclusive_status = line->attach(c->exclusive);
        auto const shared_status = line->attach(c->shared);
        auto const detach_status = line->detach(c->permanent);
        auto const result = line->notify();

        verdict(tally,
                occupied_status == Status::Ok && join_status == Status::AlreadyBound &&
                    leave_status == Status::Ok,
                "a Permanent action cannot join an occupied line");
        verdict(tally, attach_status == Status::Ok, "a Permanent action takes an empty line");
        verdict(tally,
                exclusive_status == Status::AlreadyBound && shared_status == Status::AlreadyBound,
                "a Permanent line refuses every other attach");
        verdict(tally, detach_status == Status::AlreadyBound && c->permanent.is_attached(),
                "and refuses to let its action go: attach kept the flag");
        verdict(tally,
                result == irq::IrqReturn::Handled && c->probe.calls.load() == 1 &&
                    c->other_probe.calls.load() == 0,
                "its dispatch, without the lock, reaches the action");
    }

    auto check_idle_line_repeatedly(Tally &tally) -> void {
        Probe probe;
        Probe other_probe;
        irq::Action action{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};
        irq::Action other{&probe_handler, &other_probe, irq::IrqFlags::None, "irq-test"};

        auto attach_failures = usize{0};
        auto second_attach_failures = usize{0};
        auto dispatch_failures = usize{0};
        auto detach_failures = usize{0};
        auto redetach_failures = usize{0};
        for (usize round = 0; round < ROUNDS; ++round) {
            if (irq::attach(idle_line(), action) != Status::Ok || !action.is_attached() ||
                action.virqnum() != idle_line()) {
                attach_failures += 1;
                drop_if_attached(action);
                continue;
            }
            if (irq::attach(idle_line(), other) != Status::AlreadyBound) {
                second_attach_failures += 1;
                drop_if_attached(other);
            }

            auto const calls_before = probe.calls.load(std::memory_order_relaxed);
            if (irq::handle_irq_generic(idle_line()) != irq::IrqReturn::Handled ||
                probe.calls.load(std::memory_order_relaxed) != calls_before + 1) {
                dispatch_failures += 1;
            }

            if (irq::detach(action) != Status::Ok || action.is_attached()) {
                detach_failures += 1;
                drop_if_attached(action);
            }
            if (irq::detach(action) != Status::NotFound) {
                redetach_failures += 1;
            }
        }

        log::info("irq.test:   {} rounds on line {}", ROUNDS, idle_line());
        verdict(tally, attach_failures == 0, "an idle line takes an action every round");
        verdict(tally, second_attach_failures == 0,
                "a second exclusive action answers AlreadyBound every round");
        verdict(tally, dispatch_failures == 0 && other_probe.calls.load() == 0,
                "a dispatch reaches the attached action, and only it");
        verdict(tally, detach_failures == 0, "detach takes the action off every round");
        verdict(tally, redetach_failures == 0, "a second detach answers NotFound");
    }

    auto check_shared_then_exclusive(Tally &tally) -> void {
        Probe first_probe;
        Probe second_probe;
        irq::Action first{&probe_handler, &first_probe, irq::IrqFlags::Shared, "irq-test"};
        irq::Action second{&probe_handler, &second_probe, irq::IrqFlags::Shared, "irq-test"};
        irq::Action exclusive{&probe_handler, &first_probe, irq::IrqFlags::None, "irq-test"};

        auto const first_status = irq::attach(idle_line(), first);
        auto const second_status = irq::attach(idle_line(), second);
        auto const exclusive_status = irq::attach(idle_line(), exclusive);
        drop_if_attached(exclusive);
        auto const result = irq::handle_irq_generic(idle_line());
        drop_if_attached(first);
        drop_if_attached(second);

        // The line is empty again, so it no longer says Shared.
        auto const after_status = irq::attach(idle_line(), exclusive);
        drop_if_attached(exclusive);

        verdict(tally, first_status == Status::Ok && second_status == Status::Ok,
                "two shared actions hold one line");
        verdict(tally, exclusive_status == Status::AlreadyBound,
                "an exclusive one cannot join them");
        verdict(tally,
                result == irq::IrqReturn::Handled && first_probe.calls.load() == 1 &&
                    second_probe.calls.load() == 1,
                "a dispatch on a shared line calls every action");
        verdict(tally, after_status == Status::Ok,
                "once they are gone an exclusive one takes the line: detach cleared Shared");
    }

    auto check_spurious(Tally &tally) -> void {
        auto const before = irq::spurious_count(idle_line());
        auto const result = irq::handle_irq_generic(idle_line());
        auto const after = irq::spurious_count(idle_line());

        verdict(tally, result == irq::IrqReturn::None,
                "a dispatch on an empty line answers None, not Handled");
        verdict(tally, before.is_some() && after.is_some() && *after == *before + 1,
                "and the line counts it as spurious");
    }

    /// What the irq layer can show of the chip binding: the calls reach a
    /// line's chip and answer Ok, a line with no chip answers NotSupported,
    /// and configure() is refused on an occupied line. Of the IO-APIC entry
    /// only the trigger mode is read back, through trigger_mode(), the one
    /// field the irq layer has a call for; that the chip really masks and
    /// unmasks is shown by the boot, whose tick arrives only once time.cpp's
    /// unmask has gone through the chip.
    auto check_line_control(Tally &tally) -> void {
        Probe probe;
        irq::Action action{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};

        auto const no_line_status = irq::mask(OWN_LINE);
        auto const invalid_status = irq::unmask(irq::INVALID_V_IRQ_NUM);
        auto const pit_status =
            irq::configure(pit_line(), irq::TriggerMode::Edge, irq::Polarity::ActiveHigh);

        // The idle line is masked, as the platform programmed it, and is left
        // masked: nothing raises it on q35, and nothing is attached to it.
        auto const mask_status = irq::mask(idle_line());
        auto const unmask_status = irq::unmask(idle_line());
        auto const remask_status = irq::mask(idle_line());

        auto const attach_status = irq::attach(idle_line(), action);
        auto const occupied_status =
            irq::configure(idle_line(), irq::TriggerMode::Level, irq::Polarity::ActiveLow);
        drop_if_attached(action);
        // Level and active low, then back to ISA's edge and active high,
        // where q35's MADT, with no override for ISA 3, leaves the line. The
        // controller is asked each time how the line now signals.
        auto const level_status =
            irq::configure(idle_line(), irq::TriggerMode::Level, irq::Polarity::ActiveLow);
        auto const level_read = irq::trigger_mode(idle_line());
        auto const edge_status =
            irq::configure(idle_line(), irq::TriggerMode::Edge, irq::Polarity::ActiveHigh);
        auto const edge_read = irq::trigger_mode(idle_line());
        auto const pit_read = irq::trigger_mode(pit_line());
        auto const no_line_read = irq::trigger_mode(OWN_LINE);

        // A line of the test's own, outside the table, which no chip drives.
        auto *const line = irq::Desc::create(OWN_LINE, OWN_LINE, irq::IrqFlags::None, "irq-test");
        if (line == nullptr) {
            verdict(tally, false, "allocate the line with no chip");
            return;
        }
        auto const chipless_mask = line->mask();
        auto const chipless_configure =
            line->configure(irq::TriggerMode::Edge, irq::Polarity::ActiveHigh);
        auto const chipless_read = line->trigger_mode();
        delete line;

        log::info("irq.test:   configure on the PIT's line answered {}, on an occupied line {}",
                  to_string(pit_status), to_string(occupied_status));
        verdict(tally, no_line_status == Status::NoResource && invalid_status == Status::InvArgs,
                "mask and unmask answer NoResource for no line, InvArgs for the invalid number");
        verdict(tally,
                mask_status == Status::Ok && unmask_status == Status::Ok &&
                    remask_status == Status::Ok,
                "mask and unmask go through the idle line's chip");
        verdict(tally, pit_status == Status::AlreadyBound,
                "configure is refused on the PIT's Permanent line");
        verdict(tally, attach_status == Status::Ok && occupied_status == Status::AlreadyBound,
                "configure is refused on a line someone is on");
        verdict(tally, level_status == Status::Ok && edge_status == Status::Ok,
                "configure goes through an empty line's chip, there and back");
        verdict(tally,
                level_read.is_ok() && *level_read == irq::TriggerMode::Level && edge_read.is_ok() &&
                    *edge_read == irq::TriggerMode::Edge,
                "trigger_mode reads back what configure set, each time");
        verdict(tally, pit_read.is_ok() && *pit_read == irq::TriggerMode::Edge,
                "and reads the PIT's line as the platform programmed it: edge");
        verdict(tally, no_line_read.is_err() && no_line_read.unwrap_err() == Status::NoResource,
                "trigger_mode answers NoResource for no line");
        verdict(tally,
                chipless_mask == Status::NotSupported &&
                    chipless_configure == Status::NotSupported && chipless_read.is_err() &&
                    chipless_read.unwrap_err() == Status::NotSupported,
                "a line with no chip answers NotSupported");
    }

    /// Shared between the command and its dispatcher thread. On the heap: a
    /// dispatcher that never stops still points at it after the command has
    /// given up, so on that path it is leaked rather than popped.
    struct BarrierCase {
        std::atomic<bool> stop{false};
        std::atomic<bool> done{false};
        std::atomic<usize> handled{0};
        std::atomic<usize> unhandled{0};
    };

    /// Plays the interrupt: dispatches the idle line in a loop on another
    /// CPU, as fast as it can, until told to stop.
    auto dispatch_loop(void *const arg) -> i32 {
        auto &c = *static_cast<BarrierCase *>(arg);
        while (!c.stop.load(std::memory_order_acquire)) {
            if (irq::handle_irq_generic(idle_line()) == irq::IrqReturn::Handled) {
                c.handled.fetch_add(1, std::memory_order_relaxed);
            } else {
                c.unhandled.fetch_add(1, std::memory_order_relaxed);
            }
            arch::spin_hint();
        }
        // Last touch of |c|: once the command sees this, it frees it.
        c.done.store(true, std::memory_order_release);
        return 0;
    }

    /// Spins until |condition| holds or LOST_AFTER has passed.
    template <typename Condition>
    auto spin_until(Condition const &condition) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!condition()) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            arch::spin_hint();
        }
        return true;
    }

    auto check_detach_barrier(Tally &tally) -> void {
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            log::warn("irq.test:   skipped the barrier check: it needs two online CPUs, have {}",
                      num_cpus);
            return;
        }

        auto *const c = new (mm::GAF_KERNEL) BarrierCase{};
        if (c == nullptr) {
            verdict(tally, false, "allocate the barrier case");
            return;
        }

        Probe probe;
        probe.hold_spins = HOLD_SPINS;
        irq::Action action{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};
        auto const spurious_before = spurious_on(idle_line());

        auto *const dispatcher =
            task::Thread::spawn("irq-test-dispatch", dispatch_loop, c, task::DEFAULT_PRIORITY);
        if (dispatcher == nullptr) {
            verdict(tally, false, "spawn the dispatcher");
            delete c;
            return;
        }
        // Another CPU than this one, so the dispatches race the detaches
        // rather than take turns with them.
        auto const cpu = static_cast<CpuNum>((Cpu::id() + 1) % num_cpus);
        dispatcher->set_cpu_affinity_hard(CpuMask::of(cpu));
        // detach() and resume() separately: detach_and_resume() is declared
        // but has no definition yet.
        auto _ = dispatcher->detach();
        auto _ = dispatcher->resume();

        auto unreached = usize{0};
        auto in_flight_after_detach = usize{0};
        auto ran_after_detach = usize{0};
        auto detach_failures = usize{0};
        for (usize round = 0; round < BARRIER_ROUNDS; ++round) {
            auto const calls_before = probe.calls.load(std::memory_order_relaxed);
            if (irq::attach(idle_line(), action) != Status::Ok) {
                detach_failures += 1;
                continue;
            }
            // Let a dispatch reach the action, so the detach below has a
            // handler to race.
            if (!spin_until(
                    [&] { return probe.calls.load(std::memory_order_relaxed) != calls_before; })) {
                unreached += 1;
            }

            if (irq::detach(action) != Status::Ok) {
                detach_failures += 1;
                drop_if_attached(action);
            }
            // The handler clears in_flight before the line's lock is dropped,
            // and detach took that lock after it: so it must read false.
            if (probe.in_flight.load(std::memory_order_acquire)) {
                in_flight_after_detach += 1;
            }
            auto const calls_at_detach = probe.calls.load(std::memory_order_relaxed);
            for (usize k = 0; k < SETTLE_SPINS; ++k) {
                arch::spin_hint();
            }
            if (probe.calls.load(std::memory_order_relaxed) != calls_at_detach) {
                ran_after_detach += 1;
            }
        }

        c->stop.store(true, std::memory_order_release);
        auto const stopped = spin_until([c] { return c->done.load(std::memory_order_acquire); });
        if (!stopped) {
            log::error("irq.test:   the dispatcher did not stop; leaking its state");
            verdict(tally, false, "the dispatcher stops");
            return;
        }

        auto const handled = c->handled.load(std::memory_order_relaxed);
        auto const unhandled = c->unhandled.load(std::memory_order_relaxed);
        auto const spurious = spurious_on(idle_line()) - spurious_before;
        delete c;

        log::info("irq.test:   {} rounds, {} dispatches handled, {} found the line empty",
                  BARRIER_ROUNDS, handled, unhandled);
        verdict(tally, detach_failures == 0, "attach and detach succeed under dispatch");
        verdict(tally, unreached == 0, "a dispatch on another CPU reaches the action");
        verdict(tally, in_flight_after_detach == 0, "no handler is in flight when detach returns");
        verdict(tally, ran_after_detach == 0, "no handler runs after detach returns");
        verdict(tally, handled == probe.calls.load(std::memory_order_relaxed),
                "every handled dispatch ran the handler once");
        verdict(tally, spurious == unhandled, "every unhandled dispatch counted as spurious");
    }

    auto irq_test_command(int, char **) -> int {
        Tally tally;

        check_pit_line_is_taken(tally);
        check_permanent_line(tally);
        check_idle_line_repeatedly(tally);
        check_shared_then_exclusive(tally);
        check_spurious(tally);
        check_line_control(tally);
        check_detach_barrier(tally);

        if (tally.failed != 0) {
            log::error("irq.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("irq.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(IrqTest, "irq.test", ours::irq_test_command,
             "attach, detach, mask and configure an idle line, and check the PIT's line is "
             "Permanent");
