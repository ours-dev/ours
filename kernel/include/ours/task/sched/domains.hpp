/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The scheduling domains of each CPU: the tiers of CPUs it looks at for work.
///
/// The scheduler reads this table and nothing else to know what is near. Where
/// the table comes from is in domains.cpp and nowhere else, so the code that
/// walks the tiers (Scheduler::steal_work_from()) never asks the memory manager
/// about nodes, and a kind of distance that has no source yet is one more tier
/// built there. The numbers that steer the walk are StealPolicy's.
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

/// The numbers that steer a look for threads to take: whether it happens, how
/// deep a queue must be to be worth taking from and how many threads a look
/// takes, each by tier kind (sched_domain::Knob names them, and says what each
/// may be).
///
/// Given a first value from the boot options (kernel.sched.steal.<name>) when
/// the domains are built, and changeable while the machine runs. The writers
/// are the boot hook, the console command `steal.policy` (which runs one at a
/// time on the one console), and the tests, which put a knob back as they found
/// it: a test's restore would overwrite a setting made meanwhile, so a test runs
/// alone.
///
/// Each knob is a word of its own, written and read relaxed, because each alone
/// is a complete and valid setting. A look that reads the new threshold with the
/// old batch is one of the settings someone could have asked for, so nothing
/// has to make the reads agree with one another; a look reads a tier's pair once
/// and uses it for the whole tier. What a setting touches is a hint that is
/// confirmed under a lock afterwards, and the range of each
/// (sched_domain::knob_info()) keeps a setting meaningful: a threshold of one
/// would make a CPU with a single heavy thread the victim, and a batch of none
/// takes nothing. The code that uses a value clamps it as well (the batch to the
/// room it keeps), so a wrong range would not break it.
///
/// Before the domains are built every knob reads 0: not enabled, so a look for
/// work that comes early finds nothing, as it does with no domains.
class StealPolicy {
  public:
    /// |knob|'s value now.
    [[nodiscard]]
    static auto get(sched_domain::Knob knob) -> usize;

    /// Sets |knob| to |value| if the knob accepts it, and says whether it did;
    /// a value out of range changes nothing.
    [[nodiscard]]
    static auto set(sched_domain::Knob knob, usize value) -> bool;

    /// Whether a CPU looks for threads to take: its idle thread, or a call on
    /// its scheduler from another CPU.
    [[nodiscard]]
    static auto enabled() -> bool {
        return 0 != get(sched_domain::Knob::Enabled);
    }

    /// How many threads a CPU of a tier of |kind| must hold, the one it runs
    /// counted, to be taken from.
    [[nodiscard]]
    static auto min_runnable(sched_domain::TierKind const kind) -> usize {
        return get(sched_domain::min_runnable_knob(kind));
    }

    /// The most threads one look takes from a tier of |kind|.
    [[nodiscard]]
    static auto max_batch(sched_domain::TierKind const kind) -> usize {
        return get(sched_domain::max_batch_knob(kind));
    }
};
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_DOMAINS_HPP
