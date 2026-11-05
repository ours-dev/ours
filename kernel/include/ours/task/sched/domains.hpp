/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The scheduling domains of each CPU: the tiers of CPUs it looks at for work.
///
/// The scheduler reads this table and nothing else to know what is near. Where
/// the table comes from is in domains.cpp and nowhere else, so the code that
/// walks the tiers (Scheduler::Current::steal()) never asks the memory manager
/// about nodes, and a kind of distance that has no source yet is one more tier
/// built there.
#ifndef OURS_TASK_SCHED_DOMAINS_HPP
#define OURS_TASK_SCHED_DOMAINS_HPP 1

#include <ours/types.hpp>

#include <sched_domain/sched_domain.hpp>

namespace ours::task::sched {
/// |cpu|'s tiers, nearest first, the CPU itself in none of them.
///
/// Built once, at InitLevel::Threading, before any CPU but the first runs the
/// scheduler, and read-only after: no lock. Empty before that, which makes a
/// look for work find nothing rather than something wrong. A CPU placed on
/// another node later (PmNode::set_cpu_node(), which has no caller yet) does
/// not change it (item 35 of docs/repairs/261103/kernel-task-sched.md).
[[nodiscard]]
auto domains_of(CpuNum cpu) -> sched_domain::Domains const &;
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_DOMAINS_HPP
