/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `irqdomain.test`: irq::Domain on layouts of hardware numbers this machine
// does not have (.agents/tasks/interrupt-system/PLAN.md, step I3). dbg image
// only: it is the one with a kernel shell.
//
// pinlines.test shows the domain of the machine it runs on, whose GSIs are 0
// to 23 in one run. A bug that needs a hole, a second run of pins, a
// hardware number at a multiple of 32, or a list the domain refuses passes it
// there. The domain is built here from lists of the test's own:
//   - one number; numbers with holes, given out of order: the virqs follow
//     the order of the hardware numbers, not of the list;
//   - the highest hardware number at 32, which moves the first virq to 64;
//     two runs of pins, as two IO-APICs would give, listed last run first;
//     the layout of this machine, as the baseline;
//   - in every one, a number the domain does not hold answers INVALID both
//     ways, around each end and far away;
//   - refused: an empty list and INVALID_H_IRQ_NUM (InvArgs), a number given
//     twice (AlreadyExists), a list the virq space cannot hold (InvArgs). A
//     refusal keeps nothing: the same domain takes a good list after it. A
//     built domain refuses a second build (BadState) and keeps its map.
//
// A console command rather than a unit test: Domain allocates from the
// kernel's heap.
#include <ours/irq/domain.hpp>

#include <algorithm>
#include <array>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/views/span.hpp>

namespace ours {
namespace {
    using irq::Domain;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("irqdomain.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("irqdomain.test: FAIL  {}", name);
    }

