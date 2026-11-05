/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// IrqDomain.BuildsDomainsFromLayouts: irq::Domain on layouts of hardware
// numbers this machine does not have (.agents/tasks/interrupt-system/PLAN.md,
// step I3). dbg image only: it is the one with a kernel shell.
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
//   - refused: an empty list and HIrqNum::Invalid (InvArgs), a number given
//     twice (AlreadyExists), a list the virq space cannot hold (InvArgs), also
//     from a floor too high for it. A refusal keeps nothing: the same domain takes a good list after it. A
//     built domain refuses a second build (BadState) and keeps its map.
//
// Why an in-kernel test: Domain allocates from the kernel's heap.
#include <ours/irq/controller.hpp>

#include <algorithm>
#include <array>
#include <type_traits>

#include <kernel/kernel/irq/domain.hpp>
#include <ktest/ktest.hpp>
#include <ustl/views/span.hpp>

namespace ours {
namespace {
    using irq::Domain;

    static_assert(!std::is_copy_constructible_v<Domain>);
    static_assert(!std::is_move_constructible_v<Domain>);
    static_assert(!std::is_convertible_v<HIrqNum, VIrqNum>);
    static_assert(!std::is_convertible_v<VIrqNum, HIrqNum>);
    static_assert(!std::is_convertible_v<u32, VIrqNum>);
    static_assert(!std::is_convertible_v<u32, HIrqNum>);
    static_assert(!std::is_convertible_v<VIrqNum, u32>);
    static_assert(!std::is_convertible_v<HIrqNum, u32>);
    static_assert(sizeof(VIrqNum) == sizeof(u32));
    static_assert(sizeof(HIrqNum) == sizeof(u32));

    constexpr irq::ControllerOps CONTROLLER = {
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask = [](irq::ControllerSource const &) -> void {},
        .issue_eoi = [](irq::ControllerSource const &) -> void {},
    };

    auto build_domain(Domain &domain, ustl::views::Span<HIrqNum const> const sources,
                      u32 const minimum = 0) -> Status {
        return domain.init(sources, CONTROLLER, VIrqNum{minimum});
    }

