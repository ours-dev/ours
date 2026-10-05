/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/irq/mod.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sampler/mod.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <arch/interrupt.hpp>
#include <prof/sampler.hpp>

namespace ours::sampler {
namespace {
    using Timers = mm::CpuLocal::PerCpu<time::Timer>;

    /// Serialises start() and stop() against each other. A sleeping lock on
    /// purpose: both hold it across IpiService::sync_exec(), which needs
    /// interrupts on. Nothing on the sampling path takes it.
    sync::Mutex g_control_mutex;

    /// One sampling timer per CPU. Allocated by the first start() rather than
    /// at boot, so a kernel nobody profiles never has them, and kept after
    /// that: a Timer is a few words, and a second session reuses them.
    Timers g_timers TA_GUARDED(g_control_mutex);

    /// What arm_this_cpu() needs. sync_exec() hands its task one pointer, and
    /// returns only once every CPU has run it, so this can live on the
    /// caller's stack.
    struct ArmRequest final {
        Timers *timers;
        Duration period;
    };

    /// The context the timer interrupted, in the terms prof::Sampler takes.
    ///
    /// Everything here is the kernel's own knowledge: the frame the interrupt
    /// preamble recorded, the CPU and the thread it is running, the time. It
    /// is gathered here and handed to the library, rather than the library
    /// asking an arch seam for it, because none of it is a fact about the
    /// architecture -- the frame's accessors are the one arch-specific part,
    /// and they belong to the frame.
    auto interrupted_context(arch::IrqFrame const &frame) -> prof::SampleContext {
        return prof::SampleContext{
            .pc = frame.pc(),
            .sp = frame.sp(),
            .bp = frame.fp(),
            .tid = static_cast<u32>(task::Thread::Current::tid()),
            .cpu = static_cast<u32>(Cpu::id()),
            .timestamp = static_cast<u64>(time::SteadyClock::now().time_since_epoch().count()),
            .from_uspace = frame.from_user(),
        };
    }

    /// The timer's handler: interrupt context, on the CPU that armed it.
    ///
    /// It goes quiet as soon as the session stops rather than waiting for
    /// stop() to cancel it, which is the same answer either way: a sample
    /// taken after the state left Running would be refused by
    /// record_sample() anyway, and not re-arming saves the CPU one more
    /// pointless interrupt before the cancel lands.
    auto on_sample_due(Duration period, time::Timer *timer) -> time::Timer::HandlerResult {
        auto &profiler = prof::Sampler::get();
        if (!profiler.is_running()) {
            return time::Timer::HandlerResult::Handled;
        }

        // The timer interrupt took the interrupt preamble like any other
        // vector, so the frame it interrupted is this CPU's current frame.
        if (auto const *frame = irq::current_frame(); frame != nullptr) {
            profiler.record_sample(interrupted_context(*frame));
        }

        timer->forward(period);
        return time::Timer::HandlerResult::Restart;
    }

    /// sync_exec() task: arms this CPU's timer. Timer::activate() queues on
    /// the calling CPU's TimerQueue, so running this on each CPU is what puts
    /// one timer on each.
    auto arm_this_cpu(void *context) -> void {
        auto const *request = static_cast<ArmRequest const *>(context);
        auto const period = request->period;
        auto const first = time::SteadyClock::now() + period;
        request->timers->ref_mut().activate(time::Deadline::at(first), &on_sample_due, period);
    }

    /// sync_exec() task: cancels this CPU's timer.
    ///
    /// Run here, on the timer's own CPU with interrupts off, the cancel
    /// cannot overlap the handler: the handler runs only in this CPU's timer
    /// interrupt, which cannot start while this runs. So the cancel never has
    /// to wait, and whether it found the timer queued or the handler had
    /// already declined to re-arm, the timer is off once it returns.
    auto disarm_this_cpu(void *context) -> void {
        auto *timers = static_cast<Timers *>(context);
        // Which of the two it was does not matter; see above.
        auto _ = timers->ref_mut().cancel();
    }
} // namespace

auto start(Duration period) -> Status {
    Guard<sync::Mutex> guard(g_control_mutex);

    if (period < MIN_PERIOD) {
        return Status::InvArgs;
    }
    if (!g_timers) {
        g_timers = mm::CpuLocal::allocate<time::Timer>(1, alignof(time::Timer));
        if (!g_timers) {
            return Status::NoMemory;
        }
    }

    // Running first, timers second: a timer that fires the moment it is
    // armed must already find the session running, or its first sample is
    // lost and, worse, it declines to re-arm.
    auto const status = prof::Sampler::get().start();
    if (status != Status::Ok) {
        return status;
    }

    auto request = ArmRequest{&g_timers, period};
    IpiService::sync_exec(Cpu::online_mask(), &arm_this_cpu, &request);
    return Status::Ok;
}

/// The cancel is also the guarantee stop() makes to the reader.
///
/// A handler that loaded Running just before Sampler::stop() stored Stopped
/// may still be writing its sample. It runs with interrupts off, so the
/// cancel that sync_exec() sends its CPU is taken only once that handler has
/// returned; and sync_exec() returns only once every CPU has taken it. After
/// that, every handler still to run loads Stopped. So no CPU is mid-write
/// when this returns, which is what lets prof.dump read the buffers.
///
/// The barrier holds only because every writer checks the state and writes
/// with interrupts off. A writer in thread context, as the user-stack target
/// will be, keeps that only if its check and its write sit in one
/// interrupts-off section; otherwise it needs another way to be waited for,
/// such as a count of writers in flight that stop() sleeps on until it
/// drains.
auto stop() -> void {
    Guard<sync::Mutex> guard(g_control_mutex);

    prof::Sampler::get().stop();
    if (g_timers) {
        IpiService::sync_exec(Cpu::online_mask(), &disarm_this_cpu, &g_timers);
    }
}
} // namespace ours::sampler
