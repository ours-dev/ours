/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The scheduling domains of a CPU: the nested sets of CPUs it looks at for
/// work, nearest first, each of a kind that a policy sets the parameters of.
///
/// Pure. The scheduler asks for a CPU's domains instead of asking the memory
/// manager which node a CPU is on every time it looks, so what it looks at is
/// data, built once from the topology, rather than code; and the numbers that
/// say how deep a queue has to be before it is worth taking from, and how many
/// threads a look takes, are knobs with a name, a default and a range, here, and
/// their value is the kernel's to keep. A new kind of distance (CPUs that
/// share a cache, CPUs of one package) is one more kind of tier and its knobs,
/// and no change to the code that walks them.
///
/// Header only: there is nothing here that needs a translation unit of its own.
/// A CpuMask is MAX_CPU bits wide, and MAX_CPU is a build parameter the kernel
/// sets (the host test names a number of its own), so whatever includes this
/// header must be built with the same one; being inline, the code simply takes
/// the value of the translation unit it lands in.
#ifndef SCHED_DOMAIN_SCHED_DOMAIN_HPP
#define SCHED_DOMAIN_SCHED_DOMAIN_HPP 1

#include <array>
#include <concepts>
#include <functional>
#include <span>
#include <string_view>

#include <ours/cpu_mask.hpp>
#include <ours/types.hpp>

namespace sched_domain {
using ours::CpuMask;
using ours::CpuNum;

/// What a tier is, which says which knobs apply to the CPUs in it.
enum class TierKind : u32 {
    /// The CPUs on the owner's node.
    SameNode,

    /// The CPUs on every other node.
    OtherNode,
};

/// One tier of a CPU's view of the machine.
struct Domain {
    /// The CPUs in this tier, the CPU the list belongs to excluded. Every CPU
    /// that may ever exist is in it, whether it is online or not: whoever uses
    /// the tier intersects it with the CPUs that are active at that moment.
    CpuMask span;

    /// What the tier is, which says which knobs apply to its CPUs. The nearer the
    /// kind, the cheaper the move, so by default the fewer threads a CPU of it
    /// has to hold to be worth taking from; nothing keeps the knobs in that order.
    TierKind kind = TierKind::SameNode;
};

/// The most threads one look takes, whatever a tier asks for: the room a caller
/// keeps for the threads between the two queues.
inline constexpr usize MAX_BATCH = 8;

/// The most threads of a victim's queue one look examines, taken or not. The
/// examining is done with the victim's lock held and interrupts off, and a
/// queue of threads that cannot move (pinned to the victim, too heavy for what
/// is left) would otherwise be walked to its end by every CPU that looks.
inline constexpr usize MAX_SCAN = 4 * MAX_BATCH;

/// The most tiers one CPU has. The builder below produces two; the room is for
/// the kinds of distance that have no source yet.
inline constexpr usize MAX_DOMAINS = 4;

/// One CPU's tiers, nearest first.
class Domains {
  public:
    /// How many tiers there are. Zero for a machine with one CPU.
    [[nodiscard]]
    constexpr auto count() const -> usize {
        return m_count;
    }

    /// The tiers, nearest first. Not of a temporary: the view points into the
    /// list, which a range-for over `build_domains(...).view()` would let go of
    /// before the first step.
    [[nodiscard]]
    constexpr auto view() const & -> std::span<Domain const> {
        return std::span<Domain const>{m_domains.data(), m_count};
    }
    [[nodiscard]]
    constexpr auto view() const && -> std::span<Domain const> = delete;

    /// Adds a tier outside the ones already there. False, and nothing added,
    /// when the list is full.
    [[nodiscard]]
    constexpr auto push(Domain const &domain) -> bool {
        if (m_count == MAX_DOMAINS) {
            return false;
        }
        m_domains[m_count] = domain;
        m_count += 1;
        return true;
    }

  private:
    std::array<Domain, MAX_DOMAINS> m_domains{};
    usize m_count = 0;
};

static_assert(MAX_DOMAINS >= 2, "the builder makes a tier for the node and one for the rest");

/// The numbers a policy sets, each by a name.
///
/// The library knows what a knob is called, what it is when nobody has said,
/// and what it may be; where its value is kept, and how it is changed, is the
/// kernel's (the boot options give it a first value and a console command
/// changes it while the machine runs).
enum class Knob : u32 {
    /// Whether a CPU looks for threads to take at all, when its idle thread
    /// does or when another CPU asks it to: 0 or 1.
    Enabled,

