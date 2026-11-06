/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <sched_domain/sched_domain.hpp>

#include <array>
#include <vector>

#include <ffl/fixed.hpp>
#include <gtest/gtest.h>

namespace {
using sched_domain::build_domains;
using sched_domain::CpuMask;
using sched_domain::CpuNum;
using sched_domain::Domain;
using sched_domain::Domains;
using Load = ffl::Fixed<i64, 16>;
using sched_domain::MAX_BATCH;
using sched_domain::MAX_DOMAINS;
using sched_domain::Parameters;
using sched_domain::pull_amount;
using sched_domain::PullBudget;

// The shapes below name CPUs up to 7.
static_assert(ours::MAX_CPU >= 8, "the test machines have up to eight CPUs");

// Different numbers for the two kinds of tier, so that a swap shows.
constexpr auto PARAMS = Parameters{.same_node = {.min_runnable = 2, .max_batch = 4},
                                   .other_node = {.min_runnable = 3, .max_batch = 6}};

/// The CPUs of |cpus| as a mask.
auto mask_of(std::vector<CpuNum> const &cpus) -> CpuMask {
    CpuMask mask;
    for (auto const cpu: cpus) {
        mask.set(cpu);
    }
    return mask;
}

/// The domains of |owner| on a machine whose CPU i is on node |nodes[i]|.
auto domains_on(std::vector<int> const &nodes, CpuNum const owner) -> Domains {
    CpuMask possible;
    for (CpuNum cpu = 0; cpu < nodes.size(); ++cpu) {
        possible.set(cpu);
    }
    return build_domains(
        owner, possible, [&nodes](CpuNum const cpu) { return nodes[cpu]; }, PARAMS);
}

TEST(SchedDomain, OneNodeHasOneTier) {
    auto const domains = domains_on({0, 0, 0, 0}, 0);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({1, 2, 3}));
    EXPECT_EQ(domains.view()[0].min_runnable, PARAMS.same_node.min_runnable);
}

TEST(SchedDomain, TwoNodesHaveTheNodeFirstAndTheRestAfter) {
    auto const domains = domains_on({0, 0, 1, 1}, 0);
    ASSERT_EQ(domains.count(), 2u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({1}));
    EXPECT_EQ(domains.view()[0].min_runnable, PARAMS.same_node.min_runnable);
    EXPECT_EQ(domains.view()[0].max_batch, PARAMS.same_node.max_batch);
    EXPECT_TRUE(domains.view()[1].span == mask_of({2, 3}));
    EXPECT_EQ(domains.view()[1].min_runnable, PARAMS.other_node.min_runnable);
}

TEST(SchedDomain, AnOwnerAloneOnItsNodeHasNoNodeTier) {
    auto const domains = domains_on({0, 1, 1, 1}, 0);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({1, 2, 3}));
    EXPECT_EQ(domains.view()[0].min_runnable, PARAMS.other_node.min_runnable);
}

TEST(SchedDomain, TheViewIsTheOwnersOwn) {
    auto const domains = domains_on({0, 1, 1, 1}, 2);
    ASSERT_EQ(domains.count(), 2u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({1, 3}));
    EXPECT_TRUE(domains.view()[1].span == mask_of({0}));
}

TEST(SchedDomain, AFullListRefusesAnotherTier) {
    Domains domains;
    for (usize i = 0; i < MAX_DOMAINS; ++i) {
        EXPECT_TRUE(domains.push(Domain{.span = mask_of({1}), .min_runnable = 1}));
    }
    EXPECT_FALSE(domains.push(Domain{.span = mask_of({2}), .min_runnable = 1}));
    EXPECT_EQ(domains.count(), MAX_DOMAINS);
}

TEST(SchedDomain, OneCpuHasNoTier) {
    EXPECT_EQ(domains_on({0}, 0).count(), 0u);
}

TEST(SchedDomain, ACpuThatCannotExistIsInNoTier) {
    CpuMask const possible = mask_of({0, 2});
    std::array<int, 3> const nodes = {0, 0, 1};
    auto const domains =
        build_domains(0, possible, [&nodes](CpuNum const cpu) { return nodes[cpu]; }, PARAMS);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({2}));
    EXPECT_EQ(domains.view()[0].min_runnable, PARAMS.other_node.min_runnable);
}

