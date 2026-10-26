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
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    /// How long a Dpc worker of the CPU going offline may take to finish
    /// the callback it is in.
    constexpr auto STOP_WORKERS_WITHIN = Seconds(5);

    /// The most digits a CPU number is given in.
    constexpr usize MAX_DIGITS = 4;

    /// |text| as a decimal CPU number, or none.
    auto parse_cpu(std::string_view const text) -> ustl::Option<CpuNum> {
        if (text.empty() || text.size() > MAX_DIGITS) {
            return ustl::NONE;
        }
        auto value = CpuNum{0};
        for (auto const c: text) {
            if (c < '0' || c > '9') {
                return ustl::NONE;
            }
            value = static_cast<CpuNum>(value * 10 + static_cast<CpuNum>(c - '0'));
        }
        return ustl::Option<CpuNum>(value);
    }

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
        if (argc == 2 && std::string_view(argv[1]) == "status") {
            return status();
        }
        if (argc != 3) {
            log::warn("usage: cpu status | cpu offline <cpu> | cpu online <cpu>");
            return 1;
        }
        auto const which = std::string_view(argv[1]);
        auto const cpu = parse_cpu(argv[2]);
        if (cpu.is_none() || (which != "offline" && which != "online")) {
            log::warn("usage: cpu status | cpu offline <cpu> | cpu online <cpu>");
            return 1;
        }
        auto const result =
            which == "offline"
                ? task::take_cpu_offline(cpu.value(), time::Deadline::at(time::SteadyClock::now() +
                                                                         STOP_WORKERS_WITHIN))
                : task::bring_cpu_online(cpu.value());
        if (result != Status::Ok) {
            log::warn("cpu {} {}: {}", which, cpu.value(), to_string(result));
            return 1;
        }
        log::info("cpu {} {}: ok", which, cpu.value());
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(Cpu, "cpu", ours::cpu_command,
             "cpu status | offline <cpu> | online <cpu>: list the CPUs, or take one offline and "
             "bring it back");
