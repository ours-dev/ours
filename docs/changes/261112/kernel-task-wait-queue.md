# kernel-task-wait-queue changes

## Share the wait queue mechanism between sibling queues (2026-11-12)

**Files**
- `kernel/include/ours/task/wait_queue_base.hpp` and `kernel/kernel/task/wait_queue_base.cpp` hold the shared waiter tree, lock, wait and targeted wake operations.
- `kernel/include/ours/task/wait_queue.hpp` and `kernel/kernel/task/wait_queue.cpp` hold the ordinary queue and its notify operations.
- `kernel/include/ours/task/owned_wait_queue.hpp` and `kernel/kernel/task/owned_wait_queue.cpp` hold the owned queue and its ownership and priority inheritance operations.
- `kernel/include/ours/task/types.hpp`, `kernel/include/ours/task/thread.hpp` and `kernel/kernel/task/BUILD.gn` wire in the base.
- `kernel/tests/wait_queue_test.cpp` exercises an owned waiter removed by its deadline.

**Symptom**
`OwnedWaitQueue` inherited the ordinary queue even though callers use the two concrete types for different wake operations. A thread recorded a `WaitQueue *` even when it was blocked on the owned queue.

**Cause**
The ordinary queue owned the waiter tree, lock, and parking logic. The owned queue reused those mechanisms through protected inheritance and relied on the ordinary queue's type tag and a reinterpret cast when a shared wake needed its owner-specific behavior.

**Change**

```cpp
class OwnedWaitQueue : protected WaitQueue { /* ... */ };
```

```cpp
class WaitQueue final : public WaitQueueBase { /* ... */ };
class OwnedWaitQueue final : public WaitQueueBase { /* ... */ };
```

`WaiterState::wait_queue()` now returns `WaitQueueBase *`. Either queue can be the target of a timeout or suspend without exposing its concrete notification interface. The private base constructor limits construction to the two concrete queues. The base checks the immutable queue kind before a `static_cast` to the owned queue and accepts a null back-pointer when the thread is outside a queue.

**Reference**
Zircon `WaitQueueBase::UnblockThread()` and `WaitQueueBase::TimeoutHandler()` in `zircon/kernel/kernel/wait.cc`, with `WaitQueue` and `OwnedWaitQueue` sharing the base defined in `zircon/kernel/include/kernel/wait.h`.

**Verification**
Changed-line clang-format and GN formatting passed. Clang-tidy checked all 13 changed C++ files with no diagnostics on the changed lines. RR and Fair each built and booted dbg and rel, with 46 dbg shell checks, 44 rel shell checks, and 183 host tests passing per configuration. Both configurations passed two WaitQueue tests and one MutexStress test. The mutex test completed 20,000 rounds on each of three threads in both configurations.

**Deliberately not done**
The PI propagation protocol and lock ordering follow the existing implementation. The inheritance change does not require a new locking protocol.

**How it was found**
Traced the waiter back-pointer from timer expiry and thread suspend, and traced the owned queue's wake to the type conversion in the ordinary queue header.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does a thread's waiter state refer to the common queue type?
- Which wake operations require the concrete queue interface?
- Which checks make the conversion from the base to an owned queue valid?
