/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// IrqRegistry.DomainsHoldDisjointBlocksAndRemovalWaitsForReaders:
// irq::DomainRegistry (.agents/tasks/irq-multi-arch/PLAN.md, step 10), on
// registries of the test's own with tables of synthetic domains: the machine
// has one controller, so what several do is tested here and nowhere else. dbg
// image only: it is the one with a kernel shell. What it checks:
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
#include <ours/irq/mod.hpp>
#include <ours/irq/registration.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/status.hpp>
#include <ours/task/irq_rcu.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <heap/new.hpp>
#include <kernel/kernel/irq/domain_registry.hpp>
#include <kernel/kernel/irq/line_access.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <ustl/views/span.hpp>

namespace ours {
namespace {
    using irq::DomainEndpoint;
    using irq::DomainId;
    using irq::DomainRegistry;
    using task::IrqRcuReadGuard;
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(5);
    constexpr u32 STRESS_ROUNDS = 200;
    constexpr u32 LOOK_FOR = 2000;

    auto no_op(irq::ControllerSource const &) -> void {}

    constexpr irq::ControllerOps CHIP = {
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask = no_op,
        .unmask = no_op,
        .issue_eoi = no_op,
    };

    /// A chip that cannot end an interrupt.
    constexpr irq::ControllerOps NO_EOI_CHIP = {
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask = no_op,
    };

    auto hooked_handler(VIrqNum, void *) -> irq::IrqReturn {
        return irq::IrqReturn::Handled;
    }

    auto hooked_cancel(VIrqNum, void *context) -> void;

    struct Hooked {
        Hooked(irq::RegistrationOptions const flags, irq::IrqRevocationHandler const cancel)
            : action({.handle = &hooked_handler,
                      .revoke = cancel,
                      .ready = [](void *) -> bool { return true; }},
                     this, flags) {}

        irq::Registration action;
        std::atomic<u32> told{0};
    };

    auto hooked_cancel(VIrqNum, void *const context) -> void {
        static_cast<Hooked *>(context)->told.fetch_add(1, std::memory_order_relaxed);
    }

    constexpr std::array<HIrqNum, 4> FOUR{HIrqNum{0}, HIrqNum{1}, HIrqNum{2}, HIrqNum{3}};
    constexpr std::array<HIrqNum, 2> TWO{HIrqNum{0}, HIrqNum{1}};
    constexpr std::array<HIrqNum, 1> ONE{HIrqNum{0}};

    auto span(auto const &numbers) -> ustl::views::Span<HIrqNum const> {
        return ustl::views::Span<HIrqNum const>(numbers);
    }

    auto join_within(Thread *const thread) -> bool {
        return thread != nullptr &&
               thread->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
                   Status::Ok;
    }

    /// The virq of (id, hirqnum) and the owner of a virq, each in a read of
    /// its own.
    auto virq_in(DomainRegistry const &registry, DomainId const id, u32 const hirqnum) -> VIrqNum {
        IrqRcuReadGuard const guard;
        return registry.irq_for_source(id, HIrqNum{hirqnum}, guard);
    }

    auto owner_of(DomainRegistry const &registry, auto const virq) -> irq::Domain * {
        IrqRcuReadGuard const guard;
        return registry.domain_for(static_cast<VIrqNum>(virq), guard);
    }

    auto find_in(DomainRegistry const &registry, DomainId const id) -> irq::Domain * {
        IrqRcuReadGuard const guard;
        return registry.find(id, guard);
    }

    /// Clean-up: the domains of a check are removed, so that a run leaves no
    /// tables behind (Root cannot be, and is left).
    auto drop(DomainRegistry &registry, DomainId const id) -> void {
        auto _ = registry.unregister_domain(id);
    }

    auto slot_holds(DomainEndpoint const &slot, irq::Domain const *const table) -> bool {
        IrqRcuReadGuard const guard;
        return DomainRegistry::published_domain(slot, guard) == table;
    }

    struct ControllerInstance {
        u32 masks = 0;
        u32 unmasks = 0;
        u32 ended = 0;
        u32 handled = 0;
        VIrqNum observed = VIrqNum::Invalid;
    };

    constexpr irq::ControllerOps INSTANCE_OPS = {
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask =
            [](irq::ControllerSource const &source) {
                ++static_cast<ControllerInstance *>(source.context)->masks;
            },
        .unmask =
            [](irq::ControllerSource const &source) {
                ++static_cast<ControllerInstance *>(source.context)->unmasks;
            },
        .issue_eoi =
            [](irq::ControllerSource const &source) {
                ++static_cast<ControllerInstance *>(source.context)->ended;
            },
    };

