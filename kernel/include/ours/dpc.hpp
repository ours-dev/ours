/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_DPC_HPP
#define OURS_DPC_HPP 1

#include <ours/compiler.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/status.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/types.hpp>
#include <ours/types.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours {
/// The tier a `Dpc` is submitted at. `DpcRunner` runs one dedicated
/// worker thread per tier per CPU, so a `Dpc` queued at a higher tier is
/// only guaranteed to *start* ahead of same-CPU work still waiting behind
/// it at a lower tier -- each tier's worker still runs one callback to
/// completion before looking at its next one.
enum class DpcPriority {
    Low,
    Normal,
    High,
    Count,
};

/// A deferred callback, run later on the enqueuing CPU's `DpcRunner`
/// worker thread instead of inline -- e.g. from interrupt or scheduler
/// context, where the work either must not run there or must not block.
///
/// `Dpc` is a bare descriptor, not a self-running task: `DpcRunner`
/// enqueue()/cancel() link and unlink it by reference, never by copy or
/// allocation. The owner is responsible for keeping the object alive and
/// at a fixed address for as long as `state()` reports anything other
/// than `Idle`.
class Dpc final: public ustl::collections::intrusive::ListBaseHook<> {
    using Self = Dpc;

  public:
    /// A plain function pointer, handed the `Dpc` itself; whatever context
    /// the callback needs travels in `arg()`.
    ///
    /// Deliberately not a `std::function`. A `Dpc` is submitted from the
    /// places that must not allocate -- interrupt and scheduler context --
    /// and this kernel has no global `operator new` at all, only a panic
    /// stub. The old shape took `std::bind_front(f, args...)` into a
    /// `std::function`, which is fine only while the bound object fits the
    /// small-buffer optimisation; the tree's one call site binds a member
    /// function pointer (16 bytes on the Itanium ABI) plus a `this`, which
    /// does not, so every thread exit panicked in `operator new`. A
    /// function pointer and a `void *` cannot outgrow anything.
    using Func = auto (*)(Self *) -> void;

    enum class State {
        /// Not queued anywhere; safe to destroy, move out of, or
        /// re-enqueue.
        Idle,

        /// Linked into some `DpcRunner` queue, not yet running.
        Queued,

        /// Popped off its queue and executing `Callback` right now.
        Running,
    };

    Dpc(Func func, void *arg = nullptr) : m_func(func), m_arg(arg) {
        DEBUG_ASSERT(m_func, "Dpc constructed without a callback");
    }

    /// The context handed to the constructor, typed at the use site rather
    /// than on the class so that `Dpc` stays one type no matter what it
    /// carries -- which is what lets `DpcRunner` keep a single intrusive list
    /// of them.
    template <typename T>
    [[nodiscard]]
    auto arg() const -> T * {
        return static_cast<T *>(m_arg);
    }

    ~Dpc() {
        DEBUG_ASSERT(State::Idle == m_state, "Dpc destroyed while still queued or running");
    }

    Dpc(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    [[nodiscard]]
    auto state() const -> State {
        return m_state;
    }

  private:
    friend class DpcRunner;

    State m_state{State::Idle};

    /// Where this `Dpc` is currently linked, valid only while `m_state`
    /// is not `Idle`. Lets `DpcRunner::cancel()` find the right queue
    /// without the caller having to remember what it enqueued onto.
    CpuNum m_owner_cpu{};
    DpcPriority m_owner_priority{};

    Func m_func;
    void *m_arg;

    FORCE_INLINE
    auto invoke() -> void {
        m_func(this);
    }
};

/// Per-CPU home for `Dpc` worker threads: one `mm::CpuLocal` instance per
/// CPU, one worker thread per `DpcPriority` tier on each. `enqueue()`
/// always resolves through the *calling* CPU's instance, so a `Dpc`
/// always runs on the CPU that submitted it -- there is deliberately no
/// "run this on CPU N" option yet.
class DpcRunner {
    using Self = DpcRunner;

  public:
    using Current = mm::CpuLocal::Current<DpcRunner>;
    using Level = DpcPriority;

    /// Builds |cpu|'s runner. Storage only -- no threads are spawned, so this
    /// is safe for a CPU that has not been started yet, and must run for every
    /// possible CPU before anything can enqueue.
    static auto init(CpuNum cpu) -> void;

    /// Starts |cpu|'s worker threads, one per `Level`.
    ///
    /// Separate from init() because it spawns, and a spawned thread pinned to
    /// a CPU cannot be placed until that CPU is online: resume() routes
    /// through select_target_cpu(), which only ever answers with an online
    /// CPU, so pinning a worker to a secondary before init_platform() has
    /// started it leaves the selection with an empty candidate set.
    static auto start_workers(CpuNum cpu) -> void;