    auto answered(Tally &tally, Status const got, Status const want, char const *const name)
        -> void {
        if (got != want) {
            log::error("irqdomain.test:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        verdict(tally, got == want, name);
    }

    /// Builds a domain from |list| and checks it against the rule written in
    /// domain.hpp: consecutive virqs from |first_virq|, in the order of the
    /// hardware numbers, and INVALID for every number the list does not hold.
    auto check_layout(Tally &tally, char const *const name,
                      ustl::views::Span<HIrqNum const> const list, VIrqNum const first_virq)
        -> void {
        Domain domain;
        auto const built = domain.init(list);
        if (built != Status::Ok) {
            log::error("irqdomain.test:   {}: init answered {}", name, to_string(built));
            verdict(tally, false, name);
            return;
        }

        auto highest = HIrqNum{0};
        for (auto const hwirq: list) {
            highest = std::max(highest, hwirq);
        }
        auto const count = list.size();

        // A listed number's rank is how many listed numbers are below it.
        auto wrong = usize{0};
        for (auto const hwirq: list) {
            auto const rank = static_cast<VIrqNum>(std::ranges::count_if(
                list, [hwirq](HIrqNum const other) -> bool { return other < hwirq; }));
            auto const virq = first_virq + rank;
            if (domain.virq_of(hwirq) != virq || domain.hwirq_of(virq) != hwirq) {
                wrong += 1;
                log::error("irqdomain.test:   {}: hwirq {} -> virq {} (want {}), back {}", name,
                           hwirq, domain.virq_of(hwirq), virq, domain.hwirq_of(virq));
            }
        }
        verdict(tally, wrong == 0, name);

        // Every number around and past the list: held exactly when it is in it.
        auto held_wrong = usize{0};
        for (auto hwirq = HIrqNum{0}; hwirq <= highest + 40; ++hwirq) {
            auto const in_list = std::ranges::find(list, hwirq) != list.end();
            if ((domain.virq_of(hwirq) != irq::INVALID_V_IRQ_NUM) != in_list) {
                held_wrong += 1;
                log::error("irqdomain.test:   {}: hwirq {} held={} but listed={}", name, hwirq,
                           domain.virq_of(hwirq) != irq::INVALID_V_IRQ_NUM, in_list);
            }
        }
        verdict(tally, held_wrong == 0, "a hardware number is held exactly when it is listed");

        auto virq_wrong = usize{0};
        auto const last_virq = static_cast<VIrqNum>(first_virq + count - 1);
        for (auto virq = VIrqNum{0}; virq <= last_virq + 40; ++virq) {
            auto const is_virq = virq >= first_virq && virq <= last_virq;
            if ((domain.hwirq_of(virq) != irq::INVALID_H_IRQ_NUM) != is_virq) {
                virq_wrong += 1;
                log::error("irqdomain.test:   {}: virq {} is_virq={} but hwirq {}", name, virq,
                           is_virq, domain.hwirq_of(virq));
            }
        }
        verdict(tally, virq_wrong == 0, "a virq is held exactly when it is one of the domain's");
        verdict(tally, first_virq > highest, "and every virq is past every hardware number");
        verdict(tally,
                domain.virq_of(0xFFFF'FFFE) == irq::INVALID_V_IRQ_NUM &&
                    domain.hwirq_of(irq::INVALID_V_IRQ_NUM) == irq::INVALID_H_IRQ_NUM &&
                    domain.virq_of(irq::INVALID_H_IRQ_NUM) == irq::INVALID_V_IRQ_NUM,
                "a far number and the invalid ones are held by no layout");
        verdict(tally, domain.hwirq_limit() == static_cast<usize>(highest) + 1,
                "the table size to index by hardware number is one past the highest");

        domain.reset();
    }

    auto check_layouts(Tally &tally) -> void {
        constexpr auto ONE = std::array<HIrqNum, 1>{5};
        check_layout(tally, "one number, 5", ustl::views::Span<HIrqNum const>(ONE), 32);

        constexpr auto HOLES = std::array<HIrqNum, 3>{7, 0, 2};
        check_layout(tally, "numbers 7, 0, 2 with holes, out of order",
                     ustl::views::Span<HIrqNum const>(HOLES), 32);

        constexpr auto AT_32 = std::array<HIrqNum, 2>{0, 32};
        check_layout(tally, "the highest number at 32 moves the first virq to 64",
                     ustl::views::Span<HIrqNum const>(AT_32), 64);

        std::array<HIrqNum, 48> two_runs{};
        for (auto i = usize{0}; i < 24; ++i) {
            two_runs[i] = static_cast<HIrqNum>(24 + i);
            two_runs[24 + i] = static_cast<HIrqNum>(i);
        }
        check_layout(tally, "pins 24 to 47 listed before pins 0 to 23",
                     ustl::views::Span<HIrqNum const>(two_runs), 64);

        std::array<HIrqNum, 24> machine{};
        for (auto i = usize{0}; i < machine.size(); ++i) {
            machine[i] = static_cast<HIrqNum>(i);
        }
        check_layout(tally, "pins 0 to 23, this machine's layout",
                     ustl::views::Span<HIrqNum const>(machine), 32);
    }

    auto check_refusals(Tally &tally) -> void {
        constexpr auto NONE = std::array<HIrqNum, 0>{};
        constexpr auto WITH_INVALID = std::array<HIrqNum, 2>{1, irq::INVALID_H_IRQ_NUM};
        constexpr auto TWICE = std::array<HIrqNum, 2>{3, 3};
        constexpr auto TOO_BIG = std::array<HIrqNum, 1>{0xFFFF'FFFE};
        constexpr auto GOOD = std::array<HIrqNum, 1>{3};
        constexpr auto OTHER = std::array<HIrqNum, 1>{9};

        {
            Domain domain;
            answered(tally, domain.init(ustl::views::Span<HIrqNum const>(NONE)), Status::InvArgs,
                     "an empty list is refused");
            answered(tally, domain.init(ustl::views::Span<HIrqNum const>(WITH_INVALID)),
                     Status::InvArgs, "INVALID_H_IRQ_NUM in the list is refused");
            answered(tally, domain.init(ustl::views::Span<HIrqNum const>(TOO_BIG)), Status::InvArgs,
                     "a list the virq space cannot hold is refused");
        }
        {
            Domain domain;
            answered(tally, domain.init(ustl::views::Span<HIrqNum const>(TWICE)),
                     Status::AlreadyExists, "a number given twice is refused");
            verdict(tally, domain.virq_of(3) == irq::INVALID_V_IRQ_NUM && domain.hwirq_limit() == 0,
                    "and the refusal keeps nothing");
            answered(tally, domain.init(ustl::views::Span<HIrqNum const>(GOOD)), Status::Ok,
                     "so the same domain takes a good list after it");
            verdict(tally, domain.virq_of(3) == 32 && domain.hwirq_of(32) == 3, "and maps it");
            answered(tally, domain.init(ustl::views::Span<HIrqNum const>(OTHER)), Status::BadState,
                     "a built domain refuses a second build");
            verdict(tally,
                    domain.virq_of(3) == 32 && domain.hwirq_of(32) == 3 &&
                        domain.virq_of(9) == irq::INVALID_V_IRQ_NUM,
                    "and keeps its map");
            domain.reset();
        }
    }

    auto irq_domain_test_command(int, char **) -> int {
        Tally tally;

        check_layouts(tally);
        check_refusals(tally);

        if (tally.failed != 0) {
            log::error("irqdomain.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("irqdomain.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(IrqDomainTest, "irqdomain.test", ours::irq_domain_test_command,
             "build irq::Domain from layouts of hardware numbers this machine does not have, "
             "and from lists it must refuse");
