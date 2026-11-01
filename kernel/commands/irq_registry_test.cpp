/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `irqregistry.test`: irq::DomainRegistry (.agents/tasks/irq-multi-arch/PLAN.md,
// step 10), on registries of the test's own with tables of synthetic domains:
// the machine has one controller, so what several do is tested here and nowhere
// else. dbg image only: it is the one with a kernel shell. What it checks:
//
//   - two domains whose hardware numbers overlap hold disjoint blocks of virqs,
//     the first the numbers a lone domain gets; a virq finds its domain's table
//     and the hole between the blocks finds none; interrupt_map's lookup by
//     domain and hardware number; the slot a root handler holds is published;
//   - registration refuses an id that is Invalid or in use, an empty list, a
//     number that is no hardware number or is given twice, a chip that cannot
//     end an interrupt, and a ninth domain; a refusal keeps nothing;
//   - removal: an unknown id is NotFound, Root is NotSupported, and a removed
//     domain's lookups and slot find nothing; its id may be registered again
//     and gets a block above everything given out, never the dead one;
//   - a removal that must be refused is, with nothing changed: a domain with a
//     Permanent line, or with an action that gave no cancel hook (AlreadyBound),
//     which then still works and is removed once the action is gone; a removal
//     that goes through tells each action;
//   - the grace period: readers on every other CPU look at the table of a
//     domain that is registered and removed again and again, and none finds the
//     table it began with gone before it ends. This needs two CPUs.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/irq/action.hpp>
#include <ours/irq/domain_registry.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/status.hpp>
#include <ours/task/irq_rcu.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/views/span.hpp>

namespace ours {
namespace {
    using irq::DomainId;
    using irq::DomainRegistry;
    using irq::DomainSlot;
    using task::IrqRcuReadGuard;
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(5);
    constexpr u32 STRESS_ROUNDS = 200;
    constexpr u32 LOOK_FOR = 2000;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
        usize skipped = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("irqregistry.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("irqregistry.test: FAIL  {}", name);
    }

    auto answered(Tally &tally, Status const got, Status const want, char const *const name)
        -> void {
        if (got != want) {
            log::error("irqregistry.test:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        verdict(tally, got == want, name);
    }

    auto no_op(irq::Data const &) -> void {}

    constexpr irq::Chip CHIP = {
        .mask = no_op,
        .unmask = no_op,
        .issue_eoi = no_op,
        .name = "irq-registry-test",
    };

    /// A chip that cannot end an interrupt.
    constexpr irq::Chip NO_EOI_CHIP = {
        .mask = no_op,
        .name = "irq-registry-test",
    };

    auto hooked_handler(VIrqNum, void *) -> irq::IrqReturn {
        return irq::IrqReturn::Handled;
    }

    auto hooked_cancel(VIrqNum, void *context) -> void;

    struct Hooked {
        Hooked(irq::IrqFlags const flags, irq::IrqCancel const cancel)
            : action(&hooked_handler, this, flags, "irq-registry-test", cancel) {}

        irq::Action action;
        std::atomic<u32> told{0};
    };

    auto hooked_cancel(VIrqNum, void *const context) -> void {
        static_cast<Hooked *>(context)->told.fetch_add(1, std::memory_order_relaxed);
    }

    constexpr std::array<HIrqNum, 4> FOUR{0, 1, 2, 3};
    constexpr std::array<HIrqNum, 2> TWO{0, 1};
    constexpr std::array<HIrqNum, 1> ONE{0};

    auto span(auto const &numbers) -> ustl::views::Span<HIrqNum const> {
        return ustl::views::Span<HIrqNum const>(numbers);
    }

    /// The virq of (id, hirqnum) and the owner of a virq, each in a read of
    /// its own.
    auto virq_in(DomainRegistry const &registry, DomainId const id, HIrqNum const hirqnum)
        -> VIrqNum {
        IrqRcuReadGuard const guard;
        return registry.virq_of(id, hirqnum, guard);
    }

    auto owner_of(DomainRegistry const &registry, VIrqNum const virq) -> irq::DescTable * {
        IrqRcuReadGuard const guard;
        return registry.table_of(virq, guard);
    }

    auto find_in(DomainRegistry const &registry, DomainId const id) -> irq::DescTable * {
        IrqRcuReadGuard const guard;
        return registry.find(id, guard);
    }

    /// Clean-up: the domains of a check are removed, so that a run leaves no
    /// tables behind (Root cannot be, and is left).
    auto drop(DomainRegistry &registry, DomainId const id) -> void {
        auto _ = registry.unregister_domain(id);
    }

    auto slot_holds(DomainSlot const &slot, irq::DescTable const *const table) -> bool {
        arch::AutoInterruptDisabler const off;
        return slot.table_with_irqs_off() == table;
    }

    auto check_registration(Tally &tally) -> void {
        DomainRegistry registry;
        DomainSlot slot_a;
        DomainSlot slot_b;
        auto const first = registry.register_domain(DomainId{1}, span(FOUR), CHIP, slot_a);
        auto const second = registry.register_domain(DomainId{2}, span(FOUR), CHIP, slot_b);
        answered(tally, first, Status::Ok, "a domain is registered");
        answered(tally, second, Status::Ok, "and a second one, over the same hardware numbers");

        auto const a0 = virq_in(registry, DomainId{1}, 0);
        auto const b0 = virq_in(registry, DomainId{2}, 0);
        verdict(tally, a0 == 32, "the first domain gets the virqs a lone domain gets");
        verdict(tally,
                b0 == 64 && virq_in(registry, DomainId{2}, 3) == 67 &&
                    virq_in(registry, DomainId{1}, 3) == 35,
                "and the second a block above it, so the same pin has two different virqs");

        auto *const table_a = find_in(registry, DomainId{1});
        auto *const table_b = find_in(registry, DomainId{2});
        verdict(tally, table_a != nullptr && table_b != nullptr && table_a != table_b,
                "each domain has a table of its own");
        verdict(tally,
                owner_of(registry, 32) == table_a && owner_of(registry, 35) == table_a &&
                    owner_of(registry, 64) == table_b && owner_of(registry, 67) == table_b,
                "a virq finds the table of its domain, at both ends of the block");
        verdict(tally,
                owner_of(registry, 36) == nullptr && owner_of(registry, 63) == nullptr &&
                    owner_of(registry, 68) == nullptr && owner_of(registry, 0) == nullptr &&
                    owner_of(registry, irq::INVALID_V_IRQ_NUM) == nullptr,
                "and the hole between the blocks, past them, and the invalid number find none");
        verdict(tally,
                virq_in(registry, DomainId{9}, 0) == irq::INVALID_V_IRQ_NUM &&
                    virq_in(registry, DomainId{1}, 99) == irq::INVALID_V_IRQ_NUM,
                "interrupt_map's lookup finds no virq for an unknown domain or pin");
        verdict(tally, slot_holds(slot_a, table_a) && slot_holds(slot_b, table_b),
                "the slot a root handler holds has its domain's table");
        drop(registry, DomainId{1});
        drop(registry, DomainId{2});
    }

    auto check_registration_refused(Tally &tally) -> void {
        DomainRegistry registry;
        DomainSlot slot;
        DomainSlot other;
        constexpr std::array<HIrqNum, 0> NONE{};
        constexpr std::array<HIrqNum, 2> WITH_INVALID{1, irq::INVALID_H_IRQ_NUM};
        constexpr std::array<HIrqNum, 2> TWICE{1, 1};

        answered(tally, registry.register_domain(DomainId::Invalid, span(TWO), CHIP, slot),
                 Status::InvArgs, "the Invalid id is refused");
        answered(tally, registry.register_domain(DomainId{1}, span(NONE), CHIP, slot),
                 Status::InvArgs, "an empty list is refused");
        answered(tally, registry.register_domain(DomainId{1}, span(WITH_INVALID), CHIP, slot),
                 Status::InvArgs, "a number that is no hardware number is refused");
        answered(tally, registry.register_domain(DomainId{1}, span(TWO), NO_EOI_CHIP, slot),
                 Status::InvArgs, "a chip that cannot end an interrupt is refused");
        answered(tally, registry.register_domain(DomainId{1}, span(TWICE), CHIP, slot),
                 Status::AlreadyExists, "a number given twice is refused");
        verdict(tally,
                find_in(registry, DomainId{1}) == nullptr &&
                    virq_in(registry, DomainId{1}, 1) == irq::INVALID_V_IRQ_NUM,
                "and none of them keeps anything");

        answered(tally, registry.register_domain(DomainId{1}, span(TWO), CHIP, slot), Status::Ok,
                 "so the id takes a good list after them");
        answered(tally, registry.register_domain(DomainId{1}, span(ONE), CHIP, other),
                 Status::AlreadyExists, "an id in use is refused");

        // The registry holds CAPACITY domains: one is registered; fill it.
        std::array<DomainSlot, DomainRegistry::CAPACITY> slots;
        auto filled = true;
        for (usize i = 0; i + 1 < DomainRegistry::CAPACITY; ++i) {
            filled &= registry.register_domain(DomainId{static_cast<u32>(10 + i)}, span(ONE), CHIP,
                                               slots[i]) == Status::Ok;
        }
        verdict(tally, filled, "the registry takes as many domains as its capacity");
        answered(tally,
                 registry.register_domain(DomainId{99}, span(ONE), CHIP,
                                          slots[DomainRegistry::CAPACITY - 1]),
                 Status::NoSpace, "and refuses one more");
        drop(registry, DomainId{1});
        for (usize i = 0; i + 1 < DomainRegistry::CAPACITY; ++i) {
            drop(registry, DomainId{static_cast<u32>(10 + i)});
        }
    }

    auto check_unregistration(Tally &tally) -> void {
        DomainRegistry registry;
        DomainSlot root_slot;
        DomainSlot slot_a;
        DomainSlot slot_b;
        auto const ready =
            registry.register_domain(DomainId::Root, span(TWO), CHIP, root_slot) == Status::Ok &&
            registry.register_domain(DomainId{1}, span(FOUR), CHIP, slot_a) == Status::Ok &&
            registry.register_domain(DomainId{2}, span(FOUR), CHIP, slot_b) == Status::Ok;
        verdict(tally, ready, "three domains, Root among them");
        if (!ready) {
            return;
        }

        answered(tally, registry.unregister_domain(DomainId{7}), Status::NotFound,
                 "an unknown id is NotFound");
        answered(tally, registry.unregister_domain(DomainId::Root), Status::NotSupported,
                 "Root is NotSupported");
        verdict(tally, find_in(registry, DomainId::Root) != nullptr, "and stays registered");

        auto const before = virq_in(registry, DomainId{2}, 0);
        answered(tally, registry.unregister_domain(DomainId{2}), Status::Ok, "a domain is removed");
        verdict(tally,
                find_in(registry, DomainId{2}) == nullptr &&
                    owner_of(registry, before) == nullptr &&
                    virq_in(registry, DomainId{2}, 0) == irq::INVALID_V_IRQ_NUM &&
                    slot_holds(slot_b, nullptr),
                "its lookups and its slot find nothing");
        verdict(tally,
                find_in(registry, DomainId{1}) != nullptr && owner_of(registry, 32) != nullptr,
                "the others are untouched");
        answered(tally, registry.unregister_domain(DomainId{2}), Status::NotFound,
                 "removing it again is NotFound");

        answered(tally, registry.register_domain(DomainId{2}, span(FOUR), CHIP, slot_b), Status::Ok,
                 "its id is registered again");
        auto const after = virq_in(registry, DomainId{2}, 0);
        verdict(tally, after != before && after >= 96 && owner_of(registry, before) == nullptr,
                "in a block above everything given out; the dead block stays dead");
        drop(registry, DomainId{1});
        drop(registry, DomainId{2});
    }

    auto check_removal_refused(Tally &tally) -> void {
        DomainRegistry registry;
        DomainSlot hookless_slot;

        // A Permanent line: the domain, its registry, its slot and its owner
        // are leaked, one set per run, as the action can never come off.
        auto *const permanent =
            new (mm::GAF_KERNEL) Hooked(irq::IrqFlags::Permanent, &hooked_cancel);
        auto *const permanent_registry = new (mm::GAF_KERNEL) DomainRegistry();
        auto *const leaked_slot = new (mm::GAF_KERNEL) DomainSlot();
        if (permanent == nullptr || permanent_registry == nullptr || leaked_slot == nullptr) {
            verdict(tally, false, "allocate the Permanent domain");
            return;
        }
        auto const registered =
            permanent_registry->register_domain(DomainId{1}, span(TWO), CHIP, *leaked_slot);
        auto *const table = find_in(*permanent_registry, DomainId{1});
        auto const virq0 = virq_in(*permanent_registry, DomainId{1}, 0);
        auto const virq1 = virq_in(*permanent_registry, DomainId{1}, 1);
        auto attached = Status::NotFound;
        if (table != nullptr) {
            attached = table->attach(virq1, permanent->action);
        }
        verdict(tally, registered == Status::Ok && attached == Status::Ok,
                "a domain with a Permanent action on one line");
        answered(tally, permanent_registry->unregister_domain(DomainId{1}), Status::AlreadyBound,
                 "is not removed");
        Hooked probe(irq::IrqFlags::None, &hooked_cancel);
        verdict(tally,
                find_in(*permanent_registry, DomainId{1}) == table && permanent->told.load() == 0 &&
                    table->unmask(virq0) == Status::Ok &&
                    table->attach(virq0, probe.action) == Status::Ok,
                "and works as before: no mark is left on its other line, and nothing was told");
        auto _ = table->detach(probe.action); // Clean-up; the check above is the result.

        // An action with no cancel hook.
        Hooked hookless(irq::IrqFlags::None, nullptr);
        Hooked hooked(irq::IrqFlags::None, &hooked_cancel);
        auto const ok =
            registry.register_domain(DomainId{1}, span(TWO), CHIP, hookless_slot) == Status::Ok;
        auto *const second = find_in(registry, DomainId{1});
        auto const v0 = virq_in(registry, DomainId{1}, 0);
        auto const v1 = virq_in(registry, DomainId{1}, 1);
        auto const both = ok && second->attach(v0, hooked.action) == Status::Ok &&
                          second->attach(v1, hookless.action) == Status::Ok;
        verdict(tally, both, "a domain with an action with a hook and one without");
        answered(tally, registry.unregister_domain(DomainId{1}), Status::AlreadyBound,
                 "is not removed either");
        verdict(tally,
                hooked.told.load() == 0 && hooked.action.is_attached() &&
                    second->unmask(v0) == Status::Ok,
                "its hooked action is left on its line, untold, and the line unmasks");
        auto const dropped = second->detach(hookless.action);
        answered(tally, registry.unregister_domain(DomainId{1}), Status::Ok,
                 "once the hookless action is gone it is removed");
        verdict(tally,
                dropped == Status::Ok && hooked.told.load() == 1 && !hooked.action.is_attached(),
                "and the hooked action is told, once, and is off its line");
    }

    /// The stress case: what the readers look at is whichever domain is
    /// registered, found by its virq. A reader notes the table's block as it
    /// finds it and looks at it again at the end; a table freed under it, or
    /// freed and made again, has another.
    struct Stress {
        DomainRegistry registry;
        std::atomic<VIrqNum> probe{irq::INVALID_V_IRQ_NUM};
        std::atomic<bool> stop{false};
        std::atomic<u64> seen[MAX_CPU]{};
        std::atomic<u64> broken[MAX_CPU]{};
    };
    Stress *STRESS = nullptr;

    auto look_once(CpuNum const cpu) -> bool {
        IrqRcuReadGuard const guard;
        auto const virq = STRESS->probe.load(std::memory_order_acquire);
        auto *const table = STRESS->registry.table_of(virq, guard);
        if (table == nullptr) {
            return false;
        }
        auto const first = table->first_virq();
        STRESS->seen[cpu].fetch_add(1, std::memory_order_relaxed);
        for (u32 i = 0; i < LOOK_FOR; ++i) {
            if (table->is_destroyed() || table->first_virq() != first ||
                table->hwirq_of(virq) == irq::INVALID_H_IRQ_NUM) {
                STRESS->broken[cpu].fetch_add(1, std::memory_order_relaxed);
                break;
            }
            arch::spin_hint();
        }
        return true;
    }

    CpuNum STRESS_CPUS[MAX_CPU];

    auto stress_reader(void *const arg) -> i32 {
        auto const cpu = *static_cast<CpuNum *>(arg);
        while (!STRESS->stop.load(std::memory_order_acquire)) {
            if (!look_once(cpu)) {
                for (u32 i = 0; i < 64; ++i) {
                    arch::spin_hint();
                }
            }
        }
        return 0;
    }

    auto join_within(Thread *const thread) -> bool {
        return thread != nullptr &&
               thread->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
                   Status::Ok;
    }

    auto check_stress(Tally &tally, CpuMask const &reader_cpus) -> void {
        STRESS = new (mm::GAF_KERNEL) Stress();
        if (STRESS == nullptr) {
            verdict(tally, false, "allocate the stress case");
            return;
        }

        Thread *readers[MAX_CPU]{};
        reader_cpus.for_each([&readers](CpuNum const cpu) {
            STRESS_CPUS[cpu] = cpu;
            auto *const thread = Thread::spawn("irqregistry.test/look", stress_reader,
                                               &STRESS_CPUS[cpu], task::DEFAULT_PRIORITY);
            if (thread == nullptr) {
                return;
            }
            thread->set_cpu_affinity_hard(CpuMask::of(cpu));
            auto _ = thread->resume();
            readers[cpu] = thread;
        });

        DomainSlot slot;
        auto registered = u32{0};
        auto removed = u32{0};
        for (u32 round = 0; round < STRESS_ROUNDS; ++round) {
            if (STRESS->registry.register_domain(DomainId{3}, span(TWO), CHIP, slot) !=
                Status::Ok) {
                break;
            }
            registered += 1;
            STRESS->probe.store(virq_in(STRESS->registry, DomainId{3}, 0),
                                std::memory_order_release);
            auto _ = Thread::Current::sleep_for(Microseconds(200), task::Interruptible::No);
            if (STRESS->registry.unregister_domain(DomainId{3}) != Status::Ok) {
                break;
            }
            removed += 1;
        }
        STRESS->stop.store(true, std::memory_order_release);

        auto joined = true;
        auto every_reader_saw = true;
        auto seen = u64{0};
        auto broken = u64{0};
        reader_cpus.for_each([&](CpuNum const cpu) {
            joined &= join_within(readers[cpu]);
            auto const saw = STRESS->seen[cpu].load(std::memory_order_relaxed);
            every_reader_saw &= saw > 0;
            seen += saw;
            broken += STRESS->broken[cpu].load(std::memory_order_relaxed);
        });
        log::info("irqregistry.test: {} domains registered and removed, {} readers saw a table "
                  "{} times, {} of them broken",
                  removed, reader_cpus.count(), seen, broken);
        verdict(tally, registered == STRESS_ROUNDS && removed == STRESS_ROUNDS,
                "every round registered and removed the domain");
        verdict(tally, joined, "the readers start and end");
        verdict(tally, every_reader_saw && seen >= STRESS_ROUNDS / 4,
                "every reader looked at a table, in at least a quarter of the rounds in all");
        verdict(tally, broken == 0, "and none found its table gone or replaced before it ended");
        if (joined && registered == removed) {
            delete STRESS;
        }
        STRESS = nullptr;
    }

    auto irq_registry_test_command(int, char **) -> int {
        Tally tally;

        check_registration(tally);
        check_registration_refused(tally);
        check_unregistration(tally);
        check_removal_refused(tally);

        auto const online = Cpu::online_mask();
        if (online.count() < 2) {
            log::info("irqregistry.test: one CPU only, the stress is skipped");
            tally.skipped += 1;
        } else {
            auto writer_cpu = INVALID_CPU_NUM;
            online.for_each([&writer_cpu](CpuNum const cpu) {
                if (writer_cpu == INVALID_CPU_NUM) {
                    writer_cpu = cpu;
                }
            });
            auto readers = online;
            readers.reset(writer_cpu);

            // The writer's CPU for the command thread: a reader holds its CPU
            // with interrupts off, and the writer waits for it.
            auto *const self = Thread::Current::get();
            self->set_cpu_affinity_hard(CpuMask::of(writer_cpu));
            auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
            if (Cpu::id() != writer_cpu) {
                log::error("irqregistry.test: FAILED -- the command did not move to CPU {}",
                           writer_cpu);
                self->set_cpu_affinity_hard(cpu_possible_mask());
                return 1;
            }
            check_stress(tally, readers);
            self->set_cpu_affinity_hard(cpu_possible_mask());
        }

        if (tally.failed != 0) {
            log::error("irqregistry.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        if (tally.skipped != 0) {
            log::info("irqregistry.test: ok -- {} checks, {} case(s) skipped", tally.passed,
                      tally.skipped);
            return 0;
        }
        log::info("irqregistry.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(IrqRegistryTest, "irqregistry.test", ours::irq_registry_test_command,
             "irq::DomainRegistry: domains with disjoint virq blocks, registration and removal "
             "with their refusals, and readers on other CPUs against a domain registered and "
             "removed again and again");