    auto check_public_endpoints() -> void {
        constexpr auto A = DomainId{0x7f00};
        constexpr auto B = DomainId{0x7f01};
        ControllerInstance a;
        ControllerInstance b;
        DomainEndpoint endpoint_a;
        DomainEndpoint endpoint_b;
        EXPECT_TRUE(endpoint_a.dispatch(HIrqNum{0}).is_none(),
                    "an unpublished endpoint finds no source");
        auto const first = irq::register_domain(A, span(ONE), INSTANCE_OPS, endpoint_a, &a);
        auto const second = irq::register_domain(B, span(ONE), INSTANCE_OPS, endpoint_b, &b);
        EXPECT_TRUE(first == Status::Ok, "public registration publishes the first instance");
        EXPECT_TRUE(second == Status::Ok, "public registration publishes the second instance");
        if (first != Status::Ok || second != Status::Ok) {
            if (first == Status::Ok) {
                auto _ = irq::unregister_domain(A);
            }
            if (second == Status::Ok) {
                auto _ = irq::unregister_domain(B);
            }
            return;
        }
        auto const line_a = irq::irq_for_source(A, HIrqNum{0});
        auto const line_b = irq::irq_for_source(B, HIrqNum{0});
        EXPECT_TRUE(line_a != line_b && line_a != VIrqNum::Invalid && line_b != VIrqNum::Invalid,
                    "overlapping controller sources get distinct kernel identities");
        constexpr auto HANDLE = [](VIrqNum const line, void *const context) -> irq::IrqReturn {
            auto &instance = *static_cast<ControllerInstance *>(context);
            ++instance.handled;
            instance.observed = line;
            return irq::IrqReturn::Handled;
        };
        irq::Registration registration_a({.handle = HANDLE}, &a, irq::RegistrationOptions::None);
        irq::Registration registration_b({.handle = HANDLE}, &b, irq::RegistrationOptions::None);
        EXPECT_TRUE(irq::attach(line_a, registration_a) == Status::Ok, "attach to instance A");
        EXPECT_TRUE(irq::attach(line_b, registration_b) == Status::Ok, "attach to instance B");
        auto const result_a = endpoint_a.dispatch(HIrqNum{0});
        EXPECT_TRUE(result_a.is_some() && *result_a == irq::IrqReturn::Handled && a.handled == 1 &&
                        a.observed == line_a && a.ended == 1 && b.handled == 0 && b.ended == 0,
                    "endpoint A dispatches and ends only instance A");
        auto const result_b = endpoint_b.dispatch(HIrqNum{0});
        EXPECT_TRUE(result_b.is_some() && *result_b == irq::IrqReturn::Handled && b.handled == 1 &&
                        b.observed == line_b && b.ended == 1,
                    "endpoint B dispatches and ends only instance B");
        EXPECT_TRUE(endpoint_a.dispatch(HIrqNum{99}).is_none() && a.ended == 1,
                    "an unknown source leaves root completion to the caller");
        auto const a_masks = a.masks;
        auto const b_masks = b.masks;
        EXPECT_TRUE(irq::mask(line_a) == Status::Ok, "mask resolves the owning domain");
        EXPECT_TRUE(a.masks == a_masks + 1 && b.masks == b_masks,
                    "mask reaches the correct controller context");
        auto const b_unmasks = b.unmasks;
        EXPECT_TRUE(irq::unmask(line_b) == Status::Ok, "unmask resolves the other domain");
        EXPECT_EQ(b.unmasks, b_unmasks + 1, "unmask reaches the other controller context");
        EXPECT_TRUE(irq::detach(registration_a) == Status::Ok, "detach instance A");
        EXPECT_TRUE(irq::unregister_domain(A) == Status::Ok, "remove the first public domain");
        EXPECT_TRUE(endpoint_a.dispatch(HIrqNum{0}).is_none() &&
                        irq::mask(line_a) == Status::NoResource,
                    "withdrawal retracts its endpoint and control lookup");
        EXPECT_TRUE(endpoint_b.dispatch(HIrqNum{0}).is_some() && b.handled == 2 && b.ended == 2,
                    "the other endpoint survives independent removal");
        EXPECT_TRUE(irq::detach(registration_b) == Status::Ok, "detach instance B");
        EXPECT_TRUE(irq::unregister_domain(B) == Status::Ok, "remove the second public domain");
    }

