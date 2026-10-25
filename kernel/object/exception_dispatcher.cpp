/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/exception_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>

#include <utility>

#include <heap/new.hpp>

namespace ours::object {
auto ExceptionDispatcher::create(ustl::Arc<ThreadDispatcher> thread, ExceptionType const type)
    -> ustl::Arc<Self> {
    return ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(ustl::move(thread), type));
}

ExceptionDispatcher::ExceptionDispatcher(ustl::Arc<ThreadDispatcher> thread,
                                         ExceptionType const type)
    : Base()
    , m_thread(ustl::move(thread))
    , m_type(type)
    , m_state(ExceptionState::TryNext)
    , m_thread_rights(Rights::None)
    , m_process_rights(Rights::None)
    , m_response() {}

// Out of line, where ThreadDispatcher is complete: m_thread's release needs it.
ExceptionDispatcher::~ExceptionDispatcher() = default;

auto ExceptionDispatcher::set_state(ExceptionState const state) -> void {
    m_canary.verify<Self>();
    Guard guard{get_lock()};
    m_state = state;
}

auto ExceptionDispatcher::set_task_rights(Rights const thread_rights, Rights const process_rights)
    -> void {
    m_canary.verify<Self>();
    Guard guard{get_lock()};
    m_thread_rights = thread_rights;
    m_process_rights = process_rights;
}

auto ExceptionDispatcher::make_thread_handle(ustl::Box<Handle> *const out) -> Status {
    m_canary.verify<Self>();
    auto const rights = [this] {
        Guard guard{get_lock()};
        return m_thread_rights;
    }();
    if (rights == Rights::None) {
        return Status::AccessDenied;
    }
    // Arc<Dispatcher> spelled out: an Arc<ThreadDispatcher> converts to both
    // of Handle::make()'s first parameters, KernelHandle and Arc, and this is
    // one more holder of the thread, which is the Arc overload.
    *out = Handle::make(ustl::Arc<Dispatcher>(m_thread), rights);
    if (!*out) {
        return Status::NoResource;
    }
    return Status::Ok;
}

auto ExceptionDispatcher::make_process_handle(ustl::Box<Handle> *const out) -> Status {
    m_canary.verify<Self>();
    auto const rights = [this] {
        Guard guard{get_lock()};
        return m_process_rights;
    }();
    if (rights == Rights::None) {
        return Status::AccessDenied;
    }
    // The thread keeps its process alive, and this keeps the thread.
    *out = Handle::make(ustl::Arc<Dispatcher>(m_thread->process_ref()), rights);
    if (!*out) {
        return Status::NoResource;
    }
    return Status::Ok;
}

auto ExceptionDispatcher::on_zero_handles() -> void {
    m_canary.verify<Self>();
    m_response.signal(Status::Ok);
}

auto ExceptionDispatcher::wait_for_handle_close() -> Response {
    m_canary.verify<Self>();

    auto status = Status::Ok;
    do {
        // A suspend that reaches the thread while it waits wakes it with
        // InternalIrqRetry (Thread::suspend_or_kill()), and it goes back to
        // waiting: both the suspend and the exception have to be over before
        // the thread may run on. Masking Suspend keeps a suspend that is
        // already pending from ending the wait before it starts.
        status = m_response.wait(time::Deadline::infinite(), task::ThreadSignals::Suspend);
    } while (Status::InternalIrqRetry == status);

    if (Status::InternalIrqKilled == status) {
        return Response::Killed;
    }
    // Infinite and interruptible: the wait ends in a signal, a kill or a
    // suspend, and on_zero_handles() signals only Ok.
    ASSERT(Status::Ok == status, "an exception's wait ended with {}", to_string(status));

    // The answer, and the state put back for the next handler. Only this
    // thread sends, so no other handle can be out to signal in between.
    m_response.unsignal();
    Guard guard{get_lock()};
    if (ExceptionState::Handled == std::exchange(m_state, ExceptionState::TryNext)) {
        return Response::Handled;
    }
    return Response::TryNext;
}

auto ExceptionDispatcher::discard_handle_close() -> void {
    m_canary.verify<Self>();
    m_response.unsignal();
    Guard guard{get_lock()};
    m_state = ExceptionState::TryNext;
}
} // namespace ours::object
