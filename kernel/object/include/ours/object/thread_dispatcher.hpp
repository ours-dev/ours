/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_THREAD_DISPATCHER_HPP
#define OURS_OBJECT_THREAD_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/exceptionate.hpp>
#include <ours/object/handle.hpp>
#include <ours/syscall.hpp>
#include <ours/task/types.hpp>

#include <gktl/canary.hpp>
#include <ktl/name.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/option.hpp>

namespace ours {
enum class ExceptionType : u32;
enum class PolicyCode : u32;
} // namespace ours

namespace ours::task {
struct ExceptionContext;
enum class ExceptionOutcome : u8;
} // namespace ours::task

namespace ours::object {
/// Rights a freshly created thread handle carries.
constexpr auto RIGHTS_DEFAULT_THREAD = Rights::Basic | Rights::Io | Rights::Property |
                                       Rights::Destroy | Rights::Signal | Rights::ManageThread;

using ThreadDispatcherBase = SoloDispatcher<ThreadDispatcher, RIGHTS_DEFAULT_THREAD, Signals::None>;

class ThreadDispatcher: public ThreadDispatcherBase {
    using Self = ThreadDispatcher;
    using Base = ThreadDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Thread);

    /// Guarded by get_lock().
    ///
    ///   Initial  created, and not started: there is no kernel thread yet, and
    ///            the thread is not on its process's list. Every thread
    ///            starts here, and only start() leaves it.
    ///   Running  started, until it starts to die.
    ///   Dying    killed, and the kernel thread told so, but not yet gone.
    ///   Dead     the kernel thread has exited and left the process; it is
    ///            no longer reachable from this object.
    ///
    /// There is no state for "created, with its kernel thread, not yet
    /// started", and no Suspended. The kernel thread is made at start, under
    /// the process's lock and after the process's state was checked, so a
    /// thread that does not start has nothing to dispose of. Making it at
    /// creation would need a way to dispose of a kernel thread that never
    /// ran, which this kernel does not have.
    enum class State {
        Initial,
        Running,
        Dying,
        Dead,
    };

    enum class ParkedReason : u16 {};

    struct TaskState {
        VirtAddr entry = 0;
        VirtAddr stack = 0;
        usize arg1 = 0;
        usize arg2 = 0;
    };

    /// A thread of |process| that has not started: the dispatcher and
    /// nothing else. The kernel thread is made by start(), so that a thread
    /// whose handle is closed before it ever ran leaves nothing behind (see
    /// State).
    static auto create(ustl::Arc<ProcessDispatcher> process, char const *name,
                       KernelHandle<Self> *out) -> Status;

    /// The user thread the calling kernel thread runs on behalf of, or null
    /// on an ordinary kernel thread. See ProcessDispatcher::current().
    [[nodiscard]]
    static auto current() -> Self *;

    /// Ends the calling user thread; see the definition.
    [[noreturn]]
    static auto exit_current() -> void;

    /// Starts this thread at |state|: the first thread of its process
    /// (|first_thread|, only ProcessDispatcher::start() says so) or a later
    /// one. BadState unless it is Initial (a thread starts once) and its
    /// process can take one (ProcessDispatcher::attach_thread()); InvArgs for
    /// an entry point that is not a user address.
    auto start(TaskState const &state, bool first_thread) -> Status;

    /// The kernel thread this object will run as, made now and bound to the
    /// process's address space. Called by ProcessDispatcher::attach_thread()
    /// with the process's lock held, after the process's state was checked
    /// and before anything else about the start is done, so that a failure
    /// here -- out of memory for the stack -- leaves the process, the thread
    /// and the list exactly as they were. BadState when this is not Initial.
    ///
    /// Takes this thread's own lock, the order attach_thread() uses.
    auto create_kernel_thread() -> Status;

    auto resume() -> void;

    auto suspend() -> void;

    auto activate(bool suspend) -> Status;

    /// Tells this thread to die: a thread that has started moves to Dying and
    /// its kernel thread gets the Kill signal, which it acts on at its next
    /// return towards ring 3. A thread that never started (Initial) is left as
    /// it is, and one already dying or dead is left alone.
    ///
    /// Called by ProcessDispatcher::kill() with the process's lock held; takes
    /// this thread's own lock after it.
    auto kill() -> void;

    FORCE_INLINE
    auto process() -> ProcessDispatcher & {
        return m_process.as_ref_mut();
    }

    /// The read-only form. The syscall path reaches the process through a
    /// `const ThreadDispatcher &` -- it only asks where the vDSO is -- and
    /// without this it would have to cast the constness away to ask.
    FORCE_INLINE
    auto process() const -> ProcessDispatcher const & {
        return m_process.as_ref();
    }