    auto check_registration() -> void {
        DomainRegistry registry;
        DomainEndpoint slot_a;
        DomainEndpoint slot_b;
        auto const first = registry.register_domain(DomainId{1}, span(FOUR), CHIP, slot_a);
        auto const second = registry.register_domain(DomainId{2}, span(FOUR), CHIP, slot_b);
        EXPECT_TRUE(first == Status::Ok, "a domain is registered");
        EXPECT_TRUE(second == Status::Ok, "and a second one, over the same hardware numbers");

        auto const a0 = virq_in(registry, DomainId{1}, 0);
        auto const b0 = virq_in(registry, DomainId{2}, 0);
        EXPECT_TRUE(a0 == VIrqNum{32}, "the first domain gets the virqs a lone domain gets");
        EXPECT_TRUE(b0 == VIrqNum{64} && virq_in(registry, DomainId{2}, 3) == VIrqNum{67} &&
                        virq_in(registry, DomainId{1}, 3) == VIrqNum{35},
                    "and the second a block above it, so the same pin has two different virqs");

        auto *const table_a = find_in(registry, DomainId{1});
        auto *const table_b = find_in(registry, DomainId{2});
        EXPECT_TRUE(table_a != nullptr && table_b != nullptr && table_a != table_b,
                    "each domain has a table of its own");
        EXPECT_TRUE(owner_of(registry, 32) == table_a && owner_of(registry, 35) == table_a &&
                        owner_of(registry, 64) == table_b && owner_of(registry, 67) == table_b,
                    "a virq finds the table of its domain, at both ends of the block");
        EXPECT_TRUE(owner_of(registry, 36) == nullptr && owner_of(registry, 63) == nullptr &&
                        owner_of(registry, 68) == nullptr && owner_of(registry, 0) == nullptr &&
                        owner_of(registry, static_cast<u32>(VIrqNum::Invalid)) == nullptr,
                    "and the hole between the blocks, past them, and the invalid number find none");
        EXPECT_TRUE(virq_in(registry, DomainId{9}, 0) == VIrqNum::Invalid &&
                        virq_in(registry, DomainId{1}, 99) == VIrqNum::Invalid,
                    "interrupt_map's lookup finds no virq for an unknown domain or pin");
        EXPECT_TRUE(slot_holds(slot_a, table_a) && slot_holds(slot_b, table_b),
                    "the slot a root handler holds has its domain's table");
        drop(registry, DomainId{1});
        drop(registry, DomainId{2});
    }

    auto check_registration_refused() -> void {
        DomainRegistry registry;
        DomainEndpoint slot;
        DomainEndpoint other;
        constexpr std::array<HIrqNum, 0> NONE{};
        constexpr std::array<HIrqNum, 2> WITH_INVALID{HIrqNum{1}, HIrqNum::Invalid};
        constexpr std::array<HIrqNum, 2> TWICE{HIrqNum{1}, HIrqNum{1}};

        EXPECT_TRUE(registry.register_domain(DomainId::Invalid, span(TWO), CHIP, slot) ==
                        Status::InvArgs,
                    "the Invalid id is refused");
        EXPECT_TRUE(registry.register_domain(DomainId{1}, span(NONE), CHIP, slot) ==
                        Status::InvArgs,
                    "an empty list is refused");
        EXPECT_TRUE(registry.register_domain(DomainId{1}, span(WITH_INVALID), CHIP, slot) ==
                        Status::InvArgs,
                    "a number that is no hardware number is refused");
        EXPECT_TRUE(registry.register_domain(DomainId{1}, span(TWO), NO_EOI_CHIP, slot) ==
                        Status::InvArgs,
                    "a chip that cannot end an interrupt is refused");
        EXPECT_TRUE(registry.register_domain(DomainId{1}, span(TWICE), CHIP, slot) ==
                        Status::AlreadyExists,
                    "a number given twice is refused");
        EXPECT_TRUE(find_in(registry, DomainId{1}) == nullptr &&
                        virq_in(registry, DomainId{1}, 1) == VIrqNum::Invalid,
                    "and none of them keeps anything");

        EXPECT_TRUE(registry.register_domain(DomainId{1}, span(TWO), CHIP, slot) == Status::Ok,
                    "so the id takes a good list after them");
        EXPECT_TRUE(registry.register_domain(DomainId{2}, span(ONE), CHIP, slot) ==
                        Status::BadState,
                    "a slot that holds a table is refused, in every build");
        EXPECT_TRUE(find_in(registry, DomainId{2}) == nullptr,
                    "and the refusal registered nothing");
        EXPECT_TRUE(registry.register_domain(DomainId{1}, span(ONE), CHIP, other) ==
                        Status::AlreadyExists,
                    "an id in use is refused");

        // The registry holds CAPACITY domains: one is registered; fill it.
        std::array<DomainEndpoint, DomainRegistry::CAPACITY> slots;
        auto filled = true;
        for (usize i = 0; i + 1 < DomainRegistry::CAPACITY; ++i) {
            filled &= registry.register_domain(DomainId{static_cast<u32>(10 + i)}, span(ONE), CHIP,
                                               slots[i]) == Status::Ok;
        }
        EXPECT_TRUE(filled, "the registry takes as many domains as its capacity");
        EXPECT_TRUE(registry.register_domain(DomainId{99}, span(ONE), CHIP,
                                             slots[DomainRegistry::CAPACITY - 1]) ==
                        Status::NoSpace,
                    "and refuses one more");
        drop(registry, DomainId{1});
        for (usize i = 0; i + 1 < DomainRegistry::CAPACITY; ++i) {
            drop(registry, DomainId{static_cast<u32>(10 + i)});
        }
    }

