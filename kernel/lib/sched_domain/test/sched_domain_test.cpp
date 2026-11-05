/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <sched_domain/sched_domain.hpp>

#include <array>
#include <vector>

#include <gtest/gtest.h>

namespace {
using sched_domain::build_domains;
using sched_domain::CpuMask;
using sched_domain::CpuNum;
using sched_domain::Domain;
using sched_domain::Domains;
using sched_domain::MAX_DOMAINS;
using sched_domain::Thresholds;

// The shapes below name CPUs up to 7.
static_assert(ours::MAX_CPU >= 8, "the test machines have up to eight CPUs");

constexpr auto THRESHOLDS = Thresholds{.same_node = 2, .other_node = 3};

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
        owner, possible, [&nodes](CpuNum const cpu) { return nodes[cpu]; }, THRESHOLDS);
}

TEST(SchedDomain, OneNodeHasOneTier) {
    auto const domains = domains_on({0, 0, 0, 0}, 0);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({1, 2, 3}));
    EXPECT_EQ(domains.view()[0].min_runnable, THRESHOLDS.same_node);
}

TEST(SchedDomain, TwoNodesHaveTheNodeFirstAndTheRestAfter) {
    auto const domains = domains_on({0, 0, 1, 1}, 0);
    ASSERT_EQ(domains.count(), 2u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({1}));
    EXPECT_EQ(domains.view()[0].min_runnable, THRESHOLDS.same_node);
    EXPECT_TRUE(domains.view()[1].span == mask_of({2, 3}));
    EXPECT_EQ(domains.view()[1].min_runnable, THRESHOLDS.other_node);
}

TEST(SchedDomain, AnOwnerAloneOnItsNodeHasNoNodeTier) {
    auto const domains = domains_on({0, 1, 1, 1}, 0);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({1, 2, 3}));
    EXPECT_EQ(domains.view()[0].min_runnable, THRESHOLDS.other_node);
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
        build_domains(0, possible, [&nodes](CpuNum const cpu) { return nodes[cpu]; }, THRESHOLDS);
    ASSERT_EQ(domains.count(), 1u);
    EXPECT_TRUE(domains.view()[0].span == mask_of({2}));
    EXPECT_EQ(domains.view()[0].min_runnable, THRESHOLDS.other_node);
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
                auto expected = THRESHOLDS.other_node;
                if (nodes[cpu] == nodes[owner]) {
                    expected = THRESHOLDS.same_node;
                }
                EXPECT_EQ(domain.min_runnable, expected)
                    << "CPU " << cpu << " seen from CPU " << owner;
            });
        }
    }
}

} // namespace
