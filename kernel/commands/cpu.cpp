/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `cpu`: take a CPU offline and bring it back (.agents/tasks/cpu-hotplug),
// and say which CPUs are where.
//
//   cpu status          what runs the kernel (bare metal, a hypervisor or
//                       an emulator), then each possible CPU: online,
//                       active, and its idle thread's state
//   cpu offline <cpu>   task::take_cpu_offline()
//   cpu online <cpu>    task::bring_cpu_online()
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>

#include <string_view>

#include <arch/machine.hpp>
#include <gktl/command.hpp>
#include <gktl/options.hpp>
#include <gktl/report.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    /// How long a Dpc worker of the CPU going offline may take to finish
    /// the callback it is in.
    constexpr auto STOP_WORKERS_WITHIN = Seconds(5);


    auto to_string(task::IdleThread::State const state) -> char const * {
        switch (state) {
            case task::IdleThread::State::Booting:
                return "booting";
            case task::IdleThread::State::Running:
                return "running";
            case task::IdleThread::State::Leaving:
                return "leaving";
            case task::IdleThread::State::Offline:
                return "offline";
            case task::IdleThread::State::Suspend:
                return "suspend";
        }
        return "?";
    }

    auto status() -> int {
        log::info("machine: {}", arch::to_string(arch::machine_kind()));
        auto const active = task::Scheduler::peek_active_mask();
        cpu_possible_mask().for_each([&active](CpuNum const cpu) {
            log::info("cpu {}: {}, {}, idle thread {}", cpu,
                      Cpu::is_online(cpu) ? "online" : "not online",
                      active.test(cpu) ? "active" : "inactive",
                      to_string(task::IdleThread::Current::get(cpu).state()));
        });
        return 0;
    }

    auto cpu_command(int argc, char **argv) -> int {
        constexpr std::string_view VERBS[] = {"status", "offline", "online"};
        auto options = gktl::Options{"cpu"};
        options.add<std::string_view>("verb").required().choices(VERBS).help(
            "status lists the CPUs, offline and online move one");
        options.add<u64>("cpu").help("the CPU to take offline or bring online");

        auto const parsed = options.parse(argc, argv);
        if (parsed.is_err()) {
            gktl::reject("cpu", options, parsed.unwrap_err());
            return 1;
        }
        auto const values = parsed.unwrap();
        auto const which = values.get<std::string_view>("verb");
        if (which == "status") {
            return status();
        }
        // Moving a CPU names it; status has nothing to name.
        if (!values.has("cpu")) {
            log::error("cpu: verb={} needs cpu=<number>", which);
            gktl::print_usage("cpu", options.descriptors());
            return 1;
        }
        auto const cpu = static_cast<CpuNum>(values.get<u64>("cpu"));
        auto const result =
            which == "offline"
                ? task::take_cpu_offline(
                      cpu, time::Deadline::at(time::SteadyClock::now() + STOP_WORKERS_WITHIN))
                : task::bring_cpu_online(cpu);
        if (result != Status::Ok) {
            log::warn("cpu {} {}: {}", which, cpu, to_string(result));
            return 1;
        }
        log::info("cpu {} {}: ok", which, cpu);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(Cpu, "cpu", ours::cpu_command,
             "cpu verb=<status|offline|online> [cpu=<N>]: list the CPUs, or take one offline "
             "and bring it back");