    auto check_unregistration() -> void {
        DomainRegistry registry;
        DomainEndpoint root_slot;
        DomainEndpoint slot_a;
        DomainEndpoint slot_b;
        auto const ready =
            registry.register_domain(DomainId::Root, span(TWO), CHIP, root_slot) == Status::Ok &&
            registry.register_domain(DomainId{1}, span(FOUR), CHIP, slot_a) == Status::Ok &&
            registry.register_domain(DomainId{2}, span(FOUR), CHIP, slot_b) == Status::Ok;
        EXPECT_TRUE(ready, "three domains, Root among them");
        if (!ready) {
            return;
        }

        EXPECT_TRUE(registry.unregister_domain(DomainId{7}) == Status::NotFound,
                    "an unknown id is NotFound");
        EXPECT_TRUE(registry.unregister_domain(DomainId::Root) == Status::NotSupported,
                    "Root is NotSupported");
        EXPECT_TRUE(find_in(registry, DomainId::Root) != nullptr, "and stays registered");

        auto const before = virq_in(registry, DomainId{2}, 0);
        EXPECT_TRUE(registry.unregister_domain(DomainId{2}) == Status::Ok, "a domain is removed");
        EXPECT_TRUE(find_in(registry, DomainId{2}) == nullptr &&
                        owner_of(registry, before) == nullptr &&
                        virq_in(registry, DomainId{2}, 0) == VIrqNum::Invalid &&
                        slot_holds(slot_b, nullptr),
                    "its lookups and its slot find nothing");
        EXPECT_TRUE(find_in(registry, DomainId{1}) != nullptr && owner_of(registry, 32) != nullptr,
                    "the others are untouched");
        EXPECT_TRUE(registry.unregister_domain(DomainId{2}) == Status::NotFound,
                    "removing it again is NotFound");

        EXPECT_TRUE(registry.register_domain(DomainId{2}, span(FOUR), CHIP, slot_b) == Status::Ok,
                    "its id is registered again");
        auto const after = virq_in(registry, DomainId{2}, 0);
        EXPECT_TRUE(after != before &&
                        after >= static_cast<VIrqNum>(static_cast<u32>(before) + FOUR.size()) &&
                        owner_of(registry, before) == nullptr,
                    "in a block above everything given out; the dead block stays dead");
        drop(registry, DomainId{1});
        drop(registry, DomainId{2});

        // Over and over: the place and the id come back every time, and the
        // blocks only go up.
        DomainEndpoint slot_c;
        auto rose = true;
        auto each_ok = true;
        auto last = VIrqNum{0};
        for (u32 round = 0; round < 3 * DomainRegistry::CAPACITY; ++round) {
            each_ok &= registry.register_domain(DomainId{5}, span(TWO), CHIP, slot_c) == Status::Ok;
            auto const first = virq_in(registry, DomainId{5}, 0);
            rose &= first != VIrqNum::Invalid && first > last;
            last = first;
            each_ok &= registry.unregister_domain(DomainId{5}) == Status::Ok;
        }
        EXPECT_TRUE(each_ok && rose,
                    "one id registered and removed more often than the registry has places, each "
                    "time in a higher block");
    }

    /// Hardware numbers with holes, as a controller with pins missing has.
    auto check_sparse() -> void {
        constexpr std::array<HIrqNum, 3> SPARSE{HIrqNum{0}, HIrqNum{5}, HIrqNum{9}};
        constexpr std::array<HIrqNum, 2> FAR{HIrqNum{0}, HIrqNum{40}};
        DomainRegistry registry;
        DomainEndpoint slot_a;
        DomainEndpoint slot_b;
        DomainEndpoint slot_c;
        auto const ready =
            registry.register_domain(DomainId{1}, span(SPARSE), CHIP, slot_a) == Status::Ok;
        EXPECT_TRUE(ready, "a domain over hardware numbers 0, 5 and 9");
        if (!ready) {
            return;
        }
        auto const first = virq_in(registry, DomainId{1}, 0);
        EXPECT_TRUE(virq_in(registry, DomainId{1}, 5) ==
                            static_cast<VIrqNum>(static_cast<u32>(first) + 1) &&
                        virq_in(registry, DomainId{1}, 9) ==
                            static_cast<VIrqNum>(static_cast<u32>(first) + 2) &&
                        virq_in(registry, DomainId{1}, 3) == VIrqNum::Invalid &&
                        owner_of(registry, static_cast<VIrqNum>(static_cast<u32>(first) + 3)) ==
                            nullptr,
                    "has consecutive virqs for its pins, none for the pins it lacks, and none "
                    "past them");

        EXPECT_TRUE(registry.unregister_domain(DomainId{1}) == Status::Ok, "and is removed");
        EXPECT_TRUE(registry.register_domain(DomainId{2}, span(FAR), CHIP, slot_b) == Status::Ok,
                    "another domain with a far pin is registered after it");
        auto const second = virq_in(registry, DomainId{2}, 0);
        EXPECT_TRUE(second >= static_cast<VIrqNum>(static_cast<u32>(first) + SPARSE.size()) &&
                        owner_of(registry, first) == nullptr &&
                        owner_of(registry, second) != nullptr,
                    "in a block above the dead one, which still finds no table");
        EXPECT_TRUE(registry.register_domain(DomainId{3}, span(SPARSE), CHIP, slot_c) == Status::Ok,
                    "and a third like the first");
        EXPECT_TRUE(virq_in(registry, DomainId{3}, 0) >=
                        static_cast<VIrqNum>(static_cast<u32>(second) + 2),
                    "above the second's block again");
        drop(registry, DomainId{2});
        drop(registry, DomainId{3});
    }

