# init outstanding repairs

## 1. init halts by spinning for ever, which keeps a CPU busy

**Severity**
Low: nothing is wrong, but on the dev image one CPU spends all its time in init's loop once init has nothing left to do.

**Symptom**
`halt()` in `system/service/init/init.cpp` is `for (;;) ort::spin_hint();`. init runs it once it has started what it was asked to, and never leaves.

**Cause**
init halts rather than exiting, because the system's other programs are started from it and nothing would notice it gone. Spinning was the simplest way to stay; it costs a CPU for the life of the system.

**Change**
Block instead: wait with an infinite deadline on an object that is never signalled (a port, or a handle of init's own), so the thread sleeps in the kernel. Whether init should rather go on serving something it can wait on is the user's call.

**Reference**
None, this project's own design.

**Acceptance**
On dev, after boot, no CPU shows init's thread running; a kernel-target profile shows the CPUs idle.
