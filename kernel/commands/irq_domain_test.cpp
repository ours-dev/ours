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
//   - a floor under the first virq (the first free one once another domain
//     has taken the numbers below it): above the domain's own start, below
//     it, at a multiple of 32 and between two, and at the end of the number
//     space;
//   - in every one, a number the domain does not hold answers INVALID both
//     ways, around each end and far away;
//   - refused: an empty list and HardwareIrq::Invalid (InvArgs), a number given
//     twice (AlreadyExists), a list the virq space cannot hold (InvArgs), also
//     from a floor too high for it. A refusal keeps nothing: the same domain takes a good list after it. A
//     built domain refuses a second build (BadState) and keeps its map.
//
// A console command rather than a unit test: Domain allocates from the
// kernel's heap.
#include <ours/irq/controller.hpp>

#include <algorithm>
#include <array>
#include <type_traits>

#include <gktl/command.hpp>
#include <kernel/kernel/irq/domain.hpp>
#include <logz4/log.hpp>
#include <ustl/views/span.hpp>

namespace ours {
namespace {
    using irq::Domain;

    static_assert(!std::is_copy_constructible_v<Domain>);
    static_assert(!std::is_move_constructible_v<Domain>);
    static_assert(!std::is_convertible_v<HardwareIrq, IrqId>);
    static_assert(!std::is_convertible_v<IrqId, HardwareIrq>);
    static_assert(!std::is_convertible_v<u32, IrqId>);
    static_assert(!std::is_convertible_v<u32, HardwareIrq>);
    static_assert(!std::is_convertible_v<IrqId, u32>);
    static_assert(!std::is_convertible_v<HardwareIrq, u32>);
    static_assert(sizeof(IrqId) == sizeof(u32));
    static_assert(sizeof(HardwareIrq) == sizeof(u32));

    constexpr irq::ControllerOps CONTROLLER = {
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask = [](irq::ControllerSource const &) -> void {},
        .issue_eoi = [](irq::ControllerSource const &) -> void {},
        .name = "irq-domain-test",
    };

