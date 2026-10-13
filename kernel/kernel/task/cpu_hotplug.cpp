/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/dpc.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock_source.hpp>
#include <ours/time/timer_queue.hpp>

namespace ours::task {
namespace {
    /// One CPU goes offline or comes back at a time: each step below
    /// assumes the CPU stays as the step before left it.
    sync::Mutex HOTPLUG_LOCK;

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
    auto const status = IdleThread::Current::get(cpu).transition(IdleThread::State::Offline,
                                                                 time::Deadline::infinite());
    if (status != Status::Ok) {
        // Refused before it was asked for: the CPU is as it was.
        DpcRunner::start_workers(cpu);
        return status;
    }
    time::TimerQueue::take_over_from(cpu);
    DpcRunner::take_over_from(cpu);
    return Status::Ok;
}

auto bring_cpu_online(CpuNum const cpu) -> Status {
    if (!is_possible(cpu)) {
        return Status::InvArgs;
    }

    Guard<sync::Mutex> guard{HOTPLUG_LOCK};
    auto &idler = IdleThread::Current::get(cpu);
    if (idler.state() != IdleThread::State::Offline) {
        return Status::BadState;
    }
    if (auto const status =
            idler.transition(IdleThread::State::Running, time::Deadline::infinite());
        status != Status::Ok) {
        return status;
    }
    DpcRunner::start_workers(cpu);
    return Status::Ok;
}
} // namespace ours::task
