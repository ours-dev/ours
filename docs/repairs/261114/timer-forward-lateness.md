# timer-forward-lateness outstanding repairs

## 1. Timer.ForwardLatenessHoldOffAndRestartCancel fails intermittently under a shuffled run

**Severity**

Low: an emulator-margin flake of the preempt-bounds family — one observed
failure, passing on retry and on baseline.

**Symptom**

`ktest sel=all shuffle=5` on the dbg image failed the test once at
timer_test.cpp:319 (`counted != 0 && held == counted` — the B timer's
interrupt came sooner than its pass's length in some trial, or never in
any). The same run on the baseline image, same seed and therefore same
order, passed; the failing run's seed-5 order passed on retry. The plain
`ktest sel=all` run has not failed.

**Cause**

The test measures timer-interrupt lateness margins under a forward clock
jump — the same emulator-margin family as
`docs/repairs/261107/preempt-bounds.md` (Preempt.SlicesEndAtTheDeadline,
KTEST_SKIP'd under the emulator for the same reason). Its assertion counts
trials of one timer firing in an interrupt of its own while another runs a
slow pass; whether the margins hold depends on the order the tests ran in
before it and the emulator's timer coalescing, not on the code under test.
The suite has no order-independence carve-out for it yet.

**Change**

Decide between the remedies the preempt-bounds item considered:
`KTEST_SKIP` under an emulator for this one test (the preempt-bounds
precedent), or making the assertion order-independent. Measurement first:
collect the failure rate across shuffled seeds on the emulator before
spending either fix.

**Reference**

`docs/repairs/261107/preempt-bounds.md` — the same family, same remedy
shape, same emulator.

**Acceptance**

`ktest sel=all shuffle=<seed>` green across a sweep of seeds on the dbg
image, or the skip documented the way preempt-bounds' is.
