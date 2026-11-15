/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `steal.policy`: the numbers that steer a CPU when it looks for threads to
// take (task::StealPolicy), and how often each CPU has looked and
// taken, and how long its looks took, whichever CPU made them.
//
//   steal.policy                    each knob with its value, default and range,
//                                   then each online CPU's looks, takes and the time
//                                   it spent in steal_work_from()
//   steal.policy name=<n> value=<v> sets a knob; a name that is none, or a value
//                                   out of the knob's range, changes nothing and is
//                                   logged as an error, so that a script that sweeps
//                                   a knob sees the refusal
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
#include <gktl/options.hpp>
#include <gktl/report.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using sched_domain::Knob;
    using task::Scheduler;
    using task::StealPolicy;

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

    auto steal_policy_command(gktl::Invocation const &invocation) -> int {
        auto options = gktl::Options{"steal.policy"};
        options.add<std::string_view>("name").help("the knob to set, as steal.policy lists them");
        options.add<u64>("value").help("the value to set the knob to");
        auto const parsed = options.parse(invocation);
        if (parsed.is_err()) {
            gktl::reject(options, parsed.unwrap_err());
            return 1;
        }
        auto const values = parsed.unwrap();
        if (!values.has("name")) {
            return show();
        }
        // The knob named but nothing to set it to: the pair is one request.
        if (!values.has("value")) {
            log::error("steal.policy: name needs value=<number>");
            gktl::print_usage(options);
            return 1;
        }
        auto const name = values.get<std::string_view>("name");
        Knob knob = Knob::Enabled;
        if (!sched_domain::find_knob(name, knob)) {
            log::error("steal.policy: {} is not a knob; run steal.policy to list them", name);
            return 1;
        }
        auto const info = sched_domain::knob_info(knob);
        auto const value = values.get<usize>("value");
        if (!StealPolicy::set(knob, value)) {
            log::error("steal.policy: {} takes a number from {} to {}, not {}", info.name,
                       info.lowest, info.highest, value);
            return 1;
        }
        log::info("steal.policy: {} = {}", info.name, StealPolicy::get(knob));
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(StealPolicy, "steal.policy", ours::steal_policy_command,
             "steal.policy [name=<knob> value=<N>]: show the work-stealing knobs, or set one");
