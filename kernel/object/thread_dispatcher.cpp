#include <ours/arch/entry.hpp>
#include <ours/lockdep.hpp>
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
    , m_flags()
    , m_state(State::Running)
    , m_parked_reason()
    , m_kernel_thread(nullptr)
    , m_process(ustl::move(process)) {
    // Counted here, where the object comes into being, so that it pairs with
    // the destroy count in the destructor on every path: counted after
    // task::Thread::spawn() succeeded, as it was, a spawn that failed after
    // this constructor ran destroyed a dispatcher that had never been counted.
    // Zircon counts dispatcher.thread.create in its constructor too.
    DISPATCHER_THREAD_SPAWN_COUNT.add(1);
}

/// See ~ProcessDispatcher() for why this is counted and logged. The thread
/// keeps no name of its own -- its kernel thread has one -- so the line names
/// it by koid and by the process it belonged to.
///
/// For a thread that ran, the kernel thread is gone by the time this runs: it
/// exited, detached itself (mark_dead()) and dropped its reference to this
/// object as it was destroyed. For one that was spawned and never started,
/// it is not: it is still on the global thread list in the Initial state,
/// and nothing frees it -- there is no equivalent yet of Zircon's
/// Thread::Forget() (docs/repairs/260924/object.md). No path reaches that
/// today: userabi::launch() starts every thread it spawns.
ThreadDispatcher::~ThreadDispatcher() {
    DISPATCHER_THREAD_DESTROY_COUNT.add(1);
    log::info("object: thread (koid {}) of process '{}' destroyed", koid(), m_process->name());
}

auto ThreadDispatcher::spawn(ustl::Arc<ProcessDispatcher> process, char const *name,
                             KernelHandle<Self> *out) -> Status {
    if (!process || !out) {
        return Status::InvArgs;
    }

    // Held before the kernel thread exists, because the kernel thread is
    // handed a raw pointer to it: whatever happens below, this object has to
    // outlive the thread that will run inside it.
    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(process, name));
    if (!self) {
        return Status::OutOfMem;
    }

    // The address space the kernel thread will run in, decided here rather
    // than by whoever called spawn(). It used to be set by hand at the call
    // site (userabi/userboot.cpp did `thread->set_aspace(...)`), which put a
    // kernel/kernel detail in the hands of a caller that is supposed to see
    // only the object layer -- and which only worked because the caller
    // happened to know it must come before the first resume().
    //
    // Here it is unconditionally before resume(): activate() is what resumes,
    // and that cannot be reached until start() is called on the handle this
    // function has not returned yet. That ordering matters -- the scheduler
    // reloads CR3 from Thread::aspace() on the way in, so an aspace set on a
    // thread that is already running would leave the record and the hardware
    // disagreeing.
    auto core_thread = task::Thread::spawn(
        name, [](void *arg) -> i32 { return Self::trampoline(static_cast<Self *>(arg)); },
        self.as_ptr_mut(), task::DEFAULT_PRIORITY);
    if (!core_thread) {
        return Status::OutOfMem;
    }
    core_thread->set_aspace(self->m_process->aspace());

    self->m_kernel_thread = core_thread;

    // Without this the object died here. `self` is the only reference, `out`
    // was never written, and the caller therefore got back Status::Ok and
    // nothing else -- while task::Thread went on holding the raw pointer to
    // the freed dispatcher that its trampoline runs on. Nothing caught it
    // because no caller existed.
    *out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto ThreadDispatcher::current() -> Self * {
    auto *const thread = task::Thread::Current::get();
    if (thread == nullptr) {
        return nullptr;
    }
    return thread->user_thread_mut();
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

    // The scheduler's thread trampoline enables interrupts before it calls
    // here, and they have to go off again for the last stretch into ring 3:
    // see arch_enter_uspace() for the swapgs/iretq window. Zircon's
    // ThreadDispatcher::StartRoutine makes the same arch_disable_ints() call
    // right before its arch_enter_uspace(). Nothing re-enables them on this
    // side; the IF bit in the frame does that as iretq lands in user mode.
    arch::local_irq_disable();

    // The first way back to ring 3 checks pending signals like every other
    // one does -- the exception tail, and the path to SYSRET. A thread killed
    // after it was started but before it first ran would otherwise begin its
    // program at the entry point, and die only at its first interrupt or
    // system call, having run user code in a process that is already dying.
    // A Kill exits the thread here. Zircon's StartRoutine makes the same
    // arch_iframe_process_pending_signals() call before arch_enter_uspace().
    task::Thread::Current::process_pending_signals();
    arch_enter_uspace(&frame);
}

