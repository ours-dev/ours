/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/mp.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/dpc.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>
#include <ours/time/timer_queue.hpp>

#include <arch/spin_hint.hpp>
#include <logz4/log.hpp>
#include <sampler/hotplug.hpp>

namespace ours::task {
namespace {
    /// One CPU goes offline or comes back at a time: each step below
    /// assumes the CPU stays as the step before left it.
    sync::Mutex HOTPLUG_LOCK;

#ifdef CONFIG_CPU_HOTPLUG
    /// How long a CPU started again is given to arrive: far longer than it
    /// takes, and short enough that one that never will does not hang the
    /// caller, as x86_bring_aps() bounds its wait at boot.
    constexpr auto COMING_BACK_WITHIN = Seconds(1);
#endif // #ifdef CONFIG_CPU_HOTPLUG

#ifdef CONFIG_CPU_HOTPLUG
    /// The sampling sessions' control lock, for as long as one of these
    /// lives: no session starts or stops while a CPU changes state.
    class SamplerHold final {
      public:
        SamplerHold() {
            sampler::begin_hotplug();
        }
        ~SamplerHold() {
            sampler::end_hotplug();
        }
        SamplerHold(SamplerHold const &) = delete;
        auto operator=(SamplerHold const &) -> SamplerHold & = delete;
    };
#endif // #ifdef CONFIG_CPU_HOTPLUG

    auto is_possible(CpuNum const cpu) -> bool {
        return cpu < MAX_CPU && cpu_possible_mask().test(cpu);
    }
} // namespace

/// The workers first: none of the CPU's own threads may be left to wake
/// there once it is gone, and they are the only ones it has besides its idle
/// thread. Its timers and Dpcs last, once it has stopped and nothing on it
/// can touch its queues.
auto take_cpu_offline(CpuNum const cpu, time::Deadline const deadline) -> Status {
    if (!is_possible(cpu)) {
        return Status::InvArgs;
    }
#ifndef CONFIG_CPU_HOTPLUG
    static_cast<void>(deadline);
    return Status::NotSupported;
#else

    Guard<sync::Mutex> guard{HOTPLUG_LOCK};
    if (cpu == time::ClockSource::Default::cpunum()) {
        return Status::NotSupported;
    }
    if (!Cpu::is_online(cpu)) {
        return Status::BadState;
    }
    auto others = Scheduler::peek_active_mask();
    others.set(cpu, false);
    if (others.none()) {
        return Status::BadState;
    }

    if (auto const status = DpcRunner::stop_workers(cpu, deadline); status != Status::Ok) {
        return status;
    }
    SamplerHold const sampler_hold;
    auto const status = IdleThread::Current::get(cpu).transition(IdleThread::State::Offline,
                                                                 time::Deadline::infinite());
    if (status != Status::Ok) {
        // Refused before it was asked for: the CPU is as it was.
        DpcRunner::start_workers(cpu);
        return status;
    }

    // The CPU halts straight after it says it is offline; reset it only once
    // it has, so that no lock it took on the way is left held.
    auto &idler = IdleThread::Current::get(cpu);
    while (!idler.has_halted()) {
        arch::spin_hint();
    }
    if (auto const held = arch_deactivate_cpu(cpu); held != Status::Ok) {
        // Halted all the same; bringing it back starts with an INIT anyway.
        log::warn("CPU[{}] did not take the INIT that holds it: {}", cpu, to_string(held));
    }
    time::TimerQueue::take_over_from(cpu);
    DpcRunner::take_over_from(cpu);
    return Status::Ok;
#endif // #ifndef CONFIG_CPU_HOTPLUG
}

auto bring_cpu_online(CpuNum const cpu) -> Status {
    if (!is_possible(cpu)) {
        return Status::InvArgs;
    }
#ifndef CONFIG_CPU_HOTPLUG
    return Status::NotSupported;
#else
    Guard<sync::Mutex> guard{HOTPLUG_LOCK};
    auto &idler = IdleThread::Current::get(cpu);
    if (idler.state() != IdleThread::State::Offline) {
        return Status::BadState;
    }
    SamplerHold const sampler_hold;
    if (auto const status =
            idler.transition(IdleThread::State::Running,
                             time::Deadline::at(time::SteadyClock::now() + COMING_BACK_WITHIN));
        status != Status::Ok) {
        return status;
    }
    DpcRunner::start_workers(cpu);
    return Status::Ok;
#endif // #ifndef CONFIG_CPU_HOTPLUG
}
} // namespace ours::task
