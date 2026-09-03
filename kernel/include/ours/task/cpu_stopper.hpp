/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_TASK_CPU_STOPPER_HPP
#define OURS_TASK_CPU_STOPPER_HPP 1

#include <gktl/canary.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/types.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/fn.hpp>

namespace ours {
class CpuStopper {
    using Self = CpuStopper;

  public:
    enum class State {
        Disabled,
        Enabled,
        Processing,
    };

    using Fn = ustl::function::Fn<auto()->i32>;

    struct Done {
        i32 retval;
        u32 num_todo;
        sync::Event completion;
    };

    struct Work {
        Fn fn;
        usize caller;
        Done *done;
        ustl::collections::intrusive::ListMemberHook<> queue_hook;
    };

    /// @brief Stop |cpu| and let it do |work|
    ///
    ///
    static auto stop_cpu(CpuNum cpu, Fn work) -> Status;

    static auto stop_cpus(CpuMask const &cpus, Fn work) -> Status;

    /// |buf| If nullptr to allocate from heap.
    static auto stop_cpu_nowait(CpuNum cpu, Fn work, Work *buf = nullptr) -> bool;

    static auto stop_cpus_nowait(CpuMask const &cpus, Fn work, Work *buf = nullptr) -> bool;

  private:
    static auto queue_work_cpus(CpuMask const &cpus, Fn fn, Done *done) -> bool;

    static auto queue_work_cpu(CpuNum cpu, Work &work) -> bool;

    auto queue_work(Work &work) -> bool;

    auto worker_thread() -> void;

    gktl::Canary m_canary;
    State m_state;
    sync::SpinLock m_lock;
    task::Thread *m_worker;

    USTL_DECLARE_HOOK_OPTION(Work, Work::queue_hook, WorkHookOption);
    USTL_DECLARE_LIST(Work, WorkQueue, WorkHookOption);
    WorkQueue m_works;

    Work m_stop_work; /// For stop cpus.
    usize m_caller;
    Fn m_fn;

    /// Forbids times of stop request on CPUs in same duration.
    static sync::Mutex STOP_CPUS_MUTEX;
};

} // namespace ours

#endif // #ifndef OURS_TASK_CPU_STOPPER_HPP