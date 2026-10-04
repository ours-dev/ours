# kernel-lib-arch changes

## Title: Interrupts are turned on for a scope with arch::AutoInterruptEnabler, not with a manual enable and disable (2026-10-04)

**Files**
- `kernel/lib/arch/include/arch/auto_interrupt_enabler.hpp` — new: `arch::AutoInterruptEnabler`, which turns interrupts on for its scope and puts back the state it found.
- `kernel/lib/arch/BUILD.gn` — lists it.
- `kernel/arch/x86/kernel/faults.cpp` — `deliver_user_exception()` delivers inside one.
- `kernel/arch/x86/mm/fault.cpp` — the page fault is resolved inside one.
- `kernel/lib/syscall/src/table.cpp` — `syscall_dispatch()` runs the handler inside one; the comment on why interrupts go off again moves to it.
- `kernel/kernel/task/thread.cpp` — `process_pending_signals()` suspends inside one.
- `.agents/skills/language/modern-cpp-development/SKILL.md` — the rule.

**Symptom**
None: behaviour is the same. The user asked for it on 2026-10-04, reading `deliver_user_exception()` before step 1 of `.agents/tasks/exception-channels/` changes it.

**Cause**
`arch::local_irq_enable()` followed later by `arch::local_irq_disable()` is a pair of calls that nothing ties together. An early return between them leaves the function with interrupts on, onto a path (the stub's register pops, the pending-signal check before SYSRET or IRETQ) that needs them off. A later edit that moves one half and not the other does the same. Exactly the delivery path step 1 is about to change gains a return path that resumes the thread. The tree already writes the opposite stretch, interrupts off for a scope, as `arch::AutoInterruptDisabler`, and locks as `lockdep::Guard`; interrupts on had no such object.

**Change**

```cpp
// before
arch::local_irq_enable();
Thread::Current::on_user_exception(context);
arch::local_irq_disable();

// after
{
    arch::AutoInterruptEnabler const irqs_on;
    Thread::Current::on_user_exception(context);
}
```

The enabler records `irqs_disabled()` before it turns them on and turns them off again only if they were off, so it is right in a context that already had them on as well. Four paired sites changed: the user exception's delivery, the page fault's resolution (as an immediately invoked lambda, so that the status stays const), the system call handler, and the suspend in `process_pending_signals()`. The one-way calls stay manual, because there is no scope to give back: the Kill branch, which exits; `become_idle()`; the scheduler's first-run handoff; the boot's first enable in `platform_init_apic()`; the user trampoline's disable on its way into ring 3, where `iretq` turns them back on; the IPI stop loop; and `crash panic-locked`'s lock held for good. So does `LocalLock`, which is itself the primitive.

**Reference**
None in Zircon, whose exception handlers call `arch_enable_ints()` and `arch_disable_ints()` by hand, nor in Linux (`local_irq_enable()`/`local_irq_disable()` in `do_user_addr_fault()`). The repo's own `arch::AutoInterruptDisabler` and `lockdep::Guard` are the model.

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (21 passed), rel PASS (19 passed), 0 error lines; every fault probe still ends as before.
- `waitqueue.test`, `sleep.test` and `threadjoin.test` on dbg: ok. None of them reaches the suspend branch of `process_pending_signals()`: kernel threads never get there, and no system call suspends a user thread yet. That branch is verified by the build only.
- clang-tidy: no finding on the changed lines or in the new header.

**Deliberately not done**
- The one-way calls listed above.
- A preemption counterpart: `task::AutoPreemptDisabler` already exists, and no stretch turns preemption on for a scope.

**How it was found**
None: a style request.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What goes wrong if a function with a manual enable and disable gains an early return between them, on the system call path?
- Why does the enabler remember the state it found, instead of always turning interrupts off at the end?
- Why does the Kill branch of `process_pending_signals()` still call `local_irq_enable()` by hand?
