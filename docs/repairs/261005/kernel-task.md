# kernel-task outstanding repairs

## 1. CPU 0's idle thread records a stack it never runs on, and the boot stack's own record is wrong

**Severity**
Low: a 16 KiB kernel stack is allocated and never used, and anything that asks CPU 0's idle thread where its stack is gets the wrong answer. Today that is the sampling profiler, which records CPU 0's idle samples as their pc alone.

**Symptom**
On dev.dbg_1, every sample of CPU 0's idle thread (tid 3) is one frame deep and marked `RecordTruncated`. Logging from the sampler's timer handler showed why: its sp was `0xffffff88400dac90`, inside the boot stack `KERNEL_STACK` (`load_base + 0xd71b8` to `+ 0xdb1b8`), while `Thread::kernel_stack()` said `[0xffffff8804003000, 0xffffff8804007000)`. The sampler unwinds only on the stack the thread records (`prof::StackWindow`, 2026-10-05), so it refuses.

**Cause**
Two records, both wrong:

- `task::init()` builds every CPU's idle thread with `IdleThread::init()`, which goes through `Thread::spawn_etc()` and so allocates a fresh stack for it, CPU 0's included. For the other CPUs that stack is used: they start on it. CPU 0 does not: `init_idle_task_percpu()` makes the idle thread CPU 0's current thread while the boot code keeps running on the boot stack, and that code later becomes the idle loop (`become_idle()` from `start_kernel`), still on the boot stack. The allocated stack is never touched.
- The boot stack's own record is wrong too, though nothing reads it wrongly today. `start.S` stores `%rsp` -- the stack's top, `KERNEL_STACK_END` -- into `KERNEL_STACK_BOTTOM`, and nothing sets `KERNEL_STACK_SIZE`, so the boot thread's `Stack::init_first()` records `[END, END)`. Its `top()` comes out right by accident, and `top()` is all anyone reads today (TSS.sp0).

**Change**
In `start.S`, store the stack's real bottom and size (`lea KERNEL_STACK(%rip)`, and `KERNEL_STACK_END - KERNEL_STACK`, tested 2026-10-05 and then left out, because nothing reads them until the next part lands). Then have CPU 0's idle thread adopt the boot stack instead of spawning one: in `init_idle_task_percpu()` for the running CPU, or with a `Stack` operation that takes over an existing stack and drops the allocated mapping. The idle thread's arch context needs nothing, since it is already running.

**Reference**
Zircon's `ConstructFirstThread()` (`zircon/kernel/kernel/thread.cc`) "sets up the initial thread for each CPU from its running state": the boot CPU's first thread wraps the context already running on the boot stack, and it is that CPU's idle thread, so no second stack is made for it.

**Acceptance**
On dev.dbg_1, CPU 0's idle samples unwind past `ArchIdleThread::idle` (into `IdleThread::run` and `become_idle`), and `Thread::kernel_stack()` of CPU 0's idle thread contains the sp it runs on.