    auto check_removal_refused() -> void {
        DomainRegistry registry;
        DomainEndpoint hookless_slot;

        // A Permanent line: the domain, its registry, its slot and its owner
        // are leaked, one set per run, as the action can never come off.
        auto *const permanent =
            new (mm::ALLOC_KERNEL) Hooked(irq::RegistrationOptions::Permanent, &hooked_cancel);
        auto *const permanent_registry = new (mm::ALLOC_KERNEL) DomainRegistry();
        auto *const leaked_slot = new (mm::ALLOC_KERNEL) DomainEndpoint();
        if (permanent == nullptr || permanent_registry == nullptr || leaked_slot == nullptr) {
            FAIL("allocate the Permanent domain");
        }
        auto const registered =
            permanent_registry->register_domain(DomainId{1}, span(TWO), CHIP, *leaked_slot);
        auto *const table = find_in(*permanent_registry, DomainId{1});
        auto const virq0 = virq_in(*permanent_registry, DomainId{1}, 0);
        auto const virq1 = virq_in(*permanent_registry, DomainId{1}, 1);
        auto attached = Status::NotFound;
        if (table != nullptr) {
            attached = irq::with_line(*table, virq1, [&](irq::Line &resolved) {
                return resolved.attach(permanent->action);
            });
        }
        EXPECT_TRUE(registered == Status::Ok && attached == Status::Ok,
                    "a domain with a Permanent action on one line");
        EXPECT_TRUE(permanent_registry->unregister_domain(DomainId{1}) == Status::AlreadyBound,
                    "is not removed");
        Hooked probe(irq::RegistrationOptions::None, &hooked_cancel);
        EXPECT_TRUE(find_in(*permanent_registry, DomainId{1}) == table &&
                        permanent->told.load() == 0 &&
                        irq::with_line(*table, virq0,
                                       [&](irq::Line &resolved) { return resolved.unmask(); }) ==
                            Status::Ok &&
                        irq::with_line(*table, virq0,
                                       [&](irq::Line &resolved) {
                                           return resolved.attach(probe.action);
                                       }) == Status::Ok,
                    "and works as before: no mark is left on its other line, and nothing was told");
        auto _ = table->detach(probe.action); // Clean-up; the check above is the result.

        // An action with no cancel hook.
        Hooked hookless(irq::RegistrationOptions::None, nullptr);
        Hooked hooked(irq::RegistrationOptions::None, &hooked_cancel);
        auto const ok =
            registry.register_domain(DomainId{1}, span(TWO), CHIP, hookless_slot) == Status::Ok;
        auto *const second = find_in(registry, DomainId{1});
        auto const v0 = virq_in(registry, DomainId{1}, 0);
        auto const v1 = virq_in(registry, DomainId{1}, 1);
        auto const both = ok &&
                          irq::with_line(*second, v0,
                                         [&](irq::Line &resolved) {
                                             return resolved.attach(hooked.action);
                                         }) == Status::Ok &&
                          irq::with_line(*second, v1, [&](irq::Line &resolved) {
                              return resolved.attach(hookless.action);
                          }) == Status::Ok;
        EXPECT_TRUE(both, "a domain with an action with a hook and one without");
        EXPECT_TRUE(registry.unregister_domain(DomainId{1}) == Status::AlreadyBound,
                    "is not removed either");
        EXPECT_TRUE(hooked.told.load() == 0 && hooked.action.is_attached() &&
                        irq::with_line(*second, v0,
                                       [&](irq::Line &resolved) { return resolved.unmask(); }) ==
                            Status::Ok,
                    "its hooked action is left on its line, untold, and the line unmasks");
        auto const dropped = second->detach(hookless.action);
        EXPECT_TRUE(registry.unregister_domain(DomainId{1}) == Status::Ok,
                    "once the hookless action is gone it is removed");
        EXPECT_TRUE(dropped == Status::Ok && hooked.told.load() == 1 &&
                        !hooked.action.is_attached(),
                    "and the hooked action is told, once, and is off its line");
    }

