/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/sync/local_lock.hpp>
#include <ours/task/thread.hpp>

namespace ours::sync {
/// A LocalLock guards data that only the local cpu ever touches, so it has
/// no state of its own: what it has to exclude is not another cpu but
/// another *task* on this one. Disabling preemption is the whole lock.
///
/// Interrupts are a separate question and stay a separate policy -- callers
/// that also race an interrupt handler take it as
/// `Guard<sync::LocalLock, IrqSave>`.
auto LocalLock::acquire() -> void {
    task::Thread::Current::preemption_state().disable_preemption();
}

auto LocalLock::release() -> void {
    task::Thread::Current::preemption_state().enable_preemption();
}
} // namespace ours::sync
