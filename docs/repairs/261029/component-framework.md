# component-framework outstanding repairs

## 1. The component framework design says there is no `task_kill`, which was added on 2026-10-23

**Severity**
Low: a stale statement in a design document. Nothing behaves differently, but a reader who takes the design at its word concludes that the system has no way to end a process from outside, and a guide or a review built on it repeats the error.

**Symptom**
`docs/design/261004/component-framework.md:49` lists "Stop, Kill, escrow, restart and `on_terminate: reboot`" among the non-goals and gives the reason "There is no `task_kill` and no reboot call (D23)". Decision D6 in the same document says "nothing can stop a component" as the cost of giving every component a duplicate of the root job. `system/public/ours/syscalls.gni:820` defines `task_kill`, "Ends the process |task|, asynchronously", which commit `f6e6286d` (2026-10-23) added, and `system/app/shell/shell_tests.cpp` calls it.

**Cause**
The design was written on 2026-10-04, before the call existed; its last change, dated 2026-10-29, left this line as it was. The non-goal itself still holds: nothing in `system/service/init` calls `task_kill`, so the root has no way to stop a component and never starts an exited one again (`system/service/init/realm.cpp:861-866`). Only the reason given for it is out of date.

**Change**
Reword line 49 and the cost in D6 to say that the root has no operation that stops or restarts a component, and drop the claim that the system has no `task_kill`. Whether the root should use `task_kill` to stop a component is a design decision for the user, and this item does not make it.

**Reference**
None, this project's own design.

**Acceptance**
`grep -n "no .task_kill" docs/design/261004/component-framework.md` finds nothing, and the sentence that replaces it is true of `system/public/ours/syscalls.gni` and `system/service/init/realm.cpp`.
