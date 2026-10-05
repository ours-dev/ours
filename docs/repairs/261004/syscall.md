# syscall outstanding repairs

## 1. interrupt_trigger and task_create_exception_channel refuse a bad option before their handle, against design 4.5, with no reason given

**Severity**
Low: both answers are well defined and nothing relies on the order yet, but the code departs from a written design rule without saying why.

**Symptom**
`interrupt_trigger(bad_handle, options != None, t)` answers InvArgs where design 4.5 says BadHandle. `task_create_exception_channel(bad_handle, options != None, out)` does the same.

**Cause**
Design 4.5 (`docs/design/260923/userspace-interface.md`) puts BadHandle > BadType > AccessDenied > InvArgs: resolve the handle, then check the arguments. Its 2026-10-02 revision lists the calls allowed to answer an argument first (channel_write's handle array and options, vma_allocate's unknown option bit), and neither of these two is on that list. `kernel/lib/syscall/src/exception.cpp` checks `options` at the top of `task_create_exception_channel()`, before `get_dispatcher_with_rights()`. `kernel/lib/syscall/src/interrupt.cpp` does the same in `interrupt_trigger()`. The only reason its comment gave was "as there", meaning the reference kernel's order. The 2026-10-04 comment sweep removed that reference under the comment rule, so the comment now states the order with no reason at all.

**Change**
The user decides. Either (a) add both calls to design 4.5's list of exceptions, with a reason in this codebase's terms (for example: an option word the call does not know makes the request unreadable whatever the handle names, the same reasoning vma_allocate already uses), and write that reason into both comments; or (b) resolve the handle first in both calls, so they follow 4.5 like `port_queue` and the vmo calls do. Recommended: (a). It matches vma_allocate, and an options check costs nothing and touches no table. Ruled out: leaving it as it is, because a written rule with silent exceptions is how the next reviewer "fixes" one of them back.

**Reference**
Zircon `sys_interrupt_trigger()` (`zircon/kernel/lib/syscalls/driver.cc`) checks `options` before it looks up the handle. Zircon `sys_task_create_exception_channel()` (`exceptions.cc`) does the same.

**Acceptance**
Design 4.5 and the two comments agree, under either (a) or (b). Under (b), a ring-3 probe that calls each with a bad handle and a bad option gets BadHandle. Both images pass `boot_check.py`.