    /// How many threads a CPU on the owner's node must hold, the one it runs
    /// counted, for the owner to take one from it. At least 2: a thread waits
    /// behind the running one only then, and a CPU with a single heavy thread
    /// would otherwise be chosen as the victim ahead of one that has a thread
    /// to give.
    MinRunnableSameNode,

    /// The same, on another node.
    MinRunnableOtherNode,

    /// The most threads one look takes from a CPU on the owner's node.
    MaxBatchSameNode,

    /// The same, from a CPU on another node.
    MaxBatchOtherNode,
};

/// How many knobs there are.
inline constexpr usize NUM_KNOBS = 5;

/// The most a threshold may be: a number of threads no run queue holds.
inline constexpr usize MAX_MIN_RUNNABLE = 1024;

/// The least a threshold may be: the running thread and one waiting.
inline constexpr usize MIN_MIN_RUNNABLE = 2;

/// What a knob is.
struct KnobInfo {
    /// The name the boot options (`kernel.sched.steal.<name>`) and the console
    /// command call it by.
    std::string_view name;
    usize default_value = 0;
    /// The values it accepts, both ends included.
    usize lowest = 0;
    usize highest = 0;
};

[[nodiscard]]
constexpr auto knob_info(Knob const knob) -> KnobInfo {
    switch (knob) {
        case Knob::Enabled:
            return {.name = "enabled", .default_value = 1, .lowest = 0, .highest = 1};
        case Knob::MinRunnableSameNode:
            return {.name = "min-runnable-same-node",
                    .default_value = 2,
                    .lowest = MIN_MIN_RUNNABLE,
                    .highest = MAX_MIN_RUNNABLE};
        case Knob::MinRunnableOtherNode:
            return {.name = "min-runnable-other-node",
                    .default_value = 3,
                    .lowest = MIN_MIN_RUNNABLE,
                    .highest = MAX_MIN_RUNNABLE};
        case Knob::MaxBatchSameNode:
            return {.name = "max-batch-same-node",
                    .default_value = 4,
                    .lowest = 1,
                    .highest = MAX_BATCH};
        case Knob::MaxBatchOtherNode:
            return {.name = "max-batch-other-node",
                    .default_value = 4,
                    .lowest = 1,
                    .highest = MAX_BATCH};
    }
    // Not a knob: a range nothing is in.
    return {.name = {}, .default_value = 0, .lowest = 1, .highest = 0};
}

/// Whether |value| is one the knob accepts.
[[nodiscard]]
constexpr auto knob_accepts(Knob const knob, usize const value) -> bool {
    auto const info = knob_info(knob);
    return info.lowest <= value && value <= info.highest;
}

/// The knob called |name|: true and |knob| set, or false.
[[nodiscard]]
constexpr auto find_knob(std::string_view const name, Knob &knob) -> bool {
    for (usize i = 0; i < NUM_KNOBS; ++i) {
        auto const candidate = static_cast<Knob>(i);
        if (knob_info(candidate).name == name) {
            knob = candidate;
            return true;
        }
    }
    return false;
}

/// The knob that sets how many threads a CPU of a tier of |kind| must hold.
[[nodiscard]]
constexpr auto min_runnable_knob(TierKind const kind) -> Knob {
    // Every kind is named, so that a new kind is a warning here until it has
    // knobs of its own.
    switch (kind) {
        case TierKind::SameNode:
            return Knob::MinRunnableSameNode;
        case TierKind::OtherNode:
            return Knob::MinRunnableOtherNode;
    }
    return Knob::MinRunnableOtherNode;
}

/// The knob that sets the most threads a look takes from a tier of |kind|.
[[nodiscard]]
constexpr auto max_batch_knob(TierKind const kind) -> Knob {
    switch (kind) {
        case TierKind::SameNode:
            return Knob::MaxBatchSameNode;
        case TierKind::OtherNode:
            return Knob::MaxBatchOtherNode;
    }
    return Knob::MaxBatchOtherNode;
}

/// The domains of |owner|, from a table of nodes.
///
/// |possible| is every CPU that may exist, and |node_of| answers the node of
/// one of them: any type that compares equal for CPUs on one node. Two tiers
/// come out, the CPUs on |owner|'s node and the CPUs on every other node, and
/// a tier with no CPU in it is left out, so a machine of one node has one tier
/// and a machine of one CPU has none. Together they are |possible| without
/// |owner|, with no CPU in both.
///
/// The CPUs that are not placed yet all answer the same value, so they count
/// as one node. That is the right answer for the single node such a machine
/// has, and nothing else asks.
template <typename NodeOf>
    requires std::invocable<NodeOf const &, CpuNum> &&
             std::equality_comparable<std::invoke_result_t<NodeOf const &, CpuNum>>
[[nodiscard]]
auto build_domains(CpuNum const owner, CpuMask const &possible, NodeOf const &node_of) -> Domains {
    auto const owner_node = node_of(owner);

    CpuMask same_node;
    CpuMask other_nodes;
    possible.for_each([&](CpuNum const cpu) {
        if (cpu == owner) {
            return;
        }
        if (node_of(cpu) == owner_node) {
            same_node.set(cpu);
        } else {
            other_nodes.set(cpu);
        }
    });

    // Neither push can fail: two tiers, and the list holds at least two (the
    // static_assert above).
    Domains domains;
    if (same_node.any()) {
        auto _ = domains.push(Domain{.span = same_node, .kind = TierKind::SameNode});
    }
    if (other_nodes.any()) {
        auto _ = domains.push(Domain{.span = other_nodes, .kind = TierKind::OtherNode});
    }
    return domains;
}

/// How much load a CPU should take from the busiest CPU of its domain: enough
/// to bring itself up to |average|, the mean load of the CPUs of the domain
/// with itself among them, and no more than the busiest holds above it. Neither
/// is pulled past the mean, which would only make the other the busy one. Zero
/// when the CPU is already at the mean or the busiest is not above it.
///
/// |Load| is any ordered number that subtracts: a thread count, or the weight
/// of the threads.
template <typename Load>
[[nodiscard]]
constexpr auto pull_amount(Load const &here, Load const &busiest, Load const &average) -> Load {
    if (!(here < average) || !(average < busiest)) {
        return Load{};
    }
    auto const above = busiest - average;
    auto const below = average - here;
    if (above < below) {
        return above;
    }
    return below;
}

/// The threads of one victim to take, offered in the order its queue walks
/// them, against an amount of load to move.
///
/// For a CPU that has nothing to run, the first thread offered is taken
/// whatever it weighs, and even when the amount is none (|first_is_free|). That
/// CPU would otherwise go back to halt with a thread waiting on the victim, as
/// a mean that is a fraction of one thread (many idle CPUs share the machine's
/// load), or one raised by load that cannot be taken (a heavy thread elsewhere,
/// pinned threads of a nearer tier), would make it. A CPU that is running
/// something has no such claim: every thread it takes must fit. Every thread
/// after the first fits what is left, so that a heavy thread is not carried
/// past the mean for the sake of a light one behind it, and at most
/// |max_batch| are taken in all (at least one: 0 is read as 1).
template <typename Load>
class PullBudget {
  public:
    constexpr PullBudget(Load const &amount, usize const max_batch, bool const first_is_free)
        : m_left(amount), m_max_batch(max_batch), m_first_is_free(first_is_free) {
        if (m_max_batch < 1) {
            m_max_batch = 1;
        }
    }

    /// Whether the thread of weight |load| is taken. A thread that is not
    /// taken is skipped, and the next may still be.
    [[nodiscard]]
    constexpr auto offer(Load const &load) -> bool {
        if (m_taken >= m_max_batch) {
            return false;
        }
        if ((m_taken > 0 || !m_first_is_free) && m_left < load) {
            return false;
        }
        if (load < m_left) {
            m_left = m_left - load;
        } else {
            m_left = Load{};
        }
        m_taken += 1;
        return true;
    }

    /// Whether nothing offered from here on can be taken: the batch is full, or
    /// something was taken and nothing is left to move.
    [[nodiscard]]
    constexpr auto done() const -> bool {
        auto const spent = !(Load{} < m_left);
        return m_taken >= m_max_batch || (spent && (m_taken > 0 || !m_first_is_free));
    }

    [[nodiscard]]
    constexpr auto taken() const -> usize {
        return m_taken;
    }

  private:
    Load m_left;
    usize m_max_batch;
    bool m_first_is_free;
    usize m_taken = 0;
};

} // namespace sched_domain

#endif // #ifndef SCHED_DOMAIN_SCHED_DOMAIN_HPP