    auto build_domain(Domain &domain, ustl::views::Span<HardwareIrq const> const sources,
                      u32 const minimum = 0) -> Status {
        return domain.init(sources, CONTROLLER, IrqId{minimum});
    }

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
    /// |min_first| is the floor given to init().
    auto check_layout(Tally &tally, char const *const name,
                      ustl::views::Span<HardwareIrq const> const list, u32 const first_virq,
                      u32 const min_first = 0) -> void {
        Domain domain;
        auto const built = build_domain(domain, list, min_first);
        if (built != Status::Ok) {
            log::error("irqdomain.test:   {}: init answered {}", name, to_string(built));
            verdict(tally, false, name);
            return;
        }

        auto highest = HardwareIrq{0};
        for (auto const hwirq: list) {
            highest = std::max(highest, hwirq);
        }
        auto const count = list.size();

        // A listed number's rank is how many listed numbers are below it.
        auto wrong = usize{0};
        for (auto const hwirq: list) {
            auto const rank = static_cast<u32>(std::ranges::count_if(
                list, [hwirq](HardwareIrq const other) -> bool { return other < hwirq; }));
            auto const virq = IrqId{first_virq + rank};
            if (domain.virq_of(hwirq) != virq || domain.hwirq_of(virq) != hwirq) {
                wrong += 1;
                log::error("irqdomain.test:   {}: hwirq {} -> virq {} (want {}), back {}", name,
                           static_cast<u32>(hwirq), static_cast<u32>(domain.virq_of(hwirq)),
                           static_cast<u32>(virq), static_cast<u32>(domain.hwirq_of(virq)));
            }
        }
        verdict(tally, wrong == 0, name);

        // Every number around and past the list: held exactly when it is in it.
        auto held_wrong = usize{0};
        for (u32 raw = 0; raw <= static_cast<u32>(highest) + 40; ++raw) {
            auto const hwirq = HardwareIrq{raw};
            auto const in_list = std::ranges::find(list, hwirq) != list.end();
            if ((domain.virq_of(hwirq) != IrqId::Invalid) != in_list) {
                held_wrong += 1;
                log::error("irqdomain.test:   {}: hwirq {} held={} but listed={}", name,
                           static_cast<u32>(hwirq), domain.virq_of(hwirq) != IrqId::Invalid,
                           in_list);
            }
        }
        verdict(tally, held_wrong == 0, "a hardware number is held exactly when it is listed");

        auto virq_wrong = usize{0};
        auto const last_virq = first_virq + count - 1;
        for (u32 raw = 0; raw <= last_virq + 40; ++raw) {
            auto const virq = IrqId{raw};
            auto const is_virq = raw >= first_virq && raw <= last_virq;
            if ((domain.hwirq_of(virq) != HardwareIrq::Invalid) != is_virq) {
                virq_wrong += 1;
                log::error("irqdomain.test:   {}: virq {} is_virq={} but hwirq {}", name,
                           static_cast<u32>(virq), is_virq,
                           static_cast<u32>(domain.hwirq_of(virq)));
            }
        }
        verdict(tally, virq_wrong == 0, "a virq is held exactly when it is one of the domain's");
        verdict(tally, first_virq > static_cast<u32>(highest),
                "and every virq is past every hardware number");
        verdict(tally,
                domain.virq_of(HardwareIrq{0xFFFF'FFFE}) == IrqId::Invalid &&
                    domain.hwirq_of(IrqId::Invalid) == HardwareIrq::Invalid &&
                    domain.virq_of(HardwareIrq::Invalid) == IrqId::Invalid,
                "a far number and the invalid ones are held by no layout");
        verdict(tally, domain.hwirq_limit() == static_cast<usize>(highest) + 1,
                "the table size to index by hardware number is one past the highest");
    }

    auto check_layouts(Tally &tally) -> void {
        constexpr auto ONE = std::array<HardwareIrq, 1>{HardwareIrq{5}};
        check_layout(tally, "one number, 5", ustl::views::Span<HardwareIrq const>(ONE), 32);

        constexpr auto HOLES =
            std::array<HardwareIrq, 3>{HardwareIrq{7}, HardwareIrq{0}, HardwareIrq{2}};
        check_layout(tally, "numbers 7, 0, 2 with holes, out of order",
                     ustl::views::Span<HardwareIrq const>(HOLES), 32);

        constexpr auto AT_32 = std::array<HardwareIrq, 2>{HardwareIrq{0}, HardwareIrq{32}};
        check_layout(tally, "the highest number at 32 moves the first virq to 64",
                     ustl::views::Span<HardwareIrq const>(AT_32), 64);

        std::array<HardwareIrq, 48> two_runs{};
        for (auto i = usize{0}; i < 24; ++i) {
            two_runs[i] = static_cast<HardwareIrq>(24 + i);
            two_runs[24 + i] = static_cast<HardwareIrq>(i);
        }
        check_layout(tally, "pins 24 to 47 listed before pins 0 to 23",
                     ustl::views::Span<HardwareIrq const>(two_runs), 64);

        std::array<HardwareIrq, 24> machine{};
        for (auto i = usize{0}; i < machine.size(); ++i) {
            machine[i] = static_cast<HardwareIrq>(i);
        }
        check_layout(tally, "pins 0 to 23, this machine's layout",
                     ustl::views::Span<HardwareIrq const>(machine), 32);

        // The floor: the first virq is the higher of the domain's own start
        // (32 for these pins) and the floor rounded up to a multiple of 32.
        auto const pins = ustl::views::Span<HardwareIrq const>(machine);
        check_layout(tally, "a floor above the domain's own start moves the first virq to it", pins,
                     128, 128);
        check_layout(tally, "a floor between two multiples of 32 is rounded up", pins, 96, 65);
        check_layout(tally, "a floor one past a multiple of 32 is rounded up", pins, 64, 33);
        check_layout(tally, "a floor at the domain's own start changes nothing", pins, 32, 32);
        check_layout(tally, "a floor below the domain's own start changes nothing", pins, 32, 10);
        check_layout(tally, "a floor of 0 is no floor", pins, 32, 0);
        check_layout(tally, "a floor above a start that a high number already raised",
                     ustl::views::Span<HardwareIrq const>(AT_32), 96, 70);
    }

    auto check_refusals(Tally &tally) -> void {
        constexpr auto NONE = std::array<HardwareIrq, 0>{};
        constexpr auto WITH_INVALID =
            std::array<HardwareIrq, 2>{HardwareIrq{1}, HardwareIrq::Invalid};
        constexpr auto TWICE = std::array<HardwareIrq, 2>{HardwareIrq{3}, HardwareIrq{3}};
        constexpr auto TOO_BIG = std::array<HardwareIrq, 1>{HardwareIrq{0xFFFF'FFFE}};
        constexpr auto GOOD = std::array<HardwareIrq, 1>{HardwareIrq{3}};
        constexpr auto OTHER = std::array<HardwareIrq, 1>{HardwareIrq{9}};
        auto const many = []() -> std::array<HardwareIrq, 32> {
            auto list = std::array<HardwareIrq, 32>{};
            for (auto i = usize{0}; i < list.size(); ++i) {
                list[i] = static_cast<HardwareIrq>(i);
            }
            return list;
        }();

        {
            Domain domain;
            answered(tally, build_domain(domain, ustl::views::Span<HardwareIrq const>(NONE)),
                     Status::InvArgs, "an empty list is refused");
            answered(tally,
                     build_domain(domain, ustl::views::Span<HardwareIrq const>(WITH_INVALID)),
                     Status::InvArgs, "HardwareIrq::Invalid in the list is refused");
            answered(tally, build_domain(domain, ustl::views::Span<HardwareIrq const>(TOO_BIG)),
                     Status::InvArgs, "a list the virq space cannot hold is refused");
            // IrqId::Invalid is the last number of the space and no virq.
            // The last multiple of 32 below it is u32{0xffff'ffe0}, and
            // the one line fits there; a floor past it rounds up out of the
            // space, and so does a list of 32 lines from it.
            answered(tally,
                     build_domain(domain, ustl::views::Span<HardwareIrq const>(GOOD),
                                  static_cast<u32>(IrqId::Invalid)),
                     Status::InvArgs, "a floor at IrqId::Invalid leaves no room and is refused");
            answered(
                tally,
                build_domain(domain, ustl::views::Span<HardwareIrq const>(GOOD), u32{0xffff'ffe1}),
                Status::InvArgs, "a floor that rounds up out of the space is refused");
            answered(
                tally,
                build_domain(domain, ustl::views::Span<HardwareIrq const>(many), u32{0xffff'ffe0}),
                Status::InvArgs, "a floor that leaves room for fewer virqs than lines is refused");
            verdict(tally,
                    domain.virq_of(HardwareIrq{3}) == IrqId::Invalid && domain.hwirq_limit() == 0,
                    "and none of the three keeps anything");
            answered(
                tally,
                build_domain(domain, ustl::views::Span<HardwareIrq const>(GOOD), u32{0xffff'ffe0}),
                Status::Ok, "the last multiple of 32 with room for the line is taken");
            verdict(tally, domain.virq_of(HardwareIrq{3}) == IrqId{0xffff'ffe0},
                    "and is the line's virq");
        }
        {
            Domain domain;
            answered(tally, build_domain(domain, ustl::views::Span<HardwareIrq const>(TWICE)),
                     Status::AlreadyExists, "a number given twice is refused");
            verdict(tally,
                    domain.virq_of(HardwareIrq{3}) == IrqId::Invalid && domain.hwirq_limit() == 0,
                    "and the refusal keeps nothing");
            answered(tally, build_domain(domain, ustl::views::Span<HardwareIrq const>(GOOD)),
                     Status::Ok, "so the same domain takes a good list after it");
            verdict(tally,
                    domain.virq_of(HardwareIrq{3}) == IrqId{32} &&
                        domain.hwirq_of(IrqId{32}) == HardwareIrq{3},
                    "and maps it");
            answered(tally, build_domain(domain, ustl::views::Span<HardwareIrq const>(OTHER)),
                     Status::BadState, "a built domain refuses a second build");
            verdict(tally,
                    domain.virq_of(HardwareIrq{3}) == IrqId{32} &&
                        domain.hwirq_of(IrqId{32}) == HardwareIrq{3} &&
                        domain.virq_of(HardwareIrq{9}) == IrqId::Invalid,
                    "and keeps its map");
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