TEST(SchedDomain, CpusNobodyHasPlacedCountAsOneNode) {
    // Every unplaced CPU answers the same sentinel; they are one node.
    constexpr int UNPLACED = 99;
    auto const domains = domains_on({UNPLACED, UNPLACED, UNPLACED}, 1);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({0, 2}));
}

TEST(SchedDomain, TheTiersPartitionTheOtherCpusOnEveryShape) {
    std::vector<std::vector<int>> const shapes = {
        {0},          {0, 0},          {0, 1},
        {0, 0, 1, 1}, {0, 1, 0, 1},    {0, 1, 2, 3},
        {0, 0, 0, 1}, {1, 0, 0, 0, 0}, {2, 2, 0, 0, 1, 1, 0, 2},
    };
    for (auto const &nodes: shapes) {
        for (CpuNum owner = 0; owner < nodes.size(); ++owner) {
            auto const domains = domains_on(nodes, owner);
            CpuMask seen;
            CpuMask others;
            for (CpuNum cpu = 0; cpu < nodes.size(); ++cpu) {
                if (cpu != owner) {
                    others.set(cpu);
                }
            }
            for (auto const &domain: domains.view()) {
                EXPECT_TRUE(domain.span.any());
                EXPECT_TRUE((seen & domain.span).none()) << "a CPU is in two tiers";
                EXPECT_FALSE(domain.span.test(owner)) << "the owner is in its own tier";
                seen |= domain.span;
            }
            EXPECT_TRUE(seen == others) << "the tiers are not every other CPU";
        }
    }
}

TEST(SchedDomain, EachCpuIsInTheTierOfItsKindOfDistance) {
    std::vector<int> const nodes = {2, 2, 0, 0, 1, 1, 0, 2};
    for (CpuNum owner = 0; owner < nodes.size(); ++owner) {
        auto const domains = domains_on(nodes, owner);
        for (auto const &domain: domains.view()) {
            domain.span.for_each([&](CpuNum const cpu) {
                auto expected = PARAMS.other_node.min_runnable;
                if (nodes[cpu] == nodes[owner]) {
                    expected = PARAMS.same_node.min_runnable;
                }
                EXPECT_EQ(domain.min_runnable, expected)
                    << "CPU " << cpu << " seen from CPU " << owner;
            });
        }
    }
}

TEST(SchedDomain, ABatchIsCutToTheMostAThreadCanHold) {
    CpuMask possible = mask_of({0, 1});
    auto const huge = Parameters{.same_node = {.min_runnable = 2, .max_batch = 1000},
                                 .other_node = {.min_runnable = 3, .max_batch = 1000}};
    auto const domains = build_domains(0, possible, [](CpuNum) { return 0; }, huge);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_EQ(domains.view()[0].max_batch, MAX_BATCH);
}

TEST(SchedDomain, TheOtherTierCarriesItsOwnBatch) {
    auto const domains = domains_on({0, 0, 1, 1}, 0);
    ASSERT_EQ(domains.count(), 2u);
    EXPECT_EQ(domains.view()[1].max_batch, PARAMS.other_node.max_batch);
}

// The load of n equal threads, as the scheduler weighs them.
auto threads(i64 const n) -> Load {
    return Load{n};
}

TEST(PullAmount, ABusyCpuHandsAnIdleOneItsShareOfTheMean) {
    // One CPU holds eight threads, three hold none: the mean is two.
    EXPECT_EQ(pull_amount(threads(0), threads(8), threads(2)), threads(2));
}

TEST(PullAmount, NoMoreThanTheBusiestHasAboveTheMean) {
    // Mean 3: the busiest has one above it, the idle one is three below.
    EXPECT_EQ(pull_amount(threads(0), threads(4), threads(3)), threads(1));
}

TEST(PullAmount, NothingWhenTheCpuIsAtOrAboveTheMean) {
    EXPECT_EQ(pull_amount(threads(2), threads(5), threads(2)), threads(0));
    EXPECT_EQ(pull_amount(threads(3), threads(5), threads(2)), threads(0));
}

TEST(PullAmount, NothingWhenTheBusiestIsNotAboveTheMean) {
    EXPECT_EQ(pull_amount(threads(0), threads(2), threads(2)), threads(0));
    EXPECT_EQ(pull_amount(threads(0), threads(1), threads(2)), threads(0));
}