    /// A removal tells every action, on every line, and one that refuses on
    /// the last line refuses all.
    auto check_removal_tells_every_line() -> void {
        DomainRegistry registry;
        DomainEndpoint slot;
        Hooked one(irq::RegistrationOptions::None, &hooked_cancel);
        Hooked two(irq::RegistrationOptions::None, &hooked_cancel);
        Hooked three(irq::RegistrationOptions::None, &hooked_cancel);
        Hooked four(irq::RegistrationOptions::None, nullptr);
        Hooked *const all[] = {&one, &two, &three, &four};

        auto ready = registry.register_domain(DomainId{1}, span(FOUR), CHIP, slot) == Status::Ok;
        auto *const table = find_in(registry, DomainId{1});
        ready &= table != nullptr;
        for (usize pin = 0; ready && pin < FOUR.size(); ++pin) {
            ready &= irq::with_line(*table, virq_in(registry, DomainId{1}, static_cast<u32>(pin)),
                                    [&](irq::Line &resolved) {
                                        return resolved.attach(all[pin]->action);
                                    }) == Status::Ok;
        }
        EXPECT_TRUE(ready, "an action on each of four lines, the last without a hook");
        if (!ready) {
            return;
        }
        EXPECT_TRUE(registry.unregister_domain(DomainId{1}) == Status::AlreadyBound,
                    "the one without refuses the removal of all four lines");
        EXPECT_TRUE(one.told.load() + two.told.load() + three.told.load() == 0 &&
                        one.action.is_attached() && two.action.is_attached() &&
                        three.action.is_attached() && four.action.is_attached(),
                    "and nothing was told or taken off");
        auto const dropped = table->detach(four.action);
        EXPECT_TRUE(registry.unregister_domain(DomainId{1}) == Status::Ok,
                    "without it the domain is removed");
        EXPECT_TRUE(dropped == Status::Ok && one.told.load() == 1 && two.told.load() == 1 &&
                        three.told.load() == 1 && !one.action.is_attached() &&
                        !two.action.is_attached() && !three.action.is_attached(),
                    "every action on every other line is told once and taken off");
    }

    /// A domain being removed, its readers' grace period still running: its id
    /// is taken, the registry shows it no more, and a second remover is
    /// turned away. A reader on one CPU holds the removal in the grace period,
    /// the remover is a thread of its own, and the test thread looks.
    struct Hold {
        std::atomic<bool> in_section{false};
        std::atomic<bool> release{false};
        std::atomic<bool> removed{false};
        std::atomic<Status> status{Status::BadState};
        DomainRegistry *registry = nullptr;
    };
    Hold HOLD;

    auto hold_reader(void *) -> i32 {
        IrqRcuReadGuard const guard;
        HOLD.in_section.store(true, std::memory_order_release);
        auto const give_up = time::SteadyClock::now() + Seconds(2);
        while (!HOLD.release.load(std::memory_order_acquire) &&
               time::SteadyClock::now() < give_up) {
            arch::spin_hint();
        }
        return 0;
    }

    auto hold_remover(void *) -> i32 {
        HOLD.status.store(HOLD.registry->unregister_domain(DomainId{1}), std::memory_order_relaxed);
        HOLD.removed.store(true, std::memory_order_release);
        return 0;
    }

    auto spawn_on(char const *const name, Thread::StartRoutine const routine, CpuNum const cpu)
        -> Thread * {
        auto *const thread = Thread::spawn(name, routine, nullptr, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        auto _ = thread->resume();
        return thread;
    }

    auto check_removing(CpuNum const reader_cpu, CpuNum const remover_cpu) -> void {
        auto *const registry = new (mm::ALLOC_KERNEL) DomainRegistry();
        auto *const slot = new (mm::ALLOC_KERNEL) DomainEndpoint();
        auto *const spare = new (mm::ALLOC_KERNEL) DomainEndpoint();
        if (registry == nullptr || slot == nullptr || spare == nullptr ||
            registry->register_domain(DomainId{1}, span(TWO), CHIP, *slot) != Status::Ok) {
            FAIL("make the domain being removed");
        }
        HOLD.in_section.store(false, std::memory_order_relaxed);
        HOLD.release.store(false, std::memory_order_relaxed);
        HOLD.removed.store(false, std::memory_order_relaxed);
        HOLD.status.store(Status::BadState, std::memory_order_relaxed);
        HOLD.registry = registry;

        auto *const reader = spawn_on("irqregistry.test/hold", hold_reader, reader_cpu);
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!HOLD.in_section.load(std::memory_order_acquire) &&
               time::SteadyClock::now() < give_up) {
            auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
        }
        auto *const remover = spawn_on("irqregistry.test/remove", hold_remover, remover_cpu);

        // The registry shows the domain no more once the remover has
        // unpublished it, and it then waits for the reader.
        auto gone = false;
        while (time::SteadyClock::now() < give_up) {
            if (find_in(*registry, DomainId{1}) == nullptr) {
                gone = true;
                break;
            }
            auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
        }
        auto const waiting = gone && !HOLD.removed.load(std::memory_order_acquire);
        EXPECT_TRUE(waiting,
                    "a domain whose readers are being waited for is no longer found, and its "
                    "removal has not returned");
        EXPECT_TRUE(registry->register_domain(DomainId{1}, span(TWO), CHIP, *spare) ==
                        Status::AlreadyExists,
                    "its id cannot be registered meanwhile");
        EXPECT_TRUE(registry->unregister_domain(DomainId{1}) == Status::BadState,
                    "nor removed a second time");

        HOLD.release.store(true, std::memory_order_release);
        auto const joined = join_within(reader) && join_within(remover);
        EXPECT_TRUE(joined && HOLD.status.load() == Status::Ok,
                    "the removal ends Ok once the reader has left");
        EXPECT_TRUE(registry->register_domain(DomainId{1}, span(TWO), CHIP, *spare) == Status::Ok,
                    "and then the id is registered again");
        drop(*registry, DomainId{1});
    }

