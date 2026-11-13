# wait-queue-base: review of the shared queue implementation

**Date**: 2026-11-12

## Scope and method

Reviewed the moved wait and wake operations, the new class hierarchy, and the existing timeout and notify lock scopes. Checked the C++ diff against kernel-design and modern-cpp-development, ran changed-line formatting and clang-tidy, and built, booted, and ran WaitQueue and MutexStress under RR and Fair.

## What is done well

- `kernel/include/ours/task/wait_queue_base.hpp:202` restricts construction of a tagged base to the two concrete queue classes.
- `kernel/kernel/task/wait_queue_base.cpp:43` checks the queue kind before a language-level downcast and handles an absent queue.
- `kernel/tests/wait_queue_test.cpp:76` exercises an owned waiter with a finite deadline and checks the queue and waiter after it returns.

## High severity

### H1. The waiter back-pointer has an existing race between timeout and notification

`kernel/kernel/task/wait_queue_base.cpp:220` and `kernel/kernel/task/wait_queue_base.cpp:276`. A notifier can clear the plain pointer under the queue lock while the timeout handler reads it under the thread PI lock. The subsequent dequeue check under the queue lock prevents a second erase, but the preceding pointer access has no shared lock. This is a source-level finding in the original wake logic carried into the base. The tests did not reproduce a failure. The back-pointer requires a single synchronization protocol shared by its readers and writers.

## Medium severity

None

## Minor and style

Clang-tidy's remaining diagnostics in the touched C++ files are on pre-existing lines outside the change.

## Teaching opportunities

`kernel/kernel/task/wait_queue_base.cpp:243` is the shared point where a dequeued waiter takes the owned queue's PI edge removal before it is made runnable. Zircon places this decision in `WaitQueueBase::UnblockThread()`. Here the caller drops the queue lock before acquiring the PI lock, preserving the existing nesting order.

## Suggested follow-up order

1. Review the waiter back-pointer synchronization as a separate locking change.
