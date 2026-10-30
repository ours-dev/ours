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
//   - A line's flow ends each interrupt through the line's chip exactly
//     once, after its actions, whether they handled it or not and whether
//     the line is Permanent or not; a played dispatch ends none; and the
//     line table refuses a chip that cannot end an interrupt. These run on
//     lines of the test's own, bound to a chip that only counts.
//   - A line's flow follows how its pin signals: it is chosen as the line is
//     bound to its chip, and again by configure() and by attach() with a
//     mode, set or read; a refused configure leaves it. An interrupt on a
//     line nobody is on masks the line before it ends the interrupt, on
//     either flow, and a played one masks it too; a line with an action is
//     neither masked nor unmasked by its flow; and the line table refuses a
//     chip that cannot mask. These run on lines of the test's own, bound to
//     a chip over a pin the test keeps in memory.
//   - detach() is a barrier: with a second CPU dispatching the line in a
//     loop, every detach returns with no handler in flight, and no handler
//     runs after it.
//   - mask(), unmask() and configure() reach a line's chip and answer for a
//     line that does not exist, and configure() is refused on a line someone
//     is on, the PIT's above all; trigger_mode() reads back what configure()
//     set, and is_masked_for_test() what mask() and unmask() left in the
//     idle line's IO-APIC entry.
//   - mask() and unmask() take the line's lock: with a second CPU's dispatch
//     of the idle line holding it, each returns only once the handler has.
//   - Oneshot, on the idle line set level and read back from its IO-APIC
//     entry: an interrupt whose action answers WakeThread masks the line
//     before the action and leaves it held, masked through an unmask and
//     through an interrupt the action handles itself, until
//     finalize_oneshot() unmasks it; WakeThread
//     is not counted as spurious; detach clears held, so a finalize after it
//     does nothing and the next owner's interrupt unmasks the line, and the
//     last detach masks the line it lets go. On lines of the test's own,
//     where the end of interrupt is counted, the interrupt a thread was woken
//     for is ended with the pin masked, an edge line ignores Oneshot, and a
//     finalize on a line that is not Oneshot does nothing. Oneshot is
//     refused together with Permanent or Shared, and on a line whose chip
//     cannot unmask.
//
// The dispatches are driven from thread context through
// irq::handle_irq_for_test(), the flow a real interrupt runs without its end
// of interrupt, rather than by raising the line: the idle line stays masked
// at the IO-APIC, and nothing on QEMU's q35 raises it. The locking is the
// same either way; what a real interrupt adds -- the vector, the end of
// interrupt at the local APIC -- is not what is tested here. That a real
// interrupt is ended shows elsewhere: interruptline.test raises its line by
// IPI over and over, each delivery waiting on the end of the one before,
// and on rel the keyboard's keys keep coming.
//
// A console command rather than a unit test because the irq layer has no
// host test target, and the barrier needs two CPUs and the real spin lock.
#include <ours/arch/x86/apic.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/action.hpp>
#include <ours/irq/desc.hpp>
#include <ours/irq/desc_table.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/platform/isa.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
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

    /// How long a handler holds the idle line in the line lock check: long
    /// enough that the command, on another CPU, sees it start and makes its
    /// call well before it ends, even under TCG.
    constexpr auto HOLD_FOR = Milliseconds(20);

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

    // The pin of the test's own, kept in memory, behind the chips below: how
    // many interrupts it was asked to end, and how many of those while it
    // was unmasked, how many times it was masked, whether it is masked now,
    // and how it signals. One pin behind every line of the test's own: a
    // check uses one line at a time and sets the pin before each use. Atomic
    // because the chips' operations run in whatever context the line's flow
    // does; the test reads them on the CPU that dispatched.
    // Namespace-scope objects are UPPER_CASE throughout this tree.
    // NOLINTBEGIN(readability-identifier-naming)
    constinit std::atomic<usize> EOIS_SENT{0};
    constinit std::atomic<usize> EOIS_UNMASKED{0};
    constinit std::atomic<usize> MASKS_SENT{0};
    constinit std::atomic<bool> PIN_MASKED{false};
    constinit std::atomic<irq::TriggerMode> PIN_TRIGGER{irq::TriggerMode::Edge};
    // NOLINTEND(readability-identifier-naming)

    /// Also notes an end of interrupt that found the pin unmasked: on a real
    /// level pin still asserted, that is the moment it delivers again.
    auto count_eoi(irq::Data const &) -> void {
        EOIS_SENT.fetch_add(1, std::memory_order_relaxed);
        if (!PIN_MASKED.load(std::memory_order_relaxed)) {
            EOIS_UNMASKED.fetch_add(1, std::memory_order_relaxed);
        }
    }

    auto count_mask(irq::Data const &) -> void {
        MASKS_SENT.fetch_add(1, std::memory_order_relaxed);
        PIN_MASKED.store(true, std::memory_order_relaxed);
    }

    auto pin_unmask(irq::Data const &) -> void {
        PIN_MASKED.store(false, std::memory_order_relaxed);
    }

    /// Leaves the pin masked, as a chip's configure must.
    auto pin_configure(irq::Data const &, irq::TriggerMode const trigger, irq::Polarity) -> Status {
        PIN_TRIGGER.store(trigger, std::memory_order_relaxed);
        PIN_MASKED.store(true, std::memory_order_relaxed);
        return Status::Ok;
    }

    auto pin_trigger_mode(irq::Data const &) -> irq::TriggerMode {
        return PIN_TRIGGER.load(std::memory_order_relaxed);
    }

    /// A chip that drives no real pin and only counts the interrupts it is
    /// asked to end and the masks, for lines of the test's own. The IO-APIC
    /// chip's end of interrupt is the local APIC's, which a thread must not
    /// send. It cannot say how its pin signals, so its lines run the edge
    /// flow.
    constexpr irq::Chip COUNTING_CHIP = {
        .mask = count_mask,
        .issue_eoi = count_eoi,
        .name = "irq-test",
    };

    /// The counting chip, with a pin's mode and mask bit as well: its lines
    /// take their flow from PIN_TRIGGER, and configure() sets it.
    constexpr irq::Chip PIN_CHIP = {
        .mask = count_mask,
        .unmask = pin_unmask,
        .configure = pin_configure,
        .trigger_mode = pin_trigger_mode,
        .issue_eoi = count_eoi,
        .name = "irq-test",
    };

    /// A chip that cannot end an interrupt.
    constexpr irq::Chip NO_EOI_CHIP = {
        .mask = count_mask,
        .name = "irq-test",
    };

    /// A chip that cannot mask.
    constexpr irq::Chip NO_MASK_CHIP = {
        .issue_eoi = count_eoi,
        .name = "irq-test",
    };

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
        irq::Action permanent_oneshot{&probe_handler, &other_probe,
                                      irq::IrqFlags::Permanent | irq::IrqFlags::Oneshot,
                                      "irq-test"};
    };

    /// On a line descriptor of the test's own, outside the line table, so no
    /// interrupt reaches it, bound to the counting chip. The line and its
    /// case are leaked, one pair per run: the Permanent action can never come
    /// off.
    auto check_permanent_line(Tally &tally) -> void {
        auto *const line = irq::Desc::create(OWN_LINE, OWN_LINE, irq::IrqFlags::None, "irq-test");
        auto *const c = new (mm::GAF_KERNEL) PermanentCase{};
        if (line == nullptr || c == nullptr) {
            verdict(tally, false, "allocate the Permanent line");
            return;
        }
        auto const bound = line->init(COUNTING_CHIP);

        // A Permanent action never joins an occupied line, even one whose
        // occupant shares, with a request that says Shared too.
        auto const occupied_status = line->attach(c->shared);
        auto const join_status = line->attach(c->permanent_shared);
        auto const leave_status = line->detach(c->shared);
        // Nor does one that says Oneshot take even an empty line: its flow
        // would run without the lock that held is kept under.
        auto const oneshot_status = line->attach(c->permanent_oneshot);

        auto const attach_status = line->attach(c->permanent);
        auto const exclusive_status = line->attach(c->exclusive);
        auto const shared_status = line->attach(c->shared);
        auto const detach_status = line->detach(c->permanent);
        auto const eois_before = EOIS_SENT.load(std::memory_order_relaxed);
        auto const result = line->handle_irq(irq::Eoi::Send);
        auto const eois = EOIS_SENT.load(std::memory_order_relaxed) - eois_before;

        verdict(tally,
                occupied_status == Status::Ok && join_status == Status::AlreadyBound &&
                    leave_status == Status::Ok,
                "a Permanent action cannot join an occupied line");
        verdict(tally, oneshot_status == Status::InvArgs && !c->permanent_oneshot.is_attached(),
                "a Permanent action that says Oneshot is refused");
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
        verdict(tally, bound == Status::Ok && eois == 1,
                "and ends the interrupt through the line's chip, once");
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
            if (irq::handle_irq_for_test(idle_line()) != irq::IrqReturn::Handled ||
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
        auto const result = irq::handle_irq_for_test(idle_line());
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
        auto const result = irq::handle_irq_for_test(idle_line());
        auto const after = irq::spurious_count(idle_line());

        verdict(tally, result == irq::IrqReturn::None,
                "a dispatch on an empty line answers None, not Handled");
        verdict(tally, before.is_some() && after.is_some() && *after == *before + 1,
                "and the line counts it as spurious");
    }

    /// On a line of the test's own, outside the table, bound to the counting
    /// chip, so the ends of interrupt are counted and no controller sees one.
    auto check_end_of_interrupt(Tally &tally) -> void {
        auto *const line = irq::Desc::create(OWN_LINE, OWN_LINE, irq::IrqFlags::None, "irq-test");
        if (line == nullptr) {
            verdict(tally, false, "allocate the counting line");
            return;
        }
        Probe probe;
        irq::Action action{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};
        auto const bound = line->init(COUNTING_CHIP);
        auto const attached = line->attach(action);

        auto const before = EOIS_SENT.load(std::memory_order_relaxed);
        auto const handled = line->handle_irq(irq::Eoi::Send);
        auto const after_handled = EOIS_SENT.load(std::memory_order_relaxed);
        auto const played = line->handle_irq(irq::Eoi::Skip);
        auto const after_played = EOIS_SENT.load(std::memory_order_relaxed);
        // The line is not in the table, so irq::detach() would not find it.
        // Taking an action off a line that is not Permanent cannot fail; if
        // it did, the action's hook would trap as it goes out of scope.
        auto const detached = line->detach(action);
        auto const unhandled = line->handle_irq(irq::Eoi::Send);
        auto const after_unhandled = EOIS_SENT.load(std::memory_order_relaxed);
        delete line;

        // A table of the test's own. It refuses the chip before it makes a
        // line, so it holds nothing to free.
        std::array<HIrqNum, 1> const numbers{idle_line()};
        irq::DescTable table;
        auto const refused = table.init(ustl::views::Span(numbers), NO_EOI_CHIP);

        verdict(tally, bound == Status::Ok && attached == Status::Ok && detached == Status::Ok,
                "bind the counting chip, take the line and give it back");
        verdict(tally, handled == irq::IrqReturn::Handled && after_handled == before + 1,
                "a dispatch ends its interrupt through the line's chip, once");
        verdict(tally,
                played == irq::IrqReturn::Handled && after_played == after_handled &&
                    probe.calls.load() == 2,
                "a played dispatch runs the actions and ends nothing");
        verdict(tally, unhandled == irq::IrqReturn::None && after_unhandled == after_played + 1,
                "an interrupt no action handled is ended all the same");
        verdict(tally, refused == Status::InvArgs,
                "the line table refuses a chip that cannot end an interrupt");
    }

    /// A line of the test's own, outside the table, bound to `chip`; null if
    /// it cannot be made. The caller deletes it, with nobody on it.
    auto own_line(irq::Chip const &chip) -> irq::Desc * {
        auto *const line = irq::Desc::create(OWN_LINE, OWN_LINE, irq::IrqFlags::None, "irq-test");
        if (line == nullptr) {
            return nullptr;
        }
        if (line->init(chip) != Status::Ok) {
            delete line;
            return nullptr;
        }
        return line;
    }

    /// drop_if_attached() for a line of the test's own, which irq::detach()
    /// cannot find: it looks in the table.
    auto drop_from(irq::Desc &line, irq::Action &action) -> void {
        if (action.is_attached()) {
            auto _ = line.detach(action); // Best effort: the check already failed.
        }
    }

    /// Which flow a line runs, for the log.
    auto flow_name(irq::Flow const flow) -> char const * {
        if (flow == irq::Flow::FastEoi) {
            return "fasteoi";
        }
        return "edge";
    }

    /// How a line's flow follows its pin's mode: as it is bound, by
    /// configure() and by attach() with a mode.
    auto check_flow_selection(Tally &tally) -> void {
        PIN_TRIGGER.store(irq::TriggerMode::Level, std::memory_order_relaxed);
        auto *const level_line = own_line(PIN_CHIP);
        PIN_TRIGGER.store(irq::TriggerMode::Edge, std::memory_order_relaxed);
        auto *const line = own_line(PIN_CHIP);
        auto *const modeless_line = own_line(COUNTING_CHIP);
        if (level_line == nullptr || line == nullptr || modeless_line == nullptr) {
            delete level_line;
            delete line;
            delete modeless_line;
            verdict(tally, false, "make the lines for flow selection");
            return;
        }
        auto const bound_level = level_line->flow();
        auto const bound_edge = line->flow();
        auto const bound_modeless = modeless_line->flow();
        delete level_line;
        delete modeless_line;

        auto const to_level = line->configure(irq::TriggerMode::Level, irq::Polarity::ActiveLow);
        auto const after_level = line->flow();
        auto const to_edge = line->configure(irq::TriggerMode::Edge, irq::Polarity::ActiveHigh);
        auto const after_edge = line->flow();

        // A configure refused because someone is on the line changes neither
        // the pin nor the flow.
        Probe probe;
        irq::Action action{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};
        auto const occupied = line->attach(action);
        auto const refused = line->configure(irq::TriggerMode::Level, irq::Polarity::ActiveLow);
        auto const after_refused = line->flow();
        auto const left = line->detach(action);

        auto const level_mode = irq::LineMode{irq::TriggerMode::Level, irq::Polarity::ActiveHigh};
        auto const edge_mode = irq::LineMode{irq::TriggerMode::Edge, irq::Polarity::ActiveHigh};
        auto const level_attach = line->attach(action, ustl::some(level_mode));
        auto const after_level_attach = line->flow();
        auto const level_detach = line->detach(action);
        auto const edge_attach = line->attach(action, ustl::some(edge_mode));
        auto const after_edge_attach = line->flow();
        auto const edge_detach = line->detach(action);
        // The pin turned level behind the line's back, as the platform
        // programs pins without going through a line: an attach that reads
        // the mode takes the flow from what it read.
        PIN_TRIGGER.store(irq::TriggerMode::Level, std::memory_order_relaxed);
        auto const read_attach = line->attach(action, ustl::none());
        auto const after_read_attach = line->flow();
        auto const read_detach = line->detach(action);
        drop_from(*line, action);
        delete line;
        PIN_TRIGGER.store(irq::TriggerMode::Edge, std::memory_order_relaxed);

        log::info("irq.test:   bound: a level pin runs {}, an edge pin {}, a chip with no mode {}",
                  flow_name(bound_level), flow_name(bound_edge), flow_name(bound_modeless));
        verdict(tally, bound_level == irq::Flow::FastEoi && bound_edge == irq::Flow::Edge,
                "a line bound to its chip runs the flow of how its pin signals");
        verdict(tally, bound_modeless == irq::Flow::Edge,
                "a chip that cannot say how its pin signals gives the edge flow");
        verdict(tally,
                to_level == Status::Ok && after_level == irq::Flow::FastEoi &&
                    to_edge == Status::Ok && after_edge == irq::Flow::Edge,
                "configure moves the line's flow with its mode, there and back");
        verdict(tally,
                occupied == Status::Ok && refused == Status::AlreadyBound &&
                    after_refused == irq::Flow::Edge && left == Status::Ok,
                "a configure refused on an occupied line leaves its flow");
        verdict(tally,
                level_attach.is_ok() && *level_attach == irq::TriggerMode::Level &&
                    after_level_attach == irq::Flow::FastEoi && level_detach == Status::Ok &&
                    edge_attach.is_ok() && *edge_attach == irq::TriggerMode::Edge &&
                    after_edge_attach == irq::Flow::Edge && edge_detach == Status::Ok,
                "attach with a mode gives the line that mode's flow");
        verdict(tally,
                read_attach.is_ok() && *read_attach == irq::TriggerMode::Level &&
                    after_read_attach == irq::Flow::FastEoi && read_detach == Status::Ok,
                "attach without one reads the mode and gives the line its flow");
    }

    /// What one dispatch on a line of the test's own did to its pin.
    struct PinDispatch {
        irq::IrqReturn result = irq::IrqReturn::None;
        bool masked_before = false;
        bool masked = false;
        usize masks = 0;
        usize eois = 0;
        /// Of `eois`, those sent while the pin was unmasked.
        usize eois_unmasked = 0;
    };

    /// Sets the pin's mask bit to `masked`, then dispatches `line` once.
    auto dispatch_on_pin(irq::Desc &line, irq::Eoi const eoi, bool const masked = false)
        -> PinDispatch {
        PIN_MASKED.store(masked, std::memory_order_relaxed);
        auto const masks_before = MASKS_SENT.load(std::memory_order_relaxed);
        auto const eois_before = EOIS_SENT.load(std::memory_order_relaxed);
        auto const eois_unmasked_before = EOIS_UNMASKED.load(std::memory_order_relaxed);
        auto const result = line.handle_irq(eoi);
        return PinDispatch{
            .result = result,
            .masked_before = masked,
            .masked = PIN_MASKED.load(std::memory_order_relaxed),
            .masks = MASKS_SENT.load(std::memory_order_relaxed) - masks_before,
            .eois = EOIS_SENT.load(std::memory_order_relaxed) - eois_before,
            .eois_unmasked = EOIS_UNMASKED.load(std::memory_order_relaxed) - eois_unmasked_before,
        };
    }

    /// What each flow does to the pin: a line nobody is on is masked and its
    /// interrupt ended; one with an action is left as it was.
    auto check_flow_masking(Tally &tally) -> void {
        PIN_TRIGGER.store(irq::TriggerMode::Level, std::memory_order_relaxed);
        auto *const level_line = own_line(PIN_CHIP);
        PIN_TRIGGER.store(irq::TriggerMode::Edge, std::memory_order_relaxed);
        auto *const edge_line = own_line(PIN_CHIP);
        if (level_line == nullptr || edge_line == nullptr) {
            delete level_line;
            delete edge_line;
            verdict(tally, false, "make the lines for the flows' masks");
            return;
        }
        auto const flows_ok =
            level_line->flow() == irq::Flow::FastEoi && edge_line->flow() == irq::Flow::Edge;

        auto const level_unclaimed = dispatch_on_pin(*level_line, irq::Eoi::Send);
        auto const edge_unclaimed = dispatch_on_pin(*edge_line, irq::Eoi::Send);
        auto const level_played = dispatch_on_pin(*level_line, irq::Eoi::Skip);
        auto const edge_played = dispatch_on_pin(*edge_line, irq::Eoi::Skip);

        Probe probe;
        irq::Action level_action{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};
        irq::Action edge_action{&probe_handler, &probe, irq::IrqFlags::None, "irq-test"};
        auto const level_attached = level_line->attach(level_action);
        auto const edge_attached = edge_line->attach(edge_action);
        // With the pin unmasked and then masked: the flow neither masks the
        // first nor unmasks the second.
        auto const level_claimed = dispatch_on_pin(*level_line, irq::Eoi::Send);
        auto const edge_claimed = dispatch_on_pin(*edge_line, irq::Eoi::Send);
        auto const level_claimed_masked = dispatch_on_pin(*level_line, irq::Eoi::Send, true);
        auto const edge_claimed_masked = dispatch_on_pin(*edge_line, irq::Eoi::Send, true);
        auto const level_detached = level_line->detach(level_action);
        auto const edge_detached = edge_line->detach(edge_action);
        drop_from(*level_line, level_action);
        drop_from(*edge_line, edge_action);
        delete level_line;
        delete edge_line;

        // The line table refuses the chip before it makes a line, so it holds
        // nothing to free.
        std::array<HIrqNum, 1> const numbers{idle_line()};
        irq::DescTable table;
        auto const refused = table.init(ustl::views::Span(numbers), NO_MASK_CHIP);

        // The pin starts unmasked, so an end of interrupt sent before the
        // mask finds it unmasked: the order a level pin cannot afford.
        auto const unclaimed = [](PinDispatch const &d) {
            return d.result == irq::IrqReturn::None && d.masked && d.masks == 1 && d.eois == 1 &&
                   d.eois_unmasked == 0;
        };
        auto const played = [](PinDispatch const &d) {
            return d.result == irq::IrqReturn::None && d.masked && d.masks == 1 && d.eois == 0;
        };
        auto const claimed = [](PinDispatch const &d) {
            return d.result == irq::IrqReturn::Handled && d.masked == d.masked_before &&
                   d.masks == 0 && d.eois == 1;
        };
        verdict(tally, flows_ok, "the two lines run the level and the edge flow");
        verdict(tally, unclaimed(level_unclaimed) && unclaimed(edge_unclaimed),
                "an interrupt on a line nobody is on masks it, then ends it, on either flow");
        verdict(tally, played(level_played) && played(edge_played),
                "a played one masks it too, and ends nothing");
        verdict(tally,
                level_attached == Status::Ok && edge_attached == Status::Ok &&
                    claimed(level_claimed) && claimed(edge_claimed) &&
                    claimed(level_claimed_masked) && claimed(edge_claimed_masked) &&
                    level_detached == Status::Ok && edge_detached == Status::Ok,
                "a line with an action is neither masked nor unmasked by its flow, on either");
        verdict(tally, refused == Status::InvArgs,
                "the line table refuses a chip that cannot mask");
    }

    /// What the irq layer can show of the chip binding: the calls reach a
    /// line's chip and answer Ok, a line with no chip answers NotSupported,
    /// and configure() is refused on an occupied line. Two fields of the
    /// IO-APIC entry are read back: the trigger mode, through trigger_mode(),
    /// and the mask bit, through is_masked_for_test(), each from the entry
    /// itself, so a mask or an unmask that never reached the pin shows.
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
        auto const mask_read = irq::is_masked_for_test(idle_line());
        auto const unmask_status = irq::unmask(idle_line());
        auto const unmask_read = irq::is_masked_for_test(idle_line());
        auto const remask_status = irq::mask(idle_line());
        auto const remask_read = irq::is_masked_for_test(idle_line());

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
        auto const no_line_mask_read = irq::is_masked_for_test(OWN_LINE);

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
        auto const chipless_mask_read = line->is_masked();
        delete line;

        log::info("irq.test:   configure on the PIT's line answered {}, on an occupied line {}",
                  to_string(pit_status), to_string(occupied_status));
        verdict(tally, no_line_status == Status::NoResource && invalid_status == Status::InvArgs,
                "mask and unmask answer NoResource for no line, InvArgs for the invalid number");
        verdict(tally,
                mask_status == Status::Ok && unmask_status == Status::Ok &&
                    remask_status == Status::Ok,
                "mask and unmask go through the idle line's chip");
        verdict(tally,
                mask_read.is_ok() && *mask_read && unmask_read.is_ok() && !*unmask_read &&
                    remask_read.is_ok() && *remask_read,
                "the idle line's IO-APIC entry reads masked, unmasked and masked after each");
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
                no_line_mask_read.is_err() && no_line_mask_read.unwrap_err() == Status::NoResource,
                "is_masked_for_test answers NoResource for no line");
        verdict(tally,
                chipless_mask == Status::NotSupported &&
                    chipless_configure == Status::NotSupported && chipless_read.is_err() &&
                    chipless_read.unwrap_err() == Status::NotSupported &&
                    chipless_mask_read.is_err() &&
                    chipless_mask_read.unwrap_err() == Status::NotSupported,
                "a line with no chip answers NotSupported, its mask read as well");
    }

    /// What a Oneshot test action's handler answers, and what it saw. Atomic
    /// for the reason the pin's fields are: the handler runs in whatever
    /// context the line's flow does.
    struct OneshotProbe {
        std::atomic<irq::IrqReturn> answer{irq::IrqReturn::WakeThread};
        /// Whether the line read masked at its controller while the handler
        /// ran, on its last call.
        std::atomic<bool> saw_masked{false};
    };

    /// Whether `line` is masked at its controller now: the pin kept in memory
    /// for a line of the test's own, the IO-APIC entry for a line of the
    /// table. A read that fails answers false.
    auto masked_now(VIrqNum const line) -> bool {
        if (line == OWN_LINE) {
            return PIN_MASKED.load(std::memory_order_relaxed);
        }
        auto const read = irq::is_masked_for_test(line);
        return read.is_ok() && *read;
    }

    /// Reads the line's mask from inside the dispatch, which only a handler
    /// can: that the flow masks a Oneshot line before the action runs, not
    /// only by the time the dispatch returns. The read takes LI under the
    /// dispatch's L1, the order every other path takes them in.
    auto oneshot_handler(VIrqNum const line, void *const context) -> irq::IrqReturn {
        auto &probe = *static_cast<OneshotProbe *>(context);
        probe.saw_masked.store(masked_now(line), std::memory_order_relaxed);
        return probe.answer.load(std::memory_order_relaxed);
    }

    /// The idle line's pin, read from its IO-APIC entry: masked, and
    /// unmasked. A read that fails is neither: both answer false.
    auto idle_masked() -> bool {
        auto const read = irq::is_masked_for_test(idle_line());
        return read.is_ok() && *read;
    }

    auto idle_unmasked() -> bool {
        auto const read = irq::is_masked_for_test(idle_line());
        return read.is_ok() && !*read;
    }

    /// What only a line of the test's own can show of Oneshot, over the pin
    /// kept in memory: the end of interrupt, which a played dispatch of a
    /// real line does not send, and so whether it goes out with the pin
    /// masked. The edge flow, a chip that cannot unmask and a finalize on a
    /// line that is not Oneshot are checked here too, on lines that cost
    /// nothing to set up.
    auto check_oneshot_on_pin(Tally &tally) -> void {
        PIN_TRIGGER.store(irq::TriggerMode::Level, std::memory_order_relaxed);
        auto *const level_line = own_line(PIN_CHIP);
        PIN_TRIGGER.store(irq::TriggerMode::Edge, std::memory_order_relaxed);
        auto *const edge_line = own_line(PIN_CHIP);
        auto *const counting_line = own_line(COUNTING_CHIP);
        if (level_line == nullptr || edge_line == nullptr || counting_line == nullptr) {
            delete level_line;
            delete edge_line;
            delete counting_line;
            verdict(tally, false, "make the lines for Oneshot");
            return;
        }

        OneshotProbe probe;
        irq::Action level_action{&oneshot_handler, &probe, irq::IrqFlags::Oneshot, "irq-test"};
        irq::Action edge_action{&oneshot_handler, &probe, irq::IrqFlags::Oneshot, "irq-test"};
        irq::Action counting_action{&oneshot_handler, &probe, irq::IrqFlags::Oneshot, "irq-test"};
        auto const level_attached = level_line->attach(level_action);
        auto const edge_attached = edge_line->attach(edge_action);
        // The counting chip masks but cannot unmask.
        auto const refused = counting_line->attach(counting_action);

        // Each dispatch starts with the pin unmasked, as its owner leaves it
        // once ready, so an end of interrupt sent before the mask shows.
        auto const woke = dispatch_on_pin(*level_line, irq::Eoi::Send);
        auto const woke_saw_masked = probe.saw_masked.load(std::memory_order_relaxed);
        level_line->finalize_oneshot(level_action);
        auto const finalized_masked = PIN_MASKED.load(std::memory_order_relaxed);
        probe.answer.store(irq::IrqReturn::Handled, std::memory_order_relaxed);
        auto const handled = dispatch_on_pin(*level_line, irq::Eoi::Send);
        auto const handled_saw_masked = probe.saw_masked.load(std::memory_order_relaxed);
        probe.answer.store(irq::IrqReturn::WakeThread, std::memory_order_relaxed);
        auto const edge_woke = dispatch_on_pin(*edge_line, irq::Eoi::Send);
        auto const edge_saw_masked = probe.saw_masked.load(std::memory_order_relaxed);

        auto const level_detached = level_line->detach(level_action);
        auto const edge_detached = edge_line->detach(edge_action);

        // The level line again, now with an action that does not say
        // Oneshot, and a pin masked as if by its owner. The flow never masks
        // such a line for its action, so a finalize there could only undo a
        // mask somebody else asked for: it must do nothing.
        irq::Action plain_action{&oneshot_handler, &probe, irq::IrqFlags::None, "irq-test"};
        auto const plain_attached = level_line->attach(plain_action);
        PIN_MASKED.store(true, std::memory_order_relaxed);
        level_line->finalize_oneshot(plain_action);
        auto const plain_finalize_masked = PIN_MASKED.load(std::memory_order_relaxed);
        auto const plain_detached = level_line->detach(plain_action);

        drop_from(*level_line, level_action);
        drop_from(*level_line, plain_action);
        drop_from(*edge_line, edge_action);
        drop_from(*counting_line, counting_action);
        delete level_line;
        delete edge_line;
        delete counting_line;

        verdict(tally,
                level_attached == Status::Ok && woke.result == irq::IrqReturn::WakeThread &&
                    woke_saw_masked && woke.masks == 1 && woke.masked && woke.eois == 1 &&
                    woke.eois_unmasked == 0,
                "WakeThread on a Oneshot level line: masked before the action, ended masked");
        verdict(tally,
                !finalized_masked && handled.result == irq::IrqReturn::Handled &&
                    handled_saw_masked && handled.masks == 1 && !handled.masked &&
                    handled.eois == 1 && level_detached == Status::Ok,
                "after the finalize, one the action handles is masked around it, unmasked after");
        verdict(tally,
                edge_attached == Status::Ok && edge_woke.result == irq::IrqReturn::WakeThread &&
                    !edge_saw_masked && edge_woke.masks == 0 && !edge_woke.masked &&
                    edge_woke.eois == 1 && edge_detached == Status::Ok,
                "an edge line ignores Oneshot: WakeThread leaves it unmasked");
        verdict(tally, refused == Status::NotSupported && !counting_action.is_attached(),
                "a line whose chip cannot unmask refuses a Oneshot action");
        verdict(tally,
                plain_attached == Status::Ok && plain_finalize_masked &&
                    plain_detached == Status::Ok,
                "a finalize on a line that is not Oneshot does nothing: the line stays masked");
    }

    /// Oneshot on a real line: the idle line, set level and active high, read
    /// back from its IO-APIC entry after each step. Nothing drives the pin,
    /// so in that mode it stays deasserted and the line may be unmasked, as
    /// interruptline.test unmasks it; the interrupts are played through
    /// handle_irq_for_test(), which ends none at the controller.
    auto check_oneshot_idle_line(Tally &tally) -> void {
        OneshotProbe probe;
        irq::Action action{&oneshot_handler, &probe, irq::IrqFlags::Oneshot, "irq-test"};
        irq::Action next{&oneshot_handler, &probe, irq::IrqFlags::Oneshot, "irq-test"};
        irq::Action shared{&oneshot_handler, &probe, irq::IrqFlags::Oneshot | irq::IrqFlags::Shared,
                           "irq-test"};
        auto const level = ustl::some(irq::LineMode{.trigger = irq::TriggerMode::Level,
                                                    .polarity = irq::Polarity::ActiveHigh});

        auto const shared_status = irq::attach(idle_line(), shared);
        drop_if_attached(shared);

        auto const attached = irq::attach(idle_line(), action, level);
        auto const unmasked = irq::unmask(idle_line()) == Status::Ok && idle_unmasked();
        auto const spurious_before = spurious_on(idle_line());

        // The action leaves its device to a thread: the line is held.
        auto const woke = irq::handle_irq_for_test(idle_line());
        auto const woke_saw_masked = probe.saw_masked.load(std::memory_order_relaxed);
        auto const woke_masked = idle_masked();
        auto const spurious_after = spurious_on(idle_line());

        // Held: an unmask from anyone leaves the line masked, since the
        // thread still owes the device, and answers Ok all the same.
        auto const held_unmask = irq::unmask(idle_line());
        auto const held_unmask_masked = idle_masked();

        // Still held, so the unmask cleared nothing: one the action handles
        // itself does not unmask it either.
        probe.answer.store(irq::IrqReturn::Handled, std::memory_order_relaxed);
        auto const while_held = irq::handle_irq_for_test(idle_line());
        auto const held_masked = idle_masked();

        irq::finalize_oneshot(idle_line(), action);
        auto const finalized_unmasked = idle_unmasked();

        // Not held: masked around the action, unmasked after it.
        auto const handled = irq::handle_irq_for_test(idle_line());
        auto const handled_saw_masked = probe.saw_masked.load(std::memory_order_relaxed);
        auto const handled_unmasked = idle_unmasked();

        // Held again, and then the action goes. The finalize that comes after
        // the detach, as an ack would that lost the race to a destroy, finds
        // the action gone and leaves the line masked.
        probe.answer.store(irq::IrqReturn::WakeThread, std::memory_order_relaxed);
        auto const rewoke = irq::handle_irq_for_test(idle_line());
        auto const detached = irq::detach(action);
        irq::finalize_oneshot(idle_line(), action);
        auto const late_finalize_masked = idle_masked();

        // The next owner, another action. Had the detach left the line held,
        // the first interrupt it handles itself would leave the line masked
        // for good.
        auto const reattached = irq::attach(idle_line(), next, level);
        auto const reunmasked = irq::unmask(idle_line()) == Status::Ok;
        probe.answer.store(irq::IrqReturn::Handled, std::memory_order_relaxed);
        auto const next_handled = irq::handle_irq_for_test(idle_line());
        auto const next_unmasked = idle_unmasked();

        // The next owner held: a late finalize of the action that left must
        // not unmask the line, whose debt is now the next owner's; the next
        // owner's own finalize does.
        probe.answer.store(irq::IrqReturn::WakeThread, std::memory_order_relaxed);
        auto const next_woke = irq::handle_irq_for_test(idle_line());
        irq::finalize_oneshot(idle_line(), action);
        auto const stale_finalize_masked = idle_masked();
        irq::finalize_oneshot(idle_line(), next);
        auto const own_finalize_unmasked = idle_unmasked();

        // The line is unmasked now, so this detach must mask it itself.
        auto const last_detached = irq::detach(next);
        auto const last_masked = idle_masked();
        drop_if_attached(action);
        drop_if_attached(next);
        // Back to ISA's edge and active high, where check_line_control()
        // leaves the line and the barrier check expects it.
        auto const restored =
            irq::configure(idle_line(), irq::TriggerMode::Edge, irq::Polarity::ActiveHigh);

        verdict(tally, shared_status == Status::InvArgs && !shared.is_attached(),
                "an action that says Oneshot and Shared is refused");
        verdict(tally, attached.is_ok() && *attached == irq::TriggerMode::Level && unmasked,
                "a Oneshot action takes the idle line, set level, and its owner unmasks it");
        verdict(tally, woke == irq::IrqReturn::WakeThread && woke_saw_masked && woke_masked,
                "WakeThread masks the line before the action and leaves it masked: held");
        verdict(tally, spurious_after == spurious_before,
                "WakeThread counts as handled, not as spurious");
        verdict(tally, held_unmask == Status::Ok && held_unmask_masked,
                "while held, unmask answers Ok and leaves the line masked");
        verdict(tally, while_held == irq::IrqReturn::Handled && held_masked,
                "and held stays: one the action handles itself leaves the line masked");
        verdict(tally, finalized_unmasked, "finalize_oneshot clears held and unmasks the line");
        verdict(tally, handled == irq::IrqReturn::Handled && handled_saw_masked && handled_unmasked,
                "not held, one the action handles is masked around it and unmasked after");
        verdict(tally,
                rewoke == irq::IrqReturn::WakeThread && detached == Status::Ok &&
                    late_finalize_masked,
                "a finalize after the detach does nothing: the line stays masked");
        verdict(tally,
                reattached.is_ok() && reunmasked && next_handled == irq::IrqReturn::Handled &&
                    next_unmasked,
                "detach cleared held: the next owner's interrupt unmasks the line");
        verdict(tally,
                next_woke == irq::IrqReturn::WakeThread && stale_finalize_masked &&
                    own_finalize_unmasked,
                "a stale finalize leaves the next owner's held line masked; its own unmasks it");
        verdict(tally, last_detached == Status::Ok && last_masked && restored == Status::Ok,
                "the last action's detach masks the line, which goes back to edge");
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
            if (irq::handle_irq_for_test(idle_line()) == irq::IrqReturn::Handled) {
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

    /// One dispatch of the idle line on another CPU, whose handler holds the
    /// line while this CPU calls into it: the handler's context and action,
    /// and the dispatching thread's. On the heap, as the barrier case is: a
    /// thread that never finishes still points at it, and it is leaked rather
    /// than freed under that thread.
    struct HoldCase {
        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> done{false};
        irq::Action action{&HoldCase::on_interrupt, this, irq::IrqFlags::None, "irq-test"};

        /// Holds the line for HOLD_FOR, timed by the steady clock, which reads
        /// its counter with interrupts off as well as on: the dispatch holds
        /// the line's lock with interrupts off for the whole of it.
        static auto on_interrupt(VIrqNum, void *const context) -> irq::IrqReturn {
            auto &c = *static_cast<HoldCase *>(context);
            c.started.store(true, std::memory_order_release);
            auto const until = time::SteadyClock::now() + HOLD_FOR;
            while (time::SteadyClock::now() < until) {
                arch::spin_hint();
            }
            c.finished.store(true, std::memory_order_release);
            return irq::IrqReturn::Handled;
        }
    };

    auto dispatch_held(void *const arg) -> i32 {
        auto &c = *static_cast<HoldCase *>(arg);
        // The handler records all the check reads; its answer is Handled.
        auto _ = irq::handle_irq_for_test(idle_line());
        // Last touch of |c|: once the command sees this, it frees it.
        c.done.store(true, std::memory_order_release);
        return 0;
    }

    /// Whether `call`, made on this CPU while a dispatch of the idle line on
    /// `cpu` runs its handler, waits for that handler: it is made after the
    /// handler started and before it finished, and returns only after it
    /// finished, which a call that takes the line's lock must, since the
    /// dispatch holds it throughout, and one that does not would not. `call`
    /// answers whether it did what it was asked.
    template <typename Call>
    auto waits_for_handler(CpuNum const cpu, Call const &call, char const *const name) -> bool {
        auto *const c = new (mm::GAF_KERNEL) HoldCase{};
        if (c == nullptr) {
            log::error("irq.test:   allocating the {} case failed", name);
            return false;
        }
        if (irq::attach(idle_line(), c->action) != Status::Ok) {
            log::error("irq.test:   the {} case could not take the idle line", name);
            delete c;
            return false;
        }
        auto *const dispatcher =
            task::Thread::spawn("irq-test-hold", dispatch_held, c, task::DEFAULT_PRIORITY);
        if (dispatcher == nullptr) {
            log::error("irq.test:   spawning the {} case's dispatcher failed", name);
            drop_if_attached(c->action);
            delete c;
            return false;
        }
        dispatcher->set_cpu_affinity_hard(CpuMask::of(cpu));
        // detach() and resume() separately: detach_and_resume() is declared
        // but has no definition yet.
        auto _ = dispatcher->detach();
        auto _ = dispatcher->resume();

        auto on_other_cpu = false;
        auto called_while_held = false;
        auto did = false;
        auto returned_after = false;
        {
            // This thread stays on its CPU from the handler's start to the
            // call's return, so the two run side by side: preempted, it could
            // come back to a handler already finished, which shows nothing.
            task::AutoPreemptDisabler const no_preemption;
            on_other_cpu = Cpu::id() != cpu;
            if (on_other_cpu &&
                spin_until([c] { return c->started.load(std::memory_order_acquire); })) {
                called_while_held = !c->finished.load(std::memory_order_acquire);
                did = call();
                returned_after = c->finished.load(std::memory_order_acquire);
            }
        }

        if (!spin_until([c] { return c->done.load(std::memory_order_acquire); })) {
            log::error("irq.test:   the {} case's dispatcher did not finish; leaking its state",
                       name);
            return false;
        }
        auto const detached = irq::detach(c->action);
        delete c;

        log::info("irq.test:   {}: from another CPU {}, called while the handler held the line "
                  "{}, did its work {}, returned after the handler {}",
                  name, on_other_cpu, called_while_held, did, returned_after);
        return detached == Status::Ok && called_while_held && did && returned_after;
    }

    /// mask() and unmask() take the line's lock: each, called while a dispatch
    /// of the idle line holds it on another CPU, returns only once that
    /// dispatch's handler has finished. The idle line runs the edge flow
    /// here, which neither masks nor unmasks a line with an action, so each
    /// call is the only write of the pin in its round, read back after it.
    auto check_mask_takes_line_lock(Tally &tally) -> void {
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            log::warn("irq.test:   skipped the line lock check: it needs two online CPUs, have {}",
                      num_cpus);
            return;
        }
        auto const cpu = static_cast<CpuNum>((Cpu::id() + 1) % num_cpus);

        auto const mask_waits = waits_for_handler(
            cpu, [] { return irq::mask(idle_line()) == Status::Ok && idle_masked(); }, "mask");
        auto const unmask_waits = waits_for_handler(
            cpu, [] { return irq::unmask(idle_line()) == Status::Ok && idle_unmasked(); },
            "unmask");

        verdict(tally, mask_waits,
                "mask waits for a handler of the line on another CPU: it takes the line's lock");
        verdict(tally, unmask_waits, "so does unmask");
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
        check_end_of_interrupt(tally);
        check_flow_selection(tally);
        check_flow_masking(tally);
        check_line_control(tally);
        check_oneshot_on_pin(tally);
        check_oneshot_idle_line(tally);
        check_mask_takes_line_lock(tally);
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
             "attach, detach, mask and configure an idle line, hold it Oneshot, and check the "
             "PIT's line is Permanent");