    /// Two writers at once, each registering and removing an id of its own:
    /// no registration is refused for an id nobody holds, and no two blocks of
    /// virqs ever given out overlap.
    constexpr u32 WRITER_ROUNDS = 150;
    struct Writer {
        DomainRegistry *registry = nullptr;
        DomainId id = DomainId::Invalid;
        DomainEndpoint slot;
        std::atomic<u32> failures{0};
        VIrqNum firsts[WRITER_ROUNDS]{};
    };
    Writer WRITERS[2];

    auto writer_routine(void *const arg) -> i32 {
        auto &writer = *static_cast<Writer *>(arg);
        for (u32 round = 0; round < WRITER_ROUNDS; ++round) {
            if (writer.registry->register_domain(writer.id, span(TWO), CHIP, writer.slot) !=
                Status::Ok) {
                writer.failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            writer.firsts[round] = virq_in(*writer.registry, writer.id, 0);
            if (writer.registry->unregister_domain(writer.id) != Status::Ok) {
                writer.failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return 0;
    }

    auto check_two_writers(CpuNum const first_cpu, CpuNum const second_cpu) -> void {
        auto *const registry = new (mm::ALLOC_KERNEL) DomainRegistry();
        if (registry == nullptr) {
            FAIL("make the registry of the two writers");
        }
        for (usize i = 0; i < 2; ++i) {
            WRITERS[i].registry = registry;
            WRITERS[i].id = DomainId{static_cast<u32>(20 + i)};
            WRITERS[i].failures.store(0, std::memory_order_relaxed);
        }
        auto *const a = Thread::spawn("irqregistry.test/a", writer_routine, &WRITERS[0],
                                      task::DEFAULT_PRIORITY);
        auto *const b = Thread::spawn("irqregistry.test/b", writer_routine, &WRITERS[1],
                                      task::DEFAULT_PRIORITY);
        if (a == nullptr || b == nullptr) {
            FAIL("start the two writers");
        }
        a->set_cpu_affinity_hard(CpuMask::of(first_cpu));
        b->set_cpu_affinity_hard(CpuMask::of(second_cpu));
        auto _ = a->resume();
        auto _ = b->resume();
        auto const joined = join_within(a) && join_within(b);

        // Every block ever given out, of both writers: no two overlap, a block
        // being two virqs.
        auto overlaps = u32{0};
        for (u32 i = 0; i < 2 * WRITER_ROUNDS; ++i) {
            for (u32 j = i + 1; j < 2 * WRITER_ROUNDS; ++j) {
                auto const x = WRITERS[i / WRITER_ROUNDS].firsts[i % WRITER_ROUNDS];
                auto const y = WRITERS[j / WRITER_ROUNDS].firsts[j % WRITER_ROUNDS];
                if (x == VIrqNum{0} || y == VIrqNum{0}) {
                    continue;
                }
                if (x < static_cast<VIrqNum>(static_cast<u32>(y) + 2) &&
                    y < static_cast<VIrqNum>(static_cast<u32>(x) + 2)) {
                    overlaps += 1;
                }
            }
        }
        EXPECT_TRUE(joined, "the two writers end");
        EXPECT_EQ(WRITERS[0].failures.load() + WRITERS[1].failures.load(), u32{0},
                  "each registered and removed its id every round, none refused for an id nobody "
                  "held");
        EXPECT_EQ(overlaps, u32{0}, "and no two blocks given out overlap");
        EXPECT_TRUE(!find_in(*registry, WRITERS[0].id) && !find_in(*registry, WRITERS[1].id),
                    "nothing is left registered");
    }

    /// The stress case: what the readers look at is whichever domain is
    /// registered, found by its virq. A reader notes the table's block as it
    /// finds it and looks at it again at the end; a table freed under it, or
    /// freed and made again, has another.
    struct Stress {
        DomainRegistry registry;
        DomainEndpoint slot;
        std::atomic<VIrqNum> probe{VIrqNum::Invalid};
        std::atomic<bool> stop{false};
        std::atomic<u64> seen[MAX_CPU]{};
        /// Looks that began with the table registered and ended with it
        /// unpublished: the removal happened under the reader.
        std::atomic<u64> overlapped[MAX_CPU]{};
        std::atomic<u64> broken[MAX_CPU]{};
    };
    Stress *STRESS = nullptr;

    auto look_once(CpuNum const cpu) -> bool {
        IrqRcuReadGuard const guard;
        auto const virq = STRESS->probe.load(std::memory_order_acquire);
        auto *const table = STRESS->registry.domain_for(virq, guard);
        if (table == nullptr) {
            return false;
        }
        auto const first = table->first_id();
        STRESS->seen[cpu].fetch_add(1, std::memory_order_relaxed);
        for (u32 i = 0; i < LOOK_FOR; ++i) {
            // The slot a root handler reads, when it holds a table, holds this
            // one: the removal retracts it before the grace period, never
            // after, and a new domain's cannot be published under this guard.
            auto *const from_slot = DomainRegistry::published_domain(STRESS->slot, guard);
            if (table->is_destroyed() || table->first_id() != first ||
                table->source_for_irq(virq) == HIrqNum::Invalid ||
                (from_slot != nullptr && from_slot != table)) {
                STRESS->broken[cpu].fetch_add(1, std::memory_order_relaxed);
                break;
            }
            arch::spin_hint();
        }
        if (STRESS->registry.find(DomainId{3}, guard) == nullptr) {
            STRESS->overlapped[cpu].fetch_add(1, std::memory_order_relaxed);
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

    auto check_stress(CpuMask const &reader_cpus) -> void {
        STRESS = new (mm::ALLOC_KERNEL) Stress();
        if (STRESS == nullptr) {
            FAIL("allocate the stress case");
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

        auto &slot = STRESS->slot;
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
        auto overlapped = u64{0};
        auto broken = u64{0};
        reader_cpus.for_each([&](CpuNum const cpu) {
            joined &= join_within(readers[cpu]);
            auto const saw = STRESS->seen[cpu].load(std::memory_order_relaxed);
            every_reader_saw &= saw > 0;
            seen += saw;
            overlapped += STRESS->overlapped[cpu].load(std::memory_order_relaxed);
            broken += STRESS->broken[cpu].load(std::memory_order_relaxed);
        });
        log::info("irqregistry: {} domains registered and removed, {} readers saw a table "
                  "{} times, the removal came under {} of them, {} broken",
                  removed, reader_cpus.count(), seen, overlapped, broken);
        EXPECT_TRUE(registered == STRESS_ROUNDS && removed == STRESS_ROUNDS,
                    "every round registered and removed the domain");
        EXPECT_TRUE(joined, "the readers start and end");
        EXPECT_TRUE(every_reader_saw && seen >= STRESS_ROUNDS / 4,
                    "every reader looked at a table, a quarter as many times as there were rounds, "
                    "in all");
        EXPECT_GE(overlapped, STRESS_ROUNDS / 10,
                  "and the removal came under readers in at least a tenth as many looks");
        EXPECT_EQ(broken, u64{0}, "and none found its table gone or replaced before it ended");
        // Readers that did not end keep the object they read: it is leaked.
        if (joined && registered == removed) {
            delete STRESS;
            STRESS = nullptr;
        }
    }

    KTEST(IrqRegistry, DomainsHoldDisjointBlocksAndRemovalWaitsForReaders) {
        check_public_endpoints();
        check_registration();
        check_registration_refused();
        check_unregistration();
        check_sparse();
        check_removal_refused();
        check_removal_tells_every_line();

        auto const online = Cpu::online_mask();
        if (online.count() < 2) {
            log::info("irqregistry: one CPU only, the stress is skipped");
            return;
        }
        auto writer_cpu = INVALID_CPU_NUM;
        online.for_each([&writer_cpu](CpuNum const cpu) {
            if (writer_cpu == INVALID_CPU_NUM) {
                writer_cpu = cpu;
            }
        });
        auto readers = online;
        readers.reset(writer_cpu);

        // The writer's CPU for the test thread: a reader holds its CPU with
        // interrupts off, and the writer waits for it.
        auto *const self = Thread::Current::get();
        self->set_cpu_affinity_hard(CpuMask::of(writer_cpu));
        auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
        if (Cpu::id() != writer_cpu) {
            self->set_cpu_affinity_hard(cpu_possible_mask());
            FAIL("the test thread did not move to CPU {}", writer_cpu);
        }
        check_stress(readers);
        if (online.count() >= 3) {
            CpuNum others[2]{};
            auto found = usize{0};
            readers.for_each([&](CpuNum const cpu) {
                if (found < 2) {
                    others[found] = cpu;
                    found += 1;
                }
            });
            check_removing(others[0], others[1]);
            check_two_writers(others[0], others[1]);
        } else {
            log::info("irqregistry: fewer than three CPUs, the removal held in its grace "
                      "period and the two writers are skipped");
        }
        self->set_cpu_affinity_hard(cpu_possible_mask());
    }
} // namespace
} // namespace ours
