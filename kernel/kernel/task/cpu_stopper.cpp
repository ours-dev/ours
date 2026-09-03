#include <ktl/new.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/status.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/cpu_stopper.hpp>
#include <ours/task/thread.hpp>

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
    Work work{.fn = fn, .caller = caller_pc(), .done = &done, .queue_hook = {}};

    if (!queue_work_cpu(cpu, work)) {
        return Status::NotFound;
    }

    task::Thread::Current::reschedule();
    done.completion.wait();
    return Status::Ok;
}

auto CpuStopper::stop_cpus(CpuMask const &cpus, Fn fn) -> Status {
    Done done{.retval = 0, .num_todo = static_cast<u32>(cpus.count()), .completion{}};

    {
        Guard guard{STOP_CPUS_MUTEX};
        if (!queue_work_cpus(cpus, fn, &done)) {
            return Status::NotFound;
        }
        done.completion.wait();
    }

    return Status::Ok;
}

auto CpuStopper::stop_cpu_nowait(CpuNum cpu, Fn fn, Work *work) -> bool {
    if (!work) {
        // Intentionally not tracked for release: the work is intrusive-listed
        // until its worker runs it, and a nowait caller has no handle to free
        // it through.
        work = new (mm::GAF_KERNEL) Work();
        work->fn = fn;
        work->caller = caller_pc();
        work->done = nullptr;
    }
    return queue_work_cpu(cpu, *work);
}

auto CpuStopper::stop_cpus_nowait(CpuMask const &cpus, Fn fn, Work *works) -> bool {
    if (!works) {
        works = new (mm::GAF_KERNEL) Work[cpus.count()];
    }
    return queue_work_cpus(cpus, fn, nullptr);
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
        // This stopper is enabled so wakes up it to execute |work|
        m_worker->resume();
    }

    return is_enabled;
}

auto CpuStopper::queue_work_cpu(CpuNum cpu, Work &work) -> bool {
    return Current::get(cpu).queue_work(work);
}

auto CpuStopper::queue_work_cpus(CpuMask const &cpus, Fn fn, Done *done) -> bool {
    bool is_queued = false;

    task::AutoPreemptDisabler preemption_disabler{};
    cpus.for_each([&is_queued, fn, done](CpuNum cpu) {
        auto &stopper = Current::get(cpu);
        stopper.m_canary.verify<Self>();

        std::construct_at(&stopper.m_stop_work, fn, caller_pc(), done);
        if (stopper.queue_work(stopper.m_stop_work)) {
            is_queued = true;
        }
    });

    return is_queued;
}

auto CpuStopper::worker_thread() -> void {
    Work *work = nullptr;
    while (1) {
        {
            Guard guard{m_lock};
            if (!m_works.empty()) {
                work = &*m_works.front();
                m_works.pop_front();
            }
        }

        if (!work) {
            break;
        }

        auto [fn, caller, done, _] = *work;
        m_caller = caller;
        m_fn = fn;

        {
            task::AutoPreemptDisabler preemption_disabler{};
            auto const retval = fn();
            if (done) {
                done->retval = retval;
                done->completion.signal();
            }
        }

        m_fn = {};
        m_caller = {};
    };
}

} // namespace ours
