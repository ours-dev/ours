/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Where the scheduling domains come from: the memory manager's table of which
/// node each CPU is on, read once. This is the only file of the scheduler that
/// knows the table exists. And where the policy that steers the walk of them
/// lives: the value of each knob, its first value from the boot options.
#include <ours/assert.hpp>
#include <ours/boot/options.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/task/scheduler_domains.hpp>

#include <array>
#include <atomic>
#include <string_view>
#include <utility>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours::task {
namespace {
    using sched_domain::Knob;

    /// Written once by build_all_domains(), while one CPU runs, and only read
    /// after. Indexed by the CPU that owns the tiers. Empty from the first
    /// instruction, by constinit, because a look for work may come before the
    /// hook below has run.
    constinit std::array<sched_domain::Domains, MAX_CPU> DOMAINS;

    /// The value of each knob, indexed by sched_domain::Knob. Zero from the
    /// first instruction, which for `enabled` is off: a look for work that
    /// comes before the hook below has run finds nothing, as it does with no
    /// domains. Each is a word of its own; see StealPolicy.
    constinit std::array<std::atomic<usize>, sched_domain::NUM_KNOBS> KNOBS{};

    /// The boot options are the knobs, one for one and in the knobs' order:
    /// `kernel.sched.steal.` and the knob's name. The options are parsed where
    /// the knobs are not known, by index, so a mismatch would give a value to
    /// the wrong knob without a word; this is why it is a compile-time check.
    constexpr auto options_match_knobs() -> bool {
        constexpr std::string_view PREFIX{"kernel.sched.steal."};
        if (boot::BootOptions::NUM_STEAL_OPTIONS != sched_domain::NUM_KNOBS) {
            return false;
        }
        for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
            auto const option = boot::BootOptions::STEAL_OPTIONS[i];
            auto const name = sched_domain::knob_info(static_cast<Knob>(i)).name;
            if (!option.starts_with(PREFIX) || option.substr(PREFIX.size()) != name) {
                return false;
            }
        }
        return true;
    }
    static_assert(options_match_knobs(),
                  "BootOptions::STEAL_OPTIONS must name the sched_domain knobs, in their order");
    /// The boot options read the first one as a boolean and the rest as counts.
    static_assert(std::to_underlying(Knob::Enabled) == 0, "the boolean option is the first");

    /// The first value of every knob: its default, then what the image's
    /// command line says of it, if that is a value the knob accepts. One that
    /// is not is said and left at the default, like any other boot option. The
    /// defaults are first guesses to be measured (item 24 of
    /// docs/repairs/261103/kernel-task-sched.md).
    auto init_knobs() -> void {
        auto const &options = boot::options();
        for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
            auto const knob = static_cast<Knob>(i);
            auto const info = sched_domain::knob_info(knob);
            KNOBS[i].store(info.default_value, std::memory_order_relaxed);

            auto const given = options.steal_option(i);
            if (given.is_none()) {
                continue;
            }
            if (!StealPolicy::set(knob, given.value())) {
                log::error("boot: {}={} is not between {} and {}, keeping {}",
                           boot::BootOptions::STEAL_OPTIONS[i], given.value(), info.lowest,
                           info.highest, info.default_value);
            }
        }
    }

    auto build_all_domains() -> void {
        auto const &possible = Cpu::possible_mask();
        DEBUG_ASSERT(possible.any(), "the table is built before the possible CPUs are known");
        possible.for_each([&](CpuNum const cpu) {
            DOMAINS[cpu] = sched_domain::build_domains(
                cpu, possible, [](CpuNum const other) { return mm::CpuNodeMap::node_of(other); });
        });
    }

    auto init_steal() -> void {
        init_knobs();
        build_all_domains();
    }

    /// After the MADT parse has filled the possible mask and the node table
    /// has placed every CPU in it (CpuNodeMap::claim_possible(), at
    /// InitLevel::Irq + 1), and before the other CPUs start: main.cpp announces
    /// Threading, which runs this, and only then creates the thread that calls
    /// init_platform() and with it x86_bring_aps(). The boot options were
    /// parsed before any level.
    GKTL_INIT_HOOK(InitSteal, init_steal, gktl::InitLevel::Threading);
} // namespace

auto domains_of(CpuNum const cpu) -> sched_domain::Domains const & {
    DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
    return DOMAINS[cpu];
}

auto StealPolicy::get(sched_domain::Knob const knob) -> usize {
    DEBUG_ASSERT(std::to_underlying(knob) < sched_domain::NUM_KNOBS, "not a knob");
    return KNOBS[std::to_underlying(knob)].load(std::memory_order_relaxed);
}

auto StealPolicy::set(sched_domain::Knob const knob, usize const value) -> bool {
    // A value that is not a knob's is refused too: nothing is in the range of
    // a knob that does not exist.
    if (!sched_domain::knob_accepts(knob, value)) {
        return false;
    }
    KNOBS[std::to_underlying(knob)].store(value, std::memory_order_relaxed);
    return true;
}
} // namespace ours::task
