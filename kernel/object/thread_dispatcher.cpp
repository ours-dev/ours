#include <ours/arch/entry.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/exception_dispatcher.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/task/exception.hpp>
#include <ours/task/thread.hpp>

#include <arch/interrupt.hpp>
#include <gktl/counter.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

GKTL_COUNTER(DISPATCHER_THREAD_SPAWN_COUNT, "dispatcher.thread.spawn");
GKTL_COUNTER(DISPATCHER_THREAD_DESTROY_COUNT, "dispatcher.thread.destroy");

namespace ours::object {
/// Every member below the base used to be left uninitialised. m_flags matters
/// most: mark_active() read-modify-writes it before anything ever assigns it,
/// so the Active bit was being OR-ed into whatever the allocator's memory
/// happened to hold.
ThreadDispatcher::ThreadDispatcher(ustl::Arc<ProcessDispatcher> process, char const *name)
    : Base()
    , m_name(name)
    , m_flags()
    , m_state(State::Initial)
    , m_parked_reason()
    , m_kernel_thread(nullptr)
    , m_process(ustl::move(process))
    , m_starts_process(false) {
    // Counted here, where the object comes into being, so that it pairs with
    // the destroy count in the destructor on every path: counted after
    // task::Thread::spawn() succeeded, as it was, a spawn that failed after
    // this constructor ran destroyed a dispatcher that had never been counted.
    DISPATCHER_THREAD_SPAWN_COUNT.add(1);
}

/// See ~ProcessDispatcher() for why this is counted and logged. The line
/// names the thread by koid and by the process it belonged to.
///
/// The kernel thread is gone by the time this runs, or never was: a thread
/// that ran exited, detached itself (mark_dead()) and dropped its reference
/// to this object as it was destroyed; one that never started has none,
/// because start() is what makes it (create_kernel_thread()).
ThreadDispatcher::~ThreadDispatcher() {
    DISPATCHER_THREAD_DESTROY_COUNT.add(1);
    log::info("object: thread (koid {}) of process '{}' destroyed", koid(), m_process->name());
}

auto ThreadDispatcher::create(ustl::Arc<ProcessDispatcher> process, char const *name,
                              KernelHandle<Self> *out) -> Status {
    if (!process || !out) {
        return Status::InvArgs;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(ustl::move(process), name));
    if (!self) {
        return Status::OutOfMem;
    }

    *out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto ThreadDispatcher::related_koid() const -> KoId {
    return m_process->koid();
}

/// The kernel thread, made at start and not before, and the address space it
/// runs in decided here rather than by whoever started it. That used to be set
/// by hand at the call site (userabi/userboot.cpp did `thread->set_aspace(...)`),
/// which put a kernel/kernel detail in the hands of a caller that is supposed
/// to see only the object layer -- and which only worked because the caller
/// happened to know it must come before the first resume().
///
/// Here it is unconditionally before resume(): activate() is what resumes, and
/// it runs after this, under the same process lock. That ordering matters --
/// the scheduler reloads CR3 from Thread::aspace() on the way in, so an aspace
/// set on a thread that is already running would leave the record and the
/// hardware disagreeing.
///
/// The kernel thread is handed a raw pointer to this object, which therefore
/// has to outlive it: activate() gives the kernel thread its own reference
/// (bind_user_thread()) before it can run, and until then the caller of
/// start() holds one.
auto ThreadDispatcher::create_kernel_thread() -> Status {
    verify_canary();

    Guard guard{get_lock()};
    if (m_state != State::Initial || m_kernel_thread != nullptr) {
        return Status::BadState;
    }

    auto *const core_thread = task::Thread::spawn(
        m_name.data(), [](void *arg) -> i32 { return Self::trampoline(static_cast<Self *>(arg)); },
        this, task::DEFAULT_PRIORITY);
    if (!core_thread) {
        return Status::OutOfMem;
    }
    core_thread->set_aspace(m_process->aspace());
    m_kernel_thread = core_thread;

    log::debug("object: thread (koid {}) of process '{}' has its kernel thread", koid(),
               m_process->name());
    return Status::Ok;
}

auto ThreadDispatcher::current() -> Self * {
    auto *const thread = task::Thread::Current::get();
    if (thread == nullptr) {
        return nullptr;
    }
    return thread->user_thread_mut();
}

/// The calling user thread leaves: its kernel thread exits, and on the way
/// out on_exit() takes this object out of its process. The retcode given to
/// the kernel thread means nothing to anyone -- a process's code is the one
/// ProcessDispatcher recorded -- so it is 0.
auto ThreadDispatcher::exit_current() -> void {
    DEBUG_ASSERT(current() != nullptr, "only a user thread exits through its dispatcher");
    task::Thread::Current::exit(0);
}

auto ThreadDispatcher::resume() -> void {
    verify_canary();
}

auto ThreadDispatcher::suspend() -> void {
    verify_canary();
}

auto ThreadDispatcher::trampoline(Self *self) -> i32 {
    DEBUG_ASSERT(self->m_user_entry);
    auto &task = *self->m_user_entry;

    arch::IrqFrame frame{};
    arch_build_uspace_irqframe(&frame, task.entry, task.stack, task.arg1, task.arg2);

    // Before the process's first instruction, so a debugger told of it here
    // can look at the process, and bind to it, while nothing in it has run.
    // Interrupts are still on: the debugger may take as long as it likes.
    if (self->m_starts_process) {
        self->report_process_start();
    }

    // The scheduler's thread trampoline enables interrupts before it calls
    // here, and they have to go off again for the last stretch into ring 3:
    // see arch_enter_uspace() for the swapgs/iretq window. Nothing re-enables
    // them on this side; the IF bit in the frame does that as iretq lands in
    // user mode.
    arch::local_irq_disable();

    // The first way back to ring 3 checks pending signals like every other
    // one does -- the exception tail, and the path to SYSRET. A thread killed
    // after it was started but before it first ran would otherwise begin its
    // program at the entry point, and die only at its first interrupt or
    // system call, having run user code in a process that is already dying.
    // A Kill exits the thread here.
    task::Thread::Current::process_pending_signals();
    arch_enter_uspace(&frame);
}

auto ThreadDispatcher::start(TaskState const &state, bool const first_thread) -> Status {
    verify_canary();

    // Every user thread's first instruction is named here, so this is where
    // an entry point outside the user address space is refused. Let through,
    // it would reach the iretq in x86_enter_uspace, which on Intel faults in
    // ring 0 on a non-canonical address with the user's %gs already loaded
    // (arch_is_valid_user_pc() has the details); arch_enter_uspace() would
    // stop it only by panicking. The entry is whatever the starter chose: a
    // program's, through process_start, or, for a program the kernel starts
    // itself (userabi::launch()), the ELF's e_entry moved by its load
    // address, which elfload::check_program() has already checked lies in an
    // executable segment.
    if (!arch_is_valid_user_pc(state.entry)) {
        return Status::InvArgs;
    }

    // Nothing of this thread is written here: the process checks that it can
    // take the thread and makes the kernel thread, and activate() saves the
    // entry last, under this thread's lock, where nothing can refuse the
    // start any more.
    return m_process->attach_thread(*this, first_thread, state);
}

auto ThreadDispatcher::activate(TaskState const &entry, bool const starts_process,
                                bool const suspend) -> Status {
    Guard guard{get_lock()};

    // create_kernel_thread() found this Initial under this lock, and the
    // process's lock, which the caller has held since, is what any other start
    // takes first; a kill leaves an Initial thread as it is.
    DEBUG_ASSERT(m_state == State::Initial && m_kernel_thread != nullptr,
                 "a thread activated twice, or without its kernel thread");
    m_user_entry = entry;
    m_starts_process = starts_process;

    // `adopt` reads as "take over a reference that already exists" -- and is
    // not: ustl::Rc's adopting constructor calls retain(), so this is a *new*
    // owning reference. That is what is wanted here (the caller's
    // KernelHandle keeps its own), so this line is correct as written; the
    // note is here because the name says the opposite of what it does and
    // the read-through is otherwise a double-free.
    m_kernel_thread->bind_user_thread(ustl::Arc<Self>::adopt(this));
    m_state = State::Running;
    // Under the lock this function holds: the signal follows the state, and
    // update_state() would take the lock a second time.
    update_state_locked(Signals::None, Signals::ThreadRunning);

    // Both answer only Ok: suspend() is suspend_or_kill(), which sets a
    // signal and returns, and resume() does nothing to a Dead thread.
    if (suspend) {
        auto _ = m_kernel_thread->suspend();
    } else {
        auto _ = m_kernel_thread->resume();
    }
    return Status::Ok;
}

/// The kernel thread is exiting (task::Thread::Current::exit()), in its own
/// context and able to block. This object leaves its process now, while the
/// thread still exists, rather than when the object is finally destroyed:
/// handles may keep it alive long after, and until then the process's
/// intrusive list would still link it -- a kill walking that list would find a
/// thread with nothing behind it, and destroying the object while linked
/// trips the list hook's own check.
auto ThreadDispatcher::on_exit() -> Status {
    verify_canary();
    // The thread's exception channel first: its reader sees PeerClosed, a
    // later bind is refused, and the cycle through an unread exception
    // message back to this thread is broken. No lock is held here (hard rule
    // 1 of userspace-interface.md 4.8: dropping the kernel end runs its
    // on_zero_handles()).
    m_exceptionate.shutdown();
    return m_process->detach_thread(*this);
}

/// The handlers are the thread's exception channel and then its process's.
/// A debugger's channel, ahead of both, and the job chain's, after them, come
/// later (.agents/tasks/exception-channels/).
///
/// What dies when nothing handles it is the process, not the thread: a thread
/// that faulted has left its process in a state nothing else in it can be
/// trusted to recover from. The fault is reported, with the registers, only
/// when no handler saw it: a handler that passed it on knew what it was, and
/// a warning for every fault a supervisor watched would bury the ones nothing
/// watched. Logged as a warning rather than an error: a program dying of its
/// own bug is not a kernel fault.
auto ThreadDispatcher::on_user_exception(task::ExceptionContext const &context)
    -> task::ExceptionOutcome {
    verify_canary();

    auto const delivery = deliver_exception(context.type);
    switch (delivery) {
        case Delivery::Handled:
            return task::ExceptionOutcome::Resume;
        case Delivery::Killed:
            // Whoever killed it gave the thread the Kill signal already.
            return task::ExceptionOutcome::End;
        case Delivery::Undelivered:
            log::warn("terminating process '{}' (koid {}) thread koid {}: type={} vector={} "
                      "error_code={:#x} rip={:#x} sp={:#x} cr2={:#x} synth_code={} "
                      "synth_data={:#x}",
                      m_process->name(), m_process->koid(), koid(), task::to_string(context.type),
                      context.vector_name, context.error_code, context.ip, context.sp,
                      context.fault_address, context.synth_code, context.synth_data);
            task::arch_dump_exception_context(context);
            break;
        case Delivery::PassedOn:
            log::debug("object: process '{}' (koid {}) killed: its handler passed on {}",
                       m_process->name(), m_process->koid(), task::to_string(context.type));
            break;
    }

    // Not under this thread's lock: the kill takes the process's lock and
    // then each thread's, this one's among them.
    m_process->kill(TASK_RETCODE_EXCEPTION_KILL);
    return task::ExceptionOutcome::End;
}

auto ThreadDispatcher::deliver_exception(ExceptionType const type) -> Delivery {
    // The exception holds the thread, so it needs a reference of its own.
    // This thread is running the call, so its object is alive and owned:
    // try_adopt() only refuses an object whose last owner is gone.
    auto self = ustl::Arc<Self>::try_adopt(this);
    DEBUG_ASSERT(self, "a thread raising an exception has no owner");
    // Not const: ustl::Rc's constness is deep, and the wait below changes it.
    auto exception = ExceptionDispatcher::create(ustl::move(self), type);
    if (!exception) {
        // Out of memory: no handler can be told, and the process is killed.
        return Delivery::Undelivered;
    }

    // The thread's own slot, then its process's. The one exception goes to
    // each in turn, reset in between by the wait (or by a failed send's
    // discard).
    Exceptionate *const slots[] = {&m_exceptionate, &m_process->exceptionate()};
    auto told = false;
    for (auto *const slot: slots) {
        switch (offer_exception(*slot, exception)) {
            case Offer::Unsent:
                continue;
            case Offer::PassedOn:
                told = true;
                continue;
            case Offer::Handled:
                return Delivery::Handled;
            case Offer::Killed:
                return Delivery::Killed;
        }
    }
    if (told) {
        return Delivery::PassedOn;
    }
    return Delivery::Undelivered;
}

auto ThreadDispatcher::offer_exception(Exceptionate &slot,
                                       ustl::Arc<ExceptionDispatcher> &exception) -> Offer {
    {
        // Under this thread's lock: a kill takes the same lock, so it lands
        // either before the check below -- and nothing is sent for a thread
        // that is already dying -- or after the message is in the channel,
        // where the wait below sees it. The order is ThreadDispatcher, then
        // the exceptionate's lock, then the channel's PeerHolder (the user's
        // call, 2026-10-04; see userspace-interface.md 4.8).
        Guard guard{get_lock()};
        if (m_state != State::Running) {
            return Offer::Killed;
        }
        if (slot.send(exception) != Status::Ok) {
            return Offer::Unsent;
        }
    }

    // Without the lock: the handler may take as long as it likes, and a kill
    // has to be able to take the lock meanwhile to reach this thread.
    auto const response = exception->wait_for_handle_close();

    // A kill wins over every answer. It can land after the handler's close
    // has signalled the exception and before this thread runs again; the
    // wake is then the close's (Thread::suspend_or_kill() leaves a waiter
    // that another wake has claimed), and the wait answers as if no kill had
    // come. The state says otherwise.
    Guard guard{get_lock()};
    if (m_state != State::Running || response == ExceptionDispatcher::Response::Killed) {
        return Offer::Killed;
    }
    if (response == ExceptionDispatcher::Response::Handled) {
        return Offer::Handled;
    }
    return Offer::PassedOn;
}

/// The exception is made at the first debugger channel, not before: most
/// processes start in a job nobody debugs, and they pay for one look at the
/// job's list and nothing else. Out of memory for it, the debuggers are not
/// told and the process starts all the same, as it would with none bound.
auto ThreadDispatcher::report_process_start() -> void {
    struct Report {
        ThreadDispatcher *thread;
        ustl::Arc<ExceptionDispatcher> exception;
    };
    Report report{.thread = this, .exception = {}};
    m_process->job().for_each_debugger(&report, [](void *const context, Exceptionate &debugger) {
        auto &report = *static_cast<Report *>(context);
        if (!report.exception) {
            // Owned, as in deliver_exception(): this thread is running it.
            auto self = ustl::Arc<Self>::try_adopt(report.thread);
            DEBUG_ASSERT(self, "a starting thread has no owner");
            report.exception =
                ExceptionDispatcher::create(ustl::move(self), ExceptionType::ProcessStarting);
            if (!report.exception) {
                return false;
            }
        }
        return report.thread->offer_exception(debugger, report.exception) != Offer::Killed;
    });
}

auto ThreadDispatcher::on_policy_exception(task::PolicyCode const code, u64 const data,
                                           VirtAddr const ip, VirtAddr const sp) -> void {
    // The system-call layer answers BadSyscall whatever happens here
    // (not_from_vdso()); a thread that is to end ends at the pending-signal
    // check on the way out of the call.
    auto _ = on_user_exception(task::ExceptionContext{
        .type = task::ExceptionType::PolicyError,
        .vector_name = task::to_string(code),
        .error_code = 0,
        .ip = ip,
        .sp = sp,
        .fault_address = 0,
        .synth_code = static_cast<u32>(code),
        .synth_data = data,
        // Raised from inside a system call: there is no exception frame, and
        // the syscall frame's registers are the program's arguments rather
        // than the state at a fault.
        .frame = nullptr,
    });
}

auto ThreadDispatcher::on_bad_syscall(u64 const number, VirtAddr const ip, VirtAddr const sp)
    -> void {
    on_policy_exception(task::PolicyCode::BadSyscall, number, ip, sp);
}

auto ThreadDispatcher::kill() -> void {
    verify_canary();

    Guard guard{get_lock()};
    switch (m_state) {
        case State::Initial:
            // Never started: no kernel thread, and not on the process's list
            // for a kill to reach it through.
            break;
        case State::Running: {
            DEBUG_ASSERT(m_kernel_thread, "a started thread with no kernel thread");
            auto _ = m_kernel_thread->kill();
            m_state = State::Dying;
            break;
        }
        case State::Dying:
        case State::Dead:
            break;
    }
}

auto ThreadDispatcher::mark_dead() -> void {
    verify_canary();

    Guard guard{get_lock()};
    m_state = State::Dead;
    m_kernel_thread = nullptr;
}

auto ThreadDispatcher::on_resume() -> Status {
    verify_canary();

    Guard guard{get_lock()};
    return Status::Ok;
}

auto ThreadDispatcher::on_suspend() -> Status {
    verify_canary();

    Guard guard{get_lock()};
    return Status::Ok;
}

} // namespace ours::object