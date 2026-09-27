/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_TASK_CPU_STOPPER_HPP
#define OURS_TASK_CPU_STOPPER_HPP 1

#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/types.hpp>

#include <gktl/canary.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/function.hpp>

namespace ours {
class CpuStopper {
    using Self = CpuStopper;

  public:
    enum class State {
        Disabled,
        Enabled,
        Processing,
    };

    /// What a stopper runs. Queued under m_lock with interrupts off, so it
    /// must not allocate: a Function keeps its target inline and refuses at
    /// compile time one larger than 24 bytes. Move-only, so a Work owns the
    /// one it runs; see stop_cpus() for how one fn reaches several CPUs.
    using Fn = ustl::function::Function<auto()->i32>;

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

    /// Stops |cpu|, runs |work| on it, and waits for it to finish.
    /// Linux: stop_one_cpu().
    static auto stop_cpu(CpuNum cpu, Fn work) -> Status;

    /// Runs |work| on every CPU in |cpus| at once, and waits for all of them.
    ///
    /// |work| stays in this call's frame, and each CPU's Work holds a lambda
    /// that calls it through a pointer: a move-only Function cannot be copied
    /// into several Works, and the frame outlives every call because this
    /// waits for all of them. So |work| runs concurrently on every CPU through
    /// one shared target, as Linux's stop_cpus() runs one fn with one arg, and
    /// must be safe for that -- a mutable lambda here would race with itself.
    static auto stop_cpus(CpuMask const &cpus, Fn work) -> Status;

    /// Queues |work| on |cpu| and returns without waiting. |buf| is the Work
    /// to queue it in, which must stay alive until it has run; nullptr
    /// allocates one that is never freed. Linux: stop_one_cpu_nowait().
    ///
    /// There is no stop_cpus_nowait(). One move-only |work| cannot be handed
    /// to several CPUs without a frame that outlives them all, which a nowait
    /// call does not have; Linux has no such call either.
    static auto stop_cpu_nowait(CpuNum cpu, Fn work, Work *buf = nullptr) -> bool;

  private:
    static auto queue_work_cpus(CpuMask const &cpus, Fn const &fn, Done *done) -> bool;

    static auto queue_work_cpu(CpuNum cpu, Work &work) -> bool;

    auto queue_work(Work &work) -> bool;

    auto worker_thread() -> void;

    gktl::Canary m_canary{this};
    State m_state;
    sync::SpinLock m_lock;
    task::Thread *m_worker;

    USTL_DECLARE_HOOK_OPTION(Work, Work::queue_hook, WorkHookOption);
    USTL_DECLARE_LIST(Work, WorkQueue, WorkHookOption);
    WorkQueue m_works;

    Work m_stop_work; /// For stop cpus.
    /// The work running now, for diagnostics -- Linux's cpu_stopper::fn and
    /// ::caller. A pointer, as the Work owns the Function.
    usize m_caller;
    Fn const *m_fn = nullptr;

    /// Forbids times of stop request on CPUs in same duration.
    static sync::Mutex STOP_CPUS_MUTEX;
};

} // namespace ours

#endif // #ifndef OURS_TASK_CPU_STOPPER_HPP