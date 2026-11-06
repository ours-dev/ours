/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The scheduling domains of a CPU: the nested sets of CPUs it looks at for
/// work, nearest first, each with its own parameters.
///
/// Pure. The scheduler asks for a CPU's domains instead of asking the memory
/// manager which node a CPU is on every time it looks, so what it looks at and
/// how deep a queue has to be before it is worth taking from are data, built
/// once from the topology, rather than code. A new kind of distance (CPUs that
/// share a cache, CPUs of one package) is one more tier here and no change to
/// the code that walks them.
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

#include <ours/cpu_mask.hpp>
#include <ours/types.hpp>

namespace sched_domain {
using ours::CpuMask;
using ours::CpuNum;

/// One tier of a CPU's view of the machine.
struct Domain {
    /// The CPUs in this tier, the CPU the list belongs to excluded. Every CPU
    /// that may ever exist is in it, whether it is online or not: whoever uses
    /// the tier intersects it with the CPUs that are active at that moment.
    CpuMask span;

    /// How many threads a CPU of this tier must hold, the one it runs counted,
    /// for the owner to take one from it. The nearer the tier, the cheaper the
    /// move, so the lower the number it can afford. At least 1: a CPU with no
    /// thread has none to give, and 0 is read as 1.
    usize min_runnable = 0;

    /// The most threads the owner takes from a CPU of this tier in one look. It
    /// bounds how long the threads taken are in neither queue and how long the
    /// victim's lock is held to take them off. At least 1 and at most
    /// MAX_BATCH: 0 is read as 1.
    usize max_batch = 1;
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

/// What a tier asks for.
struct TierParams {
    usize min_runnable = 0;
    usize max_batch = 0;
};

/// The parameters of each kind of tier.
struct Parameters {
    TierParams same_node;
    TierParams other_node;
};

/// A tier with |params|, its batch cut to MAX_BATCH.
[[nodiscard]]
constexpr auto make_domain(CpuMask const &span, TierParams const &params) -> Domain {
    auto max_batch = params.max_batch;
    if (max_batch > MAX_BATCH) {
        max_batch = MAX_BATCH;
    }
    if (max_batch < 1) {
        max_batch = 1;
    }
    return Domain{.span = span, .min_runnable = params.min_runnable, .max_batch = max_batch};
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
auto build_domains(CpuNum const owner, CpuMask const &possible, NodeOf const &node_of,
                   Parameters const &parameters) -> Domains {
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
        auto _ = domains.push(make_domain(same_node, parameters.same_node));
    }
    if (other_nodes.any()) {
        auto _ = domains.push(make_domain(other_nodes, parameters.other_node));
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
/// The first thread offered is taken whatever it weighs, and even when the
/// amount is none. The CPU that looks has nothing to run and the victim has a
/// thread waiting; a mean that is a fraction of one thread (many idle CPUs
/// share the machine's load), or one raised by load that cannot be taken (a
/// heavy thread elsewhere, pinned threads of a nearer tier), would otherwise
/// send it back to halt with the thread still waiting. Every later one is taken
/// only if it fits in what is left, so that a heavy thread is not carried past
/// the mean for the sake of a light one behind it, and at most |max_batch| in
/// all (at least one: 0 is read as 1).
template <typename Load>
class PullBudget {
  public:
    constexpr PullBudget(Load const &amount, usize const max_batch)
        : m_left(amount), m_max_batch(max_batch) {
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
        if (m_taken > 0 && m_left < load) {
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
        return m_taken >= m_max_batch || (m_taken > 0 && !(Load{} < m_left));
    }

    [[nodiscard]]
    constexpr auto taken() const -> usize {
        return m_taken;
    }

  private:
    Load m_left;
    usize m_max_batch;
    usize m_taken = 0;
};

} // namespace sched_domain

#endif // #ifndef SCHED_DOMAIN_SCHED_DOMAIN_HPP
