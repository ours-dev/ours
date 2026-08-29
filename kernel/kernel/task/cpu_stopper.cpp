#include <ours/task/cpu_stopper.hpp>

#include <ours/task/thread.hpp>
#include <ours/task/auto_preempt_disabler.hpp>

#include <ours/mm/cpu_local.hpp>

#include <ours/lockdep.hpp>

#include <builtin/instruction-pointer.hpp>

namespace ours {
    using Current = mm::CpuLocal::Current<CpuStopper>;
    
    auto CpuStopper::stop_cpu(CpuNum cpu, Fn fn) -> Status {
        Done done{.retval = 0, .num_todo = 1, .completion{}};
        Work work{.fn = fn, .caller = builtin::return_address(), .done = &done};

        if (!queue_work_cpu(cpu, work)) {
            return Status::NoEntry;
        }

        task::Thread::Current::reschedule();
        done.completion.wait();
        return Status::Ok;
    }

    auto CpuStopper::stop_cpus(CpuMask const &cpus, Fn fn) -> Status {
        Done done{.retval = 0, .num_todo = static_cast<u32>(cpus.count()), .completion{}};

        {
            Guard guard{g_stop_cpus_mutex};
            if (!queue_work_cpus(cpus, fn, &done)) {
                return Status::NoEntry;
            }
            done.completion.wait();
        }

        return Status::Ok;
    }

    auto CpuStopper::stop_cpu_nowait(CpuNum cpu, Fn fn, Work *work) -> bool {
        if (!work) {
            work = new Work();
        }
        return queue_work_cpu(cpu, *work);
    }

    auto CpuStopper::stop_cpus_nowait(CpuMask const &cpus, Fn fn, Work *works) -> bool {
        if (!works) {
            works = new Work[cpus.count()];
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
        cpus.for_each([&is_queued, fn, done] (CpuNum cpu) {
            auto &stopper = Current::get(cpu);
            stopper.m_canary.verify<Self>();

            std::construct_at(&stopper.m_stop_work, fn, builtin::return_address(), done);
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
                    work = &m_works.front();
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