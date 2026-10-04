# kernel-task changes

## Thread::Current::tid() reads the thread through get() instead of a CPU_LOCAL handle it made up (2026-10-04)

**Files**
- `kernel/include/ours/task/thread.hpp` — `Thread::Current::tid()` returns `Thread::Current::get()->tid()`

**Symptom**
Every CPU took a kernel page fault in its first profiler sample once the sampler ran from a timer: `page fault at 0xffffffffbff1c088 from a context that may not sleep`, at `rip=+0x14b65`, which is `prof::arch::capture_sample_context()`. The fault address differs per CPU (`...bff1c088`, `...bff33088`, `...bff05088`, `...bfeee088`) and always ends in `088`.

**Cause**
`CpuLocal::read(t)` takes a `CPU_LOCAL` variable and reads this CPU's copy of it. On x86 that is one instruction, `mov %gs:&t`: %gs holds this CPU's offset from the linker template, and the displacement is the variable's own address in the template, so the two add up to this CPU's copy (`cpu_local.hpp`, the comment above `read()`).

The old body was `mm::CpuLocal::read(CURRENT_INSTANCE->m_tid)`. The argument is not a CPU_LOCAL variable. To form it, the compiler first loads `CURRENT_INSTANCE` the ordinary way, from the template copy, which nothing ever writes and is a null pointer. `->m_tid` then names address `0 + offsetof(Thread, m_tid)`, which is 0x88, and `read()` treats that as a variable's template address: `mov %gs:0x88`. What it reads is this CPU's offset plus 0x88, an address that holds nothing, hence a fault whose address is "a per-CPU offset, plus 0x88" on every CPU.

The mistake is applying a CPU_LOCAL accessor to a field of the object a CPU_LOCAL pointer points at. Only the pointer is per-CPU; the thread is ordinary memory, reached by reading the pointer first. Every other accessor in `Thread::Current` already does that (`get()->memory_allocation_state()` and the rest).

Nothing noticed because nothing called it outside the profiler, and the profiler only reached it from a sampling interrupt, which until now meant the PMU, and QEMU TCG has no PMU.

**Change**
Before:

```cpp
static auto tid() -> KoId {
    return mm::CpuLocal::read(CURRENT_INSTANCE->m_tid);
}
```

After:

```cpp
static auto tid() -> KoId {
    return Thread::Current::get()->tid();
}
```

**Reference**
Zircon `Thread::Current::Get()` returns the current thread and every per-thread query goes through it (`zircon/kernel/include/kernel/thread.h`); Linux `current->pid` likewise reads the per-CPU `current_task` pointer and then the field. Linux's `this_cpu_read()` has the same rule as `CpuLocal::read()`: its argument must be a per-CPU variable, never an expression through one.

**Verification**
`boot_check.py --out out/sampler`: dbg and rel PASS. `SMP=1 kshell_run.py --image dbg "prof.start timer 100000" "prof.stop" ...`: samples taken on every start, no fault. Before the change the same command faulted on all four CPUs.

**Deliberately not done**
None.

**How it was found**
The fault's `rip` went through `llvm-objdump -d` on `out/sampler/kernel_x86.dbg.1/obj/kernel/products/bin/vmours` around `0x14b65`: `movq %gs:0x88, %rdi`, inside `capture_sample_context()`, right after it read the interrupt frame. A `%gs` displacement as small as 0x88 cannot be a variable's address in the template, which sits at the top of the kernel's address space, so the handle had been computed from a null pointer; `0x88` is `m_tid`'s offset.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `CpuLocal::read()` on x86 not need to subtract anything from the variable's address, and what does %gs hold?
- Why does the faulting address end in the same three digits on every CPU while its upper digits differ?
- Which part of `Thread::Current` is per-CPU, and which part is ordinary memory?
