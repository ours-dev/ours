/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_TYPES_HPP
#define OURS_TASK_TYPES_HPP 1

#include <ours/types.hpp>
#include <ustl/function/fn.hpp>

namespace ours::task {
/// A thread's entry point: a plain function plus one pointer of context.
///
/// Deliberately not a `std::function`. This kernel has no global
/// `operator new` -- only a panic stub -- so a `std::function` works exactly
/// as long as whatever is stored in it fits the small-buffer optimisation,
/// and silently panics when it does not. `Thread::spawn()` used to bind its
/// arguments into one, and binding a member function pointer (16 bytes on
/// the Itanium ABI) plus a `this` overflows it; that is what killed the DPC
/// workers the moment anything tried to start them. A function pointer and a
/// `void *` cannot outgrow anything.
///
/// The counterpart is Zircon's `thread_start_routine`, `int (*)(void* arg)`,
/// with the argument handed to `Thread::Create()` beside the routine.
using ThreadStartRoutine = auto (*)(void *arg) -> i32;
using ThreadTrampolineRoutine = auto() -> void;

class Thread;
class IdleThread;

class AutoPreemptDisabler;
class AutoEagerReschedDisabler;

class WaiterState;
class WaitQueue;
class OwnedWaitQueue;

/// Whether a thread parked on a wait queue may be woken by a signal.
/// Lives here rather than in sync because it describes how a thread
/// blocks, and the queues that read it are task's.
enum class Interruptible {
    Yes,
    No,
};

} // namespace ours::task

#endif // #ifndef OURS_TASK_TYPES_HPP