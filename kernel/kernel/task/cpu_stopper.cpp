#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/status.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/cpu_stopper.hpp>
#include <ours/task/thread.hpp>

#include <heap/new.hpp>

namespace ours {
namespace {
    /// This port used to reach for <builtin/instruction-pointer.hpp>, which
    /// does not exist in this tree. Level 0 is exact for a direct call site.
    auto caller_pc() -> usize {
        return reinterpret_cast<usize>(__builtin_return_address(0));
    }
} // namespace

using Current = mm::CpuLocal::Current<CpuStopper>;

sync::Mutex CpuStopper::STOP_CPUS_MUTEX;

auto CpuStopper::stop_cpu(CpuNum cpu, Fn fn) -> Status {
    Done done{.retval = 0, .num_todo = 1, .completion{}};
    Work work{.fn = ustl::move(fn), .caller = caller_pc(), .done = &done, .queue_hook = {}};

    if (!queue_work_cpu(cpu, work)) {
        return Status::NotFound;
    }

    task::Thread::Current::reschedule();
    // Not interruptible and with no deadline, wait() answers what signal() was
    // given, and the stopper gives only Ok.
    [[maybe_unused]]
    auto const waited = done.completion.wait();
    DEBUG_ASSERT(Status::Ok == waited);
    return Status::Ok;
}

auto CpuStopper::stop_cpus(CpuMask const &cpus, Fn fn) -> Status {
    Done done{.retval = 0, .num_todo = static_cast<u32>(cpus.count()), .completion{}};

    {
        Guard guard{STOP_CPUS_MUTEX};
        if (!queue_work_cpus(cpus, fn, &done)) {
            return Status::NotFound;
        }
        // Not interruptible and with no deadline, wait() answers what signal() was
        // given, and the stopper gives only Ok.
        [[maybe_unused]]
        auto const waited = done.completion.wait();
        DEBUG_ASSERT(Status::Ok == waited);
    }

    return Status::Ok;
}

auto CpuStopper::stop_cpu_nowait(CpuNum cpu, Fn fn, Work *work) -> bool {
    if (!work) {
        // Intentionally not tracked for release: the work is intrusive-listed
        // until its worker runs it, and a nowait caller has no handle to free
        // it through.
        work = new (mm::GAF_KERNEL) Work();
    }
    // Filled in whether or not the caller supplied |work|. It used to be
    // filled only when allocated here, so a caller's own buffer was queued
    // with whatever fn it happened to hold. Linux's stop_one_cpu_nowait()
    // fills work_buf the same way.
    work->fn = ustl::move(fn);
    work->caller = caller_pc();
    work->done = nullptr;
    return queue_work_cpu(cpu, *work);
}

auto CpuStopper::queue_work(Work &work) -> bool {
    m_canary.verify<Self>();

    bool is_enabled;
    task::AutoPreemptDisabler preemption_disabler{};
    {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        is_enabled = m_state == State::Enabled;
        if (is_enabled) {
            m_works.push_back(work);
        } else if (work.done) {
            work.done->completion.signal(Status::Ok);
        }
    }

    if (is_enabled) {
        // This stopper is enabled so wakes up it to execute |work|. resume()
        // answers only Ok.
        auto _ = m_worker->resume();
    }

    return is_enabled;
}

auto CpuStopper::queue_work_cpu(CpuNum cpu, Work &work) -> bool {
    return Current::get(cpu).queue_work(work);
}

auto CpuStopper::queue_work_cpus(CpuMask const &cpus, Fn const &fn, Done *done) -> bool {
    bool is_queued = false;

    task::AutoPreemptDisabler preemption_disabler{};
    cpus.for_each([&is_queued, &fn, done](CpuNum cpu) {
        auto &stopper = Current::get(cpu);
        stopper.m_canary.verify<Self>();

        // Assigned, not reconstructed over the old Work: the old fn is a
        // Function and is destroyed properly by the assignment. Each CPU
        // gets a forwarder to the caller's |fn|; see stop_cpus().
        auto &work = stopper.m_stop_work;
        work.fn = [shared = &fn] { return (*shared)(); };
        work.caller = caller_pc();
        work.done = done;
        if (stopper.queue_work(work)) {
            is_queued = true;
        }
    });

    return is_queued;
}

auto CpuStopper::worker_thread() -> void {
    while (1) {
        // Declared per iteration. Hoisted out of the loop, it kept the last
        // work once the queue ran dry, and the loop ran that work again --
        // after its done->completion had let a stop_cpu() caller return and
        // take the Work, which lives in that caller's frame, with it.
        // Linux's cpu_stopper_thread() likewise starts each pass from
        // `work = NULL`.
        Work *work = nullptr;
        {
            // IrqSave, as queue_work() takes it: Linux's cpu_stop_get_work()
            // likewise holds stopper->lock with interrupts off, since
            // cpu_stop_queue_work() takes it IrqSave from any context.
            Guard<sync::SpinLock, IrqSave> guard{m_lock};
            if (!m_works.empty()) {
                work = &*m_works.front();
                m_works.pop_front();
            }
        }

        if (!work) {
            break;
        }

        // Read through |work| rather than copied out of it: the Work owns a
        // move-only Function. Nothing touches |work| after signal(), which is
        // what lets a stop_cpu() caller return and take the Work with it.
        auto *const done = work->done;
        m_caller = work->caller;
        m_fn = &work->fn;

        {
            task::AutoPreemptDisabler preemption_disabler{};
            auto const retval = work->fn();
            if (done) {
                done->retval = retval;
                done->completion.signal();
            }
        }

        m_fn = nullptr;
        m_caller = {};
    };
}

} // namespace ours
