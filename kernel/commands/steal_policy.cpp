/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `steal.policy`: the numbers that steer a CPU when it looks for threads to
// take (task::StealPolicy), and how often each CPU has looked and
// taken, and how long its looks took, whichever CPU made them.
//
//   steal.policy                 each knob with its value, default and range,
//                                then each online CPU's looks, takes and the time
//                                it spent in steal_work_from()
//   steal.policy <name> <value>  sets a knob; a name that is none, or a value
//                                out of the knob's range, changes nothing and is
//                                logged as an error, so that a script that sweeps
//                                a knob sees the refusal
//
// A setting takes effect at the next look of any CPU; nothing has to stop. A CPU
// that is halted looks when something wakes it, and a change of a setting does
// not: it is used the next time that CPU is woken (item 23 of
// docs/repairs/261103/kernel-task-sched.md). It is the way to sweep a threshold
// or a batch without building another image (kernel.sched.steal.<name> on the
// command line gives the first value).
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/scheduler_domains.hpp>

#include <string_view>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    using sched_domain::Knob;
    using task::Scheduler;
    using task::StealPolicy;

    /// The most digits a value is given in: no knob takes more than 1024.
    constexpr usize MAX_DIGITS = 6;

    /// |text| as a decimal number, or none.
    auto parse_value(std::string_view const text) -> ustl::Option<usize> {
        if (text.empty() || text.size() > MAX_DIGITS) {
            return ustl::NONE;
        }
        usize value = 0;
        for (auto const c: text) {
            if (c < '0' || c > '9') {
                return ustl::NONE;
            }
            value = value * 10 + static_cast<usize>(c - '0');
        }
        return ustl::Option<usize>(value);
    }

    auto show() -> int {
        for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
            auto const knob = static_cast<Knob>(i);
            auto const info = sched_domain::knob_info(knob);
            log::info("steal.policy: {} = {} (default {}, from {} to {})", info.name,
                      StealPolicy::get(knob), info.default_value, info.lowest, info.highest);
        }
        Cpu::online_mask().for_each([](CpuNum const cpu) -> void {
            log::info("steal.policy: cpu {}: looked {} times, took {} threads, {} ns in all, "
                      "longest {} ns",
                      cpu, Scheduler::Current::num_steal_looks(cpu),
                      Scheduler::Current::num_steals(cpu),
                      Scheduler::Current::steal_nanoseconds(cpu),
                      Scheduler::Current::longest_steal_nanoseconds(cpu));
        });
        return 0;
    }

    auto steal_policy_command(int argc, char **argv) -> int {
        if (argc == 1) {
            return show();
        }
        if (argc != 3) {
            log::error("usage: steal.policy | steal.policy <name> <value>");
            return 1;
        }
        Knob knob = Knob::Enabled;
        if (!sched_domain::find_knob(argv[1], knob)) {
            log::error("steal.policy: {} is not a knob; run steal.policy to list them", argv[1]);
            return 1;
        }
        auto const info = sched_domain::knob_info(knob);
        auto const value = parse_value(argv[2]);
        if (value.is_none() || !StealPolicy::set(knob, value.value())) {
            log::error("steal.policy: {} takes a number from {} to {}, not {}", info.name,
                       info.lowest, info.highest, argv[2]);
            return 1;
        }
        log::info("steal.policy: {} = {}", info.name, StealPolicy::get(knob));
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(StealPolicy, "steal.policy", ours::steal_policy_command,
             "steal.policy [<name> <value>]: show the work-stealing knobs, or set one");
