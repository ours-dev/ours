/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/exception_dispatcher.hpp>
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
    , m_response() {}

// Out of line, where ThreadDispatcher is complete: m_thread's release needs it.
ExceptionDispatcher::~ExceptionDispatcher() = default;

auto ExceptionDispatcher::set_state(ExceptionState const state) -> void {
    verify_canary();
    Guard guard{get_lock()};
    m_state = state;
}

auto ExceptionDispatcher::on_zero_handles() -> void {
    verify_canary();
    m_response.signal(Status::Ok);
}

auto ExceptionDispatcher::wait_for_handle_close() -> Response {
    verify_canary();

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
    verify_canary();
    m_response.unsignal();
    Guard guard{get_lock()};
    m_state = ExceptionState::TryNext;
}
} // namespace ours::object