    /// Builds a domain from |list| and checks it against the rule written in
    /// domain.hpp: consecutive virqs from |first_id|, in the order of the
    /// hardware numbers, and INVALID for every number the list does not hold.
    /// |min_first| is the floor given to init().
    auto check_layout(char const *const name, ustl::views::Span<HIrqNum const> const list,
                      u32 const first_id, u32 const min_first = 0) -> void {
        Domain domain;
        auto const built = build_domain(domain, list, min_first);
        if (built != Status::Ok) {
            FAIL("{}: init answered {}", name, to_string(built));
        }

        auto highest = HIrqNum{0};
        for (auto const hwirq: list) {
            highest = std::max(highest, hwirq);
        }
        auto const count = list.size();

        // A listed number's rank is how many listed numbers are below it.
        auto wrong = usize{0};
        for (auto const hwirq: list) {
            auto const rank = static_cast<u32>(std::ranges::count_if(
                list, [hwirq](HIrqNum const other) -> bool { return other < hwirq; }));
            auto const virq = VIrqNum{first_id + rank};
            if (domain.irq_for_source(hwirq) != virq || domain.source_for_irq(virq) != hwirq) {
                wrong += 1;
            }
        }
        EXPECT_EQ(wrong, usize{0}, "{}", name);

        // Every number around and past the list: held exactly when it is in it.
        auto held_wrong = usize{0};
        for (u32 raw = 0; raw <= static_cast<u32>(highest) + 40; ++raw) {
            auto const hwirq = HIrqNum{raw};
            auto const in_list = std::ranges::find(list, hwirq) != list.end();
            if ((domain.irq_for_source(hwirq) != VIrqNum::Invalid) != in_list) {
                held_wrong += 1;
            }
        }
        EXPECT_EQ(held_wrong, usize{0}, "a hardware number is held exactly when it is listed");

        auto virq_wrong = usize{0};
        auto const last_virq = first_id + count - 1;
        for (u32 raw = 0; raw <= last_virq + 40; ++raw) {
            auto const virq = VIrqNum{raw};
            auto const is_virq = raw >= first_id && raw <= last_virq;
            if ((domain.source_for_irq(virq) != HIrqNum::Invalid) != is_virq) {
                virq_wrong += 1;
            }
        }
        EXPECT_EQ(virq_wrong, usize{0}, "a virq is held exactly when it is one of the domain's");
        EXPECT_GT(first_id, static_cast<u32>(highest),
                  "and every virq is past every hardware number");
        EXPECT_TRUE(domain.irq_for_source(HIrqNum{0xFFFF'FFFE}) == VIrqNum::Invalid &&
                        domain.source_for_irq(VIrqNum::Invalid) == HIrqNum::Invalid &&
                        domain.irq_for_source(HIrqNum::Invalid) == VIrqNum::Invalid,
                    "a far number and the invalid ones are held by no layout");
        EXPECT_EQ(domain.source_limit(), static_cast<usize>(highest) + 1,
                  "the table size to index by hardware number is one past the highest");
    }

    auto check_layouts() -> void {
        constexpr auto ONE = std::array<HIrqNum, 1>{HIrqNum{5}};
        check_layout("one number, 5", ustl::views::Span<HIrqNum const>(ONE), 32);

        constexpr auto HOLES = std::array<HIrqNum, 3>{HIrqNum{7}, HIrqNum{0}, HIrqNum{2}};
        check_layout("numbers 7, 0, 2 with holes, out of order",
                     ustl::views::Span<HIrqNum const>(HOLES), 32);

        constexpr auto AT_32 = std::array<HIrqNum, 2>{HIrqNum{0}, HIrqNum{32}};
        check_layout("the highest number at 32 moves the first virq to 64",
                     ustl::views::Span<HIrqNum const>(AT_32), 64);

        std::array<HIrqNum, 48> two_runs{};
        for (auto i = usize{0}; i < 24; ++i) {
            two_runs[i] = static_cast<HIrqNum>(24 + i);
            two_runs[24 + i] = static_cast<HIrqNum>(i);
        }
        check_layout("pins 24 to 47 listed before pins 0 to 23",
                     ustl::views::Span<HIrqNum const>(two_runs), 64);

        std::array<HIrqNum, 24> machine{};
        for (auto i = usize{0}; i < machine.size(); ++i) {
            machine[i] = static_cast<HIrqNum>(i);
        }
        check_layout("pins 0 to 23, this machine's layout",
                     ustl::views::Span<HIrqNum const>(machine), 32);

        // The floor: the first virq is the higher of the domain's own start
        // (32 for these pins) and the floor rounded up to a multiple of 32.
        auto const pins = ustl::views::Span<HIrqNum const>(machine);
        check_layout("a floor above the domain's own start moves the first virq to it", pins, 128,
                     128);
        check_layout("a floor between two multiples of 32 is rounded up", pins, 96, 65);
        check_layout("a floor one past a multiple of 32 is rounded up", pins, 64, 33);
        check_layout("a floor at the domain's own start changes nothing", pins, 32, 32);
        check_layout("a floor below the domain's own start changes nothing", pins, 32, 10);
        check_layout("a floor of 0 is no floor", pins, 32, 0);
        check_layout("a floor above a start that a high number already raised",
                     ustl::views::Span<HIrqNum const>(AT_32), 96, 70);
    }

    auto check_refusals() -> void {
        constexpr auto NONE = std::array<HIrqNum, 0>{};
        constexpr auto WITH_INVALID = std::array<HIrqNum, 2>{HIrqNum{1}, HIrqNum::Invalid};
        constexpr auto TWICE = std::array<HIrqNum, 2>{HIrqNum{3}, HIrqNum{3}};
        constexpr auto TOO_BIG = std::array<HIrqNum, 1>{HIrqNum{0xFFFF'FFFE}};
        constexpr auto GOOD = std::array<HIrqNum, 1>{HIrqNum{3}};
        constexpr auto OTHER = std::array<HIrqNum, 1>{HIrqNum{9}};
        auto const many = []() -> std::array<HIrqNum, 32> {
            auto list = std::array<HIrqNum, 32>{};
            for (auto i = usize{0}; i < list.size(); ++i) {
                list[i] = static_cast<HIrqNum>(i);
            }
            return list;
        }();

        {
            Domain domain;
            EXPECT_EQ(build_domain(domain, ustl::views::Span<HIrqNum const>(NONE)), Status::InvArgs,
                      "an empty list is refused");
            EXPECT_EQ(build_domain(domain, ustl::views::Span<HIrqNum const>(WITH_INVALID)),
                      Status::InvArgs, "HIrqNum::Invalid in the list is refused");
            EXPECT_EQ(build_domain(domain, ustl::views::Span<HIrqNum const>(TOO_BIG)),
                      Status::InvArgs, "a list the virq space cannot hold is refused");
            // VIrqNum::Invalid is the last number of the space and no virq.
            // The last multiple of 32 below it is u32{0xffff'ffe0}, and
            // the one line fits there; a floor past it rounds up out of the
            // space, and so does a list of 32 lines from it.
            EXPECT_EQ(build_domain(domain, ustl::views::Span<HIrqNum const>(GOOD),
                                   static_cast<u32>(VIrqNum::Invalid)),
                      Status::InvArgs, "a floor at VIrqNum::Invalid leaves no room and is refused");
            EXPECT_EQ(
                build_domain(domain, ustl::views::Span<HIrqNum const>(GOOD), u32{0xffff'ffe1}),
                Status::InvArgs, "a floor that rounds up out of the space is refused");
            EXPECT_EQ(
                build_domain(domain, ustl::views::Span<HIrqNum const>(many), u32{0xffff'ffe0}),
                Status::InvArgs, "a floor that leaves room for fewer virqs than lines is refused");
            EXPECT_TRUE(domain.irq_for_source(HIrqNum{3}) == VIrqNum::Invalid &&
                            domain.source_limit() == 0,
                        "and none of the three keeps anything");
            EXPECT_OK(
                build_domain(domain, ustl::views::Span<HIrqNum const>(GOOD), u32{0xffff'ffe0}),
                "the last multiple of 32 with room for the line is taken");
            EXPECT_TRUE(domain.irq_for_source(HIrqNum{3}) == VIrqNum{0xffff'ffe0},
                        "and is the line's virq");
        }
        {
            Domain domain;
            EXPECT_EQ(build_domain(domain, ustl::views::Span<HIrqNum const>(TWICE)),
                      Status::AlreadyExists, "a number given twice is refused");
            EXPECT_TRUE(domain.irq_for_source(HIrqNum{3}) == VIrqNum::Invalid &&
                            domain.source_limit() == 0,
                        "and the refusal keeps nothing");
            EXPECT_OK(build_domain(domain, ustl::views::Span<HIrqNum const>(GOOD)),
                      "so the same domain takes a good list after it");
            EXPECT_TRUE(domain.irq_for_source(HIrqNum{3}) == VIrqNum{32} &&
                            domain.source_for_irq(VIrqNum{32}) == HIrqNum{3},
                        "and maps it");
            EXPECT_EQ(build_domain(domain, ustl::views::Span<HIrqNum const>(OTHER)),
                      Status::BadState, "a built domain refuses a second build");
            EXPECT_TRUE(domain.irq_for_source(HIrqNum{3}) == VIrqNum{32} &&
                            domain.source_for_irq(VIrqNum{32}) == HIrqNum{3} &&
                            domain.irq_for_source(HIrqNum{9}) == VIrqNum::Invalid,
                        "and keeps its map");
        }
    }

    KTEST(IrqDomain, BuildsDomainsFromLayouts) {
        check_layouts();
        check_refusals();
    }

} // namespace
} // namespace ours
