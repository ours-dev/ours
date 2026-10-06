# init outstanding repairs

## 1. init parks by spinning after a failed start, which keeps a CPU busy

**Severity**
Low: only after init has already failed to start the system, when the CPU it spins on is the smaller problem.

**Symptom**
`halt()` in `system/service/init/init.cpp` is `for (;;) ort::spin_hint();`. init calls it when it cannot go on: a startup handle is missing, the BootFS cannot be mapped or served, `port_create` fails, the vDSO is not found, `meta/root.cm` cannot be resolved, or its loop returns. From then on one CPU runs init's loop for ever. On the normal path init never gets there: it blocks in `loop.run(ours::INSTANT_INFINITE)`, waiting on its port.

**Cause**
init halts rather than exiting, because the system's other programs are started from it and nothing yet would notice it gone. Spinning was the simplest way to stay.

**Change**
Park by blocking: wait with an infinite deadline on something that is never signalled (its own port, with `port_wait` and `ours::INSTANT_INFINITE`, or `object_wait_one` on a handle of its own), so the thread sleeps in the kernel and the failure line stays the last thing on the console.

**Reference**
None, this project's own design.

**Acceptance**
With a failure forced (for example a missing `meta/root.cm`), init reports it and then no CPU shows init's thread running.
