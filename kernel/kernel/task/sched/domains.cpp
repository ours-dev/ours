/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Where the scheduling domains come from: the memory manager's table of which
/// node each CPU is on, read once. This is the only file of the scheduler that
/// knows the table exists.
#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/task/sched/domains.hpp>

#include <array>

#include <gktl/init_hook.hpp>

namespace ours::task::sched {
namespace {
    /// How many threads a CPU must hold, the one it runs counted
    /// (Scheduler::num_runnable()), for a CPU with nothing to run to take one
    /// from it, when it is on the same NUMA node as the CPU taking. One running
    /// and one waiting is enough: the waiting one is behind a whole slice, and
    /// on the same node it loses only its cache.
    constexpr usize STEAL_MIN_RUNNABLE_SAME_NODE = 2;

    /// The same, when the CPU is on another node. The thread leaves the memory
    /// it has been working on, so a second one waiting is asked for. There is no
    /// estimate of how long a thread has been queued here, only the count, and
    /// both numbers are first guesses to be measured on real hardware (item 24
    /// of docs/repairs/261103/kernel-task-sched.md).
    constexpr usize STEAL_MIN_RUNNABLE_OTHER_NODE = 3;

    /// The most threads one look takes from a CPU on the same node, and from a
    /// CPU on another. It bounds the time the threads are in neither queue and
    /// the dequeues done with the victim's lock held (the walk that picks them
    /// has a bound of its own, sched_domain::MAX_SCAN). Four is a first guess,
    /// for the same reason as the numbers above (item 24 of
    /// docs/repairs/261103/kernel-task-sched.md).
    constexpr usize STEAL_MAX_BATCH_SAME_NODE = 4;
    constexpr usize STEAL_MAX_BATCH_OTHER_NODE = 4;

    /// Written once by build_all_domains(), while one CPU runs, and only read
    /// after. Indexed by the CPU that owns the tiers. Empty from the first
    /// instruction, by constinit, because a look for work may come before the
    /// hook below has run.
    constinit std::array<sched_domain::Domains, MAX_CPU> DOMAINS;

    auto build_all_domains() -> void {
        auto const parameters = sched_domain::Parameters{
            .same_node = {.min_runnable = STEAL_MIN_RUNNABLE_SAME_NODE,
                          .max_batch = STEAL_MAX_BATCH_SAME_NODE},
            .other_node = {.min_runnable = STEAL_MIN_RUNNABLE_OTHER_NODE,
                           .max_batch = STEAL_MAX_BATCH_OTHER_NODE},
        };
        auto const &possible = Cpu::possible_mask();
        DEBUG_ASSERT(possible.any(), "the table is built before the possible CPUs are known");
        possible.for_each([&](CpuNum const cpu) {
            DOMAINS[cpu] = sched_domain::build_domains(
                cpu, possible, [](CpuNum const other) { return mm::CpuNodeMap::node_of(other); },
                parameters);
        });
    }

    /// After the MADT parse has filled the possible mask and the node table
    /// has placed every CPU in it (PmNode::claim_possible_cpus(), at
    /// InitLevel::Irq + 1), and before the other CPUs start: main.cpp announces
    /// Threading, which runs this, and only then creates the thread that calls
    /// init_platform() and with it x86_bring_aps().
    GKTL_INIT_HOOK(BuildSchedDomains, build_all_domains, gktl::InitLevel::Threading);
} // namespace

auto domains_of(CpuNum const cpu) -> sched_domain::Domains const & {
    DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
    return DOMAINS[cpu];
}
} // namespace ours::task::sched
