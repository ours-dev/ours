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

    /// The context handed to the constructor. Named after Zircon's
    /// `Dpc::arg<ArgType>()`, and typed at the use site rather than on the
    /// class so that `Dpc` stays one type no matter what it carries -- which
    /// is what lets `DpcRunner` keep a single intrusive list of them.
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

    FORCE_INLINE auto invoke() -> void {
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

    /// Starts this CPU's worker threads, one per `Level`. Every CPU must
    /// call this on itself once, during its own bring-up, after
    /// threading is up.
    static auto init() -> void;

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
        /// Spawns the worker thread for this queue, pinned to `cpu`.
        [[nodiscard]]
        auto start(char const *name, CpuNum cpu) -> Status;

        auto enqueue(Dpc &dpc, CpuNum cpu, Level level) -> void;

        [[nodiscard]]
        auto cancel(Dpc &dpc) -> Status;

      private:
        auto do_work() -> i32;

        sync::Event m_event;
        sync::SpinLock m_lock;
        task::Thread *m_thread{};
        ustl::collections::intrusive::List<Dpc> m_list TA_GUARDED(m_lock);
    };

    Queue m_dpc_queues[static_cast<usize>(Level::Count)];
};

} // namespace ours

#endif // #ifndef OURS_DPC_HPP