    /// Stops |cpu|'s worker threads, so that no thread pinned to |cpu| is
    /// left to run there: what taking the CPU offline needs first. Each
    /// worker leaves its loop at the next Dpc boundary; a Dpc it has not
    /// started stays queued, as does every one enqueued on |cpu| from now
    /// on, until start_workers() gives the CPU workers again. Waits for each
    /// worker until |deadline|: TimeOut leaves the ones not yet out
    /// stopping, and a later call waits for them again. BadState when |cpu|
    /// has no workers.
    ///
    /// The workers are not joined: a thread's exit queues the Dpc that reaps
    /// it on its own CPU's Normal queue, which the Normal worker would be
    /// waiting on itself to run. Each worker frees itself through that Dpc
    /// once the CPU has a Normal worker again.
    ///
    /// The caller serialises this with start_workers() for the same CPU.
    [[nodiscard]]
    static auto stop_workers(CpuNum cpu, time::Deadline deadline) -> Status;

    /// Moves every Dpc queued on |cpu|, whose workers are stopped, onto the
    /// calling CPU's queue of the same level, where this CPU's workers run
    /// them: |cpu| is going offline, and they would otherwise wait for it.
    /// Each Dpc's owner becomes this CPU, so cancel() finds it here. A Dpc a
    /// thread queued as it exited on |cpu| (Thread::Current::exit()) may run
    /// here: by then that thread has made its last switch, since |cpu| has
    /// stopped. The caller serialises this with start_workers() for |cpu|.
    static auto take_over_from(CpuNum cpu) -> void;

    /// Queues `dpc` on the calling CPU's `level` worker.
    ///
    /// `dpc` must be `Idle` on entry and must stay alive and at a fixed
    /// address until `state()` reports `Idle` again -- i.e. until it has
    /// either run or been `cancel()`ed.
    [[nodiscard]]
    static auto enqueue(Dpc &dpc, Level level) -> Status;

    /// Removes `dpc` before it runs, if still possible.
    ///
    /// Returns `Ok` if it was queued and has now been unlinked,
    /// `NotFound` if it was already `Idle` (nothing to cancel), or
    /// `BadState` if it is `Running` right now -- the caller must
    /// synchronize with completion some other way in that case.
    ///
    /// The caller must not race this against a concurrent `enqueue()`
    /// of the same `dpc`; like `enqueue()` itself, pairing these calls
    /// correctly for one `Dpc` is the caller's responsibility, not
    /// something the queue locks arbitrate.
    [[nodiscard]]
    static auto cancel(Dpc &dpc) -> Status;

  private:
    /// The only code outside `Dpc` itself allowed to touch its
    /// bookkeeping fields. `Queue`, as `DpcRunner`'s own nested class,
    /// reaches these through their shared enclosing scope rather than
    /// needing its own friendship into `Dpc` -- `Dpc` is defined before
    /// `Queue` even has a name to grant that to.
    static auto mark_queued(Dpc &dpc, CpuNum cpu, Level level) -> void {
        dpc.m_state = Dpc::State::Queued;
        dpc.m_owner_cpu = cpu;
        dpc.m_owner_priority = level;
    }

    static auto mark_running(Dpc &dpc) -> void {
        dpc.m_state = Dpc::State::Running;
    }

    static auto mark_idle(Dpc &dpc) -> void {
        dpc.m_state = Dpc::State::Idle;
    }

    [[nodiscard]]
    static auto state_of(Dpc const &dpc) -> Dpc::State {
        return dpc.m_state;
    }

    [[nodiscard]]
    static auto owner_cpu_of(Dpc const &dpc) -> CpuNum {
        return dpc.m_owner_cpu;
    }

    [[nodiscard]]
    static auto owner_level_of(Dpc const &dpc) -> Level {
        return dpc.m_owner_priority;
    }

    /// One priority tier's worker: its own thread, its own queue. Fully
    /// self-contained -- `start()` spawns and pins the thread, callers
    /// never touch the thread handle directly.
    class Queue {
      public:
        /// Spawns the worker thread for this queue, pinned to `cpu`. The
        /// queue must have no worker: none started yet, or stop() done.
        [[nodiscard]]
        auto start(char const *name, CpuNum cpu) -> Status;

        /// Tells the worker to leave its loop and waits until it has; see
        /// DpcRunner::stop_workers().
        [[nodiscard]]
        auto stop(time::Deadline deadline) -> Status;

        /// Moves every Dpc on |from| to the end of this queue, owned by
        /// |cpu| at |level|, and wakes this queue's worker if any moved.
        auto take_over(Queue &from, CpuNum cpu, Level level) -> void;

        auto enqueue(Dpc &dpc, CpuNum cpu, Level level) -> void;

        [[nodiscard]]
        auto cancel(Dpc &dpc) -> Status;

      private:
        auto do_work() -> i32;

        sync::Event m_event;

        /// Signalled by the worker as it leaves its loop, the last thing it
        /// does to this queue. Unsignalled by start().
        sync::Event m_stopped;

        sync::SpinLock m_lock;

        /// The worker, or null when the queue has none. Written by start()
        /// and stop(), which their callers serialise.
        task::Thread *m_thread{};

        /// stop() has asked the worker to leave. Read by the worker in the
        /// same critical section that pops a Dpc, so a Dpc is either popped
        /// and run by this worker or left for the next one.
        bool m_stopping TA_GUARDED(m_lock) = false;

        ustl::collections::intrusive::List<Dpc> m_list TA_GUARDED(m_lock);
    };

    Queue m_dpc_queues[static_cast<usize>(Level::Count)];
};

} // namespace ours

#endif // #ifndef OURS_DPC_HPP
