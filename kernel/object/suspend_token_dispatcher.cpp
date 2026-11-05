/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/suspend_token_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>

#include <utility>

#include <heap/new.hpp>

namespace ours::object {
/// The token is made before the thread is touched, so that running out of
/// memory suspends nothing; and it takes the thread only once the suspend has
/// been granted, so that a token that is refused dies holding nothing, and its
/// on_zero_handles() resumes nothing.
auto SuspendTokenDispatcher::create(ustl::Arc<ThreadDispatcher> thread, KernelHandle<Self> *out)
    -> Status {
    DEBUG_ASSERT(thread);

    if (thread.as_ptr() == ThreadDispatcher::current()) {
        return Status::NotSupported;
    }

    auto *const token = new (mm::ALLOC_KERNEL) Self();
    if (token == nullptr) {
        return Status::OutOfMem;
    }
    KernelHandle<Self> held(token);

    if (auto const status = thread->suspend(); Status::Ok != status) {
        return status;
    }
    {
        Guard guard{token->get_lock()};
        token->m_thread = ustl::move(thread);
    }
    *out = ustl::move(held);
    return Status::Ok;
}

SuspendTokenDispatcher::SuspendTokenDispatcher() : Base(), m_thread() {}

// Out of line, where ThreadDispatcher is complete: m_thread's release needs it.
SuspendTokenDispatcher::~SuspendTokenDispatcher() = default;

/// Resumes after letting go of the token's own lock. ThreadDispatcher::resume()
/// takes the thread's lock, which comes before any other dispatcher's in the
/// lock order (docs/design/260923/userspace-interface.md 4.8), so it must not
/// be taken under this one.
auto SuspendTokenDispatcher::on_zero_handles() -> void {
    m_canary.verify<Self>();

    ustl::Arc<ThreadDispatcher> thread;
    {
        Guard guard{get_lock()};
        thread = ustl::move(m_thread);
    }
    if (thread) {
        thread->resume();
    }
}
} // namespace ours::object
