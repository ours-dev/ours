# mm outstanding repairs

## 1. A dead thread's kernel stack is never given back

**Severity**
Medium: a leak on a path the kernel runs today, at every thread exit.

**Symptom**
Every thread that is destroyed leaves its 16 KiB kernel stack behind: the pinned frames, the `VmObjectPaged` and `VmMapping` behind them, and the range of kernel virtual addresses. Not measured; it grows with the number of threads ever destroyed.

**Cause**
`Stack::init()` (`kernel/mm/stack.cpp:22-50`) makes the mapping with `create_mapping()` in the kernel's root VMA, which links it into the root's `VmaSet`; `VmaSet::insert()` takes a reference of its own (`vm_area_or_mapping.cpp:93`), released only by `VmaSet::erase()`, that is by destroying the mapping. `~Stack()` is defaulted (`kernel/mm/include/ours/mm/stack.hpp:20`): it drops the `Stack`'s own `Arc` and nothing else. `Thread::destroy()` (`kernel/kernel/task/thread.cpp:432-456`, run from a DPC) runs `~Thread()`, which runs `~Stack()`, and nothing ever destroys the mapping. The comment above `~VmCowPages()` (`kernel/mm/vm_cow_pages.cpp:38-46`) already notes that kernel stacks are never destroyed.

Fixing it gives the stack's range back to a lowest-address-first allocator while other CPUs routinely hold translations of a thread's stack (it migrates; `sync_exec()` requests live on it), and the next thread gets the same range. That is safe only because a kernel unmap is shot down on every online CPU, since 2026-10-02 (`ArchVmAspace::invalidate()`).

**Change**
Destroy the mapping when the stack goes (`~Stack()` or `Thread::destroy()` calls `destroy()` on it); the unmap already reaches every CPU. Zircon also puts guard pages around each kernel stack; this one has none, and that is a separate item if wanted.

**Reference**
Zircon `KernelStack::Teardown()` (`zircon/kernel/vm/kstack.cc:248`), which destroys the stack's mappings and VMARs when the thread is freed.

**Acceptance**
A command that spawns and joins many threads shows the free-frame count back where it started; `vmosnapshot.test` and the shell's checks still pass on both images.
