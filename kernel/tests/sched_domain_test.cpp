/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// SchedDomains.MatchTheNodeTable: the scheduling domains the kernel built at
// boot (domains_of()) are what sched_domain::build_domains() makes of the node
// table the memory manager holds. The builder is checked on the host, against
// tables written by hand; this checks the part that only a booted kernel can,
// the glue in domains.cpp -- that the init hook ran after the possible CPUs
// were known and the node table had placed them, and indexed each CPU's tiers
// by that CPU. A table that was never built, or built before the CPUs were
// known, is empty, which a look for work answers by finding nothing, and
// nothing else in a boot would show it.
//
// For every possible CPU: its tiers hold every other possible CPU, once each
// and not itself, and the CPUs of one tier all stand in the same relation to
// its node, on it or off it.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/task/sched/domains.hpp>

#include <ktest/ktest.hpp>

namespace ours {
namespace {
    KTEST(SchedDomains, MatchTheNodeTable) {
        auto const possible = Cpu::possible_mask();
        ASSERT_TRUE(possible.any(), "the possible CPUs are known");

        possible.for_each([&](CpuNum const cpu) {
            auto const &domains = task::sched::domains_of(cpu);

            CpuMask expected = possible;
            expected.set(cpu, false);
            EXPECT_EQ(domains.count() != 0, expected.any(),
                      "a CPU has a tier if and only if there is another CPU");

            CpuMask seen;
            for (auto const &domain: domains.view()) {
                EXPECT_TRUE(domain.span.any(), "a tier is never empty");
                EXPECT_TRUE((seen & domain.span).none(), "no CPU is in two tiers");
                EXPECT_FALSE(domain.span.test(cpu), "a CPU is in no tier of its own");

                auto const own_node = mm::CpuNodeMap::node_of(cpu);
                CpuMask on_node;
                domain.span.for_each([&](CpuNum const other) {
                    if (mm::CpuNodeMap::node_of(other) == own_node) {
                        on_node.set(other);
                    }
                });
                EXPECT_TRUE(on_node.none() || on_node == domain.span,
                            "a tier is the CPUs of the node or the CPUs of the others");
                seen |= domain.span;
            }
            EXPECT_TRUE(seen == expected, "the tiers together are every other possible CPU");
        });
    }
} // namespace
} // namespace ours