TEST(PullAmount, KeepsAFractionOfAThread) {
    // Three threads over two CPUs: the mean is one and a half.
    auto const mean = threads(3) / 2;
    EXPECT_EQ(pull_amount(threads(0), threads(3), mean), mean);
}

TEST(PullAmount, WorksOnPlainCounts) {
    EXPECT_EQ(pull_amount(0, 8, 2), 2);
    EXPECT_EQ(pull_amount(0, 3, 2), 1);
}

TEST(PullAmount, TheMeanSettlesAnEightThreadMachineInThreeLooks) {
    // The example the scheduler was designed against: eight threads on CPU 0
    // of four, three CPUs look in turn.
    std::array<i64, 4> loads = {8, 0, 0, 0};
    for (usize looker = 1; looker < loads.size(); ++looker) {
        i64 total = 0;
        for (auto const load: loads) {
            total += load;
        }
        auto const mean = threads(total) / static_cast<i64>(loads.size());
        usize busiest = 0;
        for (usize cpu = 0; cpu < loads.size(); ++cpu) {
            if (cpu != looker && loads[cpu] > loads[busiest]) {
                busiest = cpu;
            }
        }
        auto const amount = pull_amount(threads(loads[looker]), threads(loads[busiest]), mean);
        auto const moved = static_cast<i64>(amount.floor());
        loads[busiest] -= moved;
        loads[looker] += moved;
    }
    EXPECT_EQ(loads, (std::array<i64, 4>{2, 2, 2, 2}));
}

TEST(PullBudget, TheFirstThreadIsTakenWhateverItWeighs) {
    PullBudget<Load> budget{threads(1) / 2, 4};
    EXPECT_TRUE(budget.offer(threads(3)));
    EXPECT_EQ(budget.taken(), 1u);
    EXPECT_TRUE(budget.done());
}

TEST(PullBudget, LaterThreadsMustFitWhatIsLeft) {
    PullBudget<Load> budget{threads(5) / 2, 4};
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_FALSE(budget.done());
    EXPECT_FALSE(budget.offer(threads(1))) << "half a thread is left";
    EXPECT_EQ(budget.taken(), 2u);
}

TEST(PullBudget, AHeavyThreadIsSkippedAndALightOneBehindItTaken) {
    PullBudget<Load> budget{threads(2), 4};
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_FALSE(budget.offer(threads(3))) << "more than is left";
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.done());
}

TEST(PullBudget, StopsAtTheBatch) {
    PullBudget<Load> budget{threads(100), 3};
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.done());
    EXPECT_FALSE(budget.offer(threads(1)));
    EXPECT_EQ(budget.taken(), 3u);
}

TEST(PullBudget, IsNotDoneOneThreadBeforeTheBatchIsFull) {
    PullBudget<Load> budget{threads(100), 3};
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_FALSE(budget.done()) << "one more is allowed";
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.done());
}

TEST(PullBudget, TakesTheFirstThreadEvenWhenNothingIsToBeMoved) {
    PullBudget<Load> budget{threads(0), 4};
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_FALSE(budget.offer(threads(1))) << "and nothing after it";
    EXPECT_TRUE(budget.done());
}

TEST(PullBudget, ABatchOfNoneIsABatchOfOne) {
    PullBudget<Load> budget{threads(100), 0};
    EXPECT_TRUE(budget.offer(threads(1)));
    EXPECT_FALSE(budget.offer(threads(1)));
    EXPECT_TRUE(budget.done());
}

TEST(SchedDomain, ABatchOfNoneInTheParametersIsOne) {
    CpuMask const possible = mask_of({0, 1});
    auto const none = Parameters{.same_node = {.min_runnable = 2, .max_batch = 0},
                                 .other_node = {.min_runnable = 3, .max_batch = 0}};
    auto const domains = build_domains(0, possible, [](CpuNum) { return 0; }, none);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_EQ(domains.view()[0].max_batch, 1u);
}

TEST(PullBudget, NothingIsDoneBeforeTheFirstOffer) {
    PullBudget<Load> budget{threads(0), 4};
    EXPECT_FALSE(budget.done());
}

} // namespace