    /// The thread's own exception channel slot, tried before its process's:
    /// task_create_exception_channel on a thread handle binds here. Shut down
    /// as the thread exits (on_exit()).
    [[nodiscard]]
    FORCE_INLINE
    auto exceptionate() -> Exceptionate & {
        return m_exceptionate;
    }

    /// A reference of the caller's own to the process, for an object that
    /// has to hold it: an exception's process handle (exception_get_process).
    [[nodiscard]]
    auto process_ref() const -> ustl::Arc<ProcessDispatcher> {
        return m_process;
    }

    /// The process's koid: a thread belongs to the process it was created in
    /// for its whole life (m_process, set by the constructor and never
    /// changed).
    [[nodiscard]]
    auto related_koid() const -> KoId override;

    /// This thread made a system call without going through the vDSO: the
    /// trap came from |ip|, with |number| in the call-number register. The
    /// process is killed for it (a PolicyError, BadSyscall).
    ///
    /// Its own entry point, rather than the system-call layer naming a
    /// PolicyCode, so that //kernel/lib/syscall keeps to kernel/object's
    /// headers (design 4.7). It is on_policy_exception() with BadSyscall.
    auto on_bad_syscall(u64 number, VirtAddr ip, VirtAddr sp) -> void;

    FORCE_INLINE
    auto mark_active() -> void {
        m_flags |= Flags::Active;
    }

    /// Marks this thread gone, as it leaves its process: Dead, and no longer
    /// pointing at a kernel thread that is about to be destroyed.
    ///
    /// Called by ProcessDispatcher::detach_thread() with the process's lock
    /// held; takes this thread's own lock after it, the order attach_thread()
    /// uses too.
    auto mark_dead() -> void;

  private:
    enum class Flags {
        Active = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    friend class task::Thread;

    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static auto trampoline(Self *) -> i32;

    ThreadDispatcher(ustl::Arc<ProcessDispatcher> process, char const *name);
    ~ThreadDispatcher() override;

    auto on_exit() -> Status;

    /// This thread raised |context| in ring 3, and this decides what becomes
    /// of it: the thread's own exception channel, then its process's, each
    /// that is bound is told in turn until one answers Handled. Handled
    /// resumes the thread (Resume). Anything else -- no channel, handlers
    /// that pass it on, a kill meanwhile -- ends it (End): the process is
    /// killed with TASK_RETCODE_EXCEPTION_KILL unless it is being killed
    /// already. Called by
    /// task::Thread::Current::on_user_exception(), in the thread's own
    /// context, interrupts on; it may block for as long as the handler takes.
    [[nodiscard]]
    auto on_user_exception(task::ExceptionContext const &context) -> task::ExceptionOutcome;

    /// What became of an exception sent to a handler.
    enum class Delivery {
        /// No handler was told: none is bound, or the message could not be
        /// made or queued.
        Undelivered,
        /// The handler set ExceptionState::Handled and closed the handle.
        Handled,
        /// Every handler told closed the handle without Handled.
        PassedOn,
        /// The thread was killed, before the send or while it waited.
        Killed,
    };

    /// Sends an exception of |type| to the thread's exception channel, then,
    /// if that one passes it on or none is bound, to the process's, waiting
    /// each time for the handler's answer. The send runs under this thread's
    /// lock, so a kill cannot come between "still running" and "sent"; the
    /// wait runs without it, and the state is looked at again after it, so a
    /// kill that came during the wait wins over any answer.
    [[nodiscard]]
    auto deliver_exception(ExceptionType type) -> Delivery;

    /// This thread broke a rule the kernel enforces -- |code|, with |data| as
    /// its detail -- in a system call made from |ip|. Raised as a synthetic
    /// PolicyError exception and handled like any other unhandled one: the
    /// process is killed. Called from system-call context, interrupts on; the
    /// thread exits at the pending-signal check on the way back out.
    auto on_policy_exception(PolicyCode code, u64 data, VirtAddr ip, VirtAddr sp) -> void;

    auto on_resume() -> Status;

    auto on_suspend() -> Status;

    static constexpr auto MAGIC = gktl::magic("ThreadDispatcher");

    /// Kept from creation to start: the kernel thread is named when it is
    /// made, and it is not made until then.
    ktl::Name<MAX_NAME_LEN> m_name;

    Flags m_flags;
    State m_state;
    ParkedReason m_parked_reason;
    /// The kernel thread this object runs as. Guarded by get_lock(); null
    /// once the thread has exited (mark_dead()), because the task::Thread is
    /// destroyed after its exit while this object may live on in handles.
    task::Thread *m_kernel_thread;

    ustl::Arc<ProcessDispatcher> m_process;

    /// See exceptionate().
    Exceptionate m_exceptionate;
    ustl::Option<TaskState> m_user_entry;

    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOption);

  public:
    USTL_DECLARE_LIST(Self, List, ManagedOption);
}; // class ThreadDispatcher

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_THREAD_DISPATCHER_HPP