auto ThreadDispatcher::start(TaskState const &state) -> Status {
    verify_canary();

    m_user_entry = state;
    auto status = m_process->attach_thread(*this);
    if (Status::Ok != status) {
        m_user_entry = ustl::none();
        return status;
    }

    return Status::Ok;
}

auto ThreadDispatcher::activate(bool suspend) -> Status {
    Guard guard{get_lock()};

    // `adopt` reads as fbl::AdoptRef -- take over a reference that already
    // exists -- and is not: ustl::Rc's adopting constructor calls retain(), so
    // this is a *new* owning reference, the equivalent of Zircon's
    // fbl::RefPtr<ThreadDispatcher>(this). That is what is wanted here (the
    // caller's KernelHandle keeps its own), so this line is correct as
    // written; the note is here because the name says the opposite of what it
    // does and the read-through is otherwise a double-free.
    m_kernel_thread->bind_user_thread(ustl::Arc<Self>::adopt(this));

    if (suspend) {
        m_kernel_thread->suspend();
    } else {
        m_kernel_thread->resume();
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
///
/// Zircon counterpart: ThreadDispatcher::ExitingCurrent(), which calls
/// process_->RemoveThread(this).
auto ThreadDispatcher::on_exit() -> Status {
    verify_canary();
    return m_process->detach_thread(*this);
}

/// The only answer there is yet: there are no exception channels (Zircon's
/// zx_task_create_exception_channel) for a debugger or a supervisor to take
/// the exception first, so it is reported and the process dies. What dies is
/// the process, not the thread, as in Zircon: a thread that faulted has left
/// its process in a state nothing else in it can be trusted to recover from.
///
/// Logged as a warning rather than an error: a program dying of its own bug is
/// not a kernel fault. The report goes out before the kill so that it is
/// there even if the kill path itself goes wrong.
///
/// Zircon counterpart: the end of dispatch_user_exception(), which prints the
/// crash report and kills with ZX_TASK_RETCODE_EXCEPTION_KILL when no handler
/// took the exception.
auto ThreadDispatcher::on_user_exception(task::ExceptionContext const &context) -> void {
    verify_canary();

    log::warn("terminating process '{}' (koid {}) thread koid {}: type={} vector={} "
              "error_code={:#x} rip={:#x} sp={:#x} cr2={:#x} synth_code={} synth_data={:#x}",
              m_process->name(), m_process->koid(), koid(), task::to_string(context.type),
              context.vector_name, context.error_code, context.ip, context.sp,
              context.fault_address, context.synth_code, context.synth_data);
    task::arch_dump_exception_context(context);

    m_process->kill(TASK_RETCODE_EXCEPTION_KILL);
}

auto ThreadDispatcher::on_policy_exception(task::PolicyCode const code, u64 const data,
                                           VirtAddr const ip, VirtAddr const sp) -> void {
    on_user_exception(task::ExceptionContext{
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
        case State::Running:
            // Active is set when the thread starts (attach_thread()); one
            // that never started has nothing running to kill. Zircon leaves
            // INITIAL and INITIALIZED threads alone for the same reason.
            if (!(m_flags & Flags::Active)) {
                break;
            }
            DEBUG_ASSERT(m_kernel_thread, "a started thread with no kernel thread");
            static_cast<void>(m_kernel_thread->kill());
            m_state = State::Dying;
            break;
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