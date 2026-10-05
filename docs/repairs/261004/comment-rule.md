# comment-rule outstanding repairs

## 1. fair.hpp compares the Fair run queue's placement with another kernel's run queue

**Severity**
Low: comment text only, no behaviour involved, and Fair is not in the default build.

**Symptom**
`kernel/include/ours/task/sched/fair.hpp` line 20 says the per-CPU container's direct embedding of the Fair run queue "mirrors" another kernel's run queue embedding its fair-class queue. The grep for rule 2 over `kernel/include` still reports it.

**Cause**
The Fair/EEVDF discipline (`fair.hpp`, `fair.cpp`) is the user's own design, and no agent edits it in any way, mechanical changes included (`ours-kernel-design`, "Hands off"). The sweep therefore left this one comment as it was. The paragraph's real reason is already in it -- the set of disciplines is small and fixed at compile time, so naming each discipline's queue directly is simpler than type erasure -- and the comparison adds nothing a reader of this tree needs.

**Change**
For the user to make: drop the comparison sentence's opening ("This mirrors ... directly rather than routing through ...") and start the paragraph from the reason, for example "Embedded directly rather than routed through a generic, type-erased slot: the set of disciplines is small and fixed at compile time ...". The comparison then belongs in that change record's Reference field.

**Reference**
Linux `struct rq` embedding `struct cfs_rq cfs` (`kernel/sched/sched.h`).

**Acceptance**
`grep -n -i -E 'zircon|fuchsia|linux' kernel/include/ours/task/sched/fair.hpp` finds nothing, and the paragraph still gives the compile-time-fixed set of disciplines as the reason for the direct embedding.
