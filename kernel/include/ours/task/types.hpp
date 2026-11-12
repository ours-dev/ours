/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_TYPES_HPP
#define OURS_TASK_TYPES_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

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
/// The argument travels beside the routine (`Thread::spawn()`), and the
/// routine's return value is the thread's exit code.
using ThreadStartRoutine = auto (*)(void *arg) -> i32;
using ThreadTrampolineRoutine = auto() -> void;

class Thread;
class IdleThread;

class AutoPreemptDisabler;
class AutoEagerReschedDisabler;

class WaiterState;
class WaitQueueBase;
class WaitQueue;
class OwnedWaitQueue;

/// Whether a thread parked on a wait queue may be woken by a signal.
/// Lives here rather than in sync because it describes how a thread
/// blocks, and the queues that read it are task's.
enum class Interruptible {
    Yes,
    No,
};

/// Requests pending against a thread: stop at the next safe point, die, or
/// have its user stack sampled before it returns to user space.
///
/// These used to be ours::Signals, the public header's object-signal type,
/// which made two unrelated things share one set of bits. An object signal is
/// state an observer waits on through a handle (a channel became readable, a
/// process terminated) and it is ABI; these are orders the kernel gives one of
/// its own threads, checked when the thread parks and on its way back to user
/// space, and no program ever sees them. Sharing the enum put Kill on bit 1,
/// which is where every object's WRITABLE lives.
enum class ThreadSignals : u32 {
    None,
    Suspend = 0_bit,
    Kill = 1_bit,
    /// The sampling timer interrupted this thread in user space: walk its
    /// user stack on the way back there (Thread::Current::
    /// request_stack_sample()). Not a reason to stop waiting, unlike the
    /// two above.
    SampleStack = 2_bit,
};
USTL_ENABLE_ENUM_BITMASK(ThreadSignals);

} // namespace ours::task

#endif // #ifndef OURS_TASK_TYPES_HPP
