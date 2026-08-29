/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_TYPES_HPP
#define OURS_TASK_TYPES_HPP 1

#include <ours/types.hpp>
#include <ustl/function/fn.hpp>

namespace ours::task {
using ThreadStartRoutine = ustl::function::Fn<auto()->i32>;
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