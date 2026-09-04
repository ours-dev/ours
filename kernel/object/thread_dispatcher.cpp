#include <gktl/counter.hpp>
#include <ktl/new.hpp>
#include <ours/arch/entry.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/task/thread.hpp>

GKTL_COUNTER(DISPATCHER_THREAD_SPAWN_COUNT, "dispatcher.thread.spawn");
GKTL_COUNTER(DISPATCHER_THREAD_DESTROY_COUNT, "dispatcher.thread.destroy");

namespace ours::object {
ThreadDispatcher::ThreadDispatcher(ustl::Arc<ProcessDispatcher> process, char const *name)
    : Base(), m_process(process) {}

auto ThreadDispatcher::spawn(ustl::Arc<ProcessDispatcher> process, char const *name,
                             KernelHandle<Self> *out) -> Status {
    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(process, name));
    if (!self) {
        return Status::OutOfMem;
    }

    auto core_thread = task::Thread::spawn(
        name, [](void *arg) -> i32 { return Self::trampoline(static_cast<Self *>(arg)); },
        self.as_ptr_mut(), task::DEFAULT_PRIORITY);
    if (!core_thread) {
        return Status::OutOfMem;
    }

    self->m_kernel_thread = core_thread;

    DISPATCHER_THREAD_SPAWN_COUNT.add(1);
    return Status::Ok;
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

    m_kernel_thread->bind_user_thread(ustl::Arc<Self>::adopt(this));

    if (suspend) {
        m_kernel_thread->suspend();
    } else {
        m_kernel_thread->resume();
    }
    return Status::Ok;
}

auto ThreadDispatcher::on_exit() -> Status {
    verify_canary();
    return Status::Ok;
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