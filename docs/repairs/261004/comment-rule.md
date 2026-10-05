# comment-rule outstanding repairs

## 1. sampler/mod.hpp still explains the sampler and its minimum period by another kernel's names

**Severity**
Low: comment text only, no behaviour involved.

**Symptom**
`kernel/include/ours/sampler/mod.hpp` lines 15, 16 and 23 describe the sampler as another kernel's thread sampler, with that kernel's source path, and justify `MIN_PERIOD` by that kernel's minimum-period constant. The grep for rule 2 over `kernel/include` still reports them.

**Cause**
The kernel/include sweep (`docs/changes/261004/comment-rule.md`) could not touch this file: another session was changing it at the time, and a sweep that edits a file mid-change sweeps that session's hunks into its own commit. The comments therefore still break the rule that code comments say what a thing is and why in this codebase's own terms, and the reader is sent to another kernel's tree to learn what the per-CPU timer is and why the period has a floor.

**Change**
Once the other session's change to `mod.hpp` has landed, rewrite the two comments the way the sweep rewrote the rest of `kernel/include`: the namespace comment says that one timer is armed per CPU while a session runs and none otherwise, without naming the other kernel's class or path; `MIN_PERIOD` keeps its own reason (a sample costs a few microseconds, so a period near that leaves a CPU doing little but sampling) and drops the foreign constant. Move the two names into that change record's Reference field.

**Reference**
Zircon `sampler::ThreadSampler` (`zircon/kernel/lib/thread_sampler`) and `ZX_SAMPLER_MIN_PERIOD`.

**Acceptance**
`grep -rn -i -E 'zircon|fuchsia|linux' kernel/include/ours/sampler/mod.hpp` finds nothing, and a comment-stripped comparison of the file before and after shows no code change.

## 2. fair.hpp compares the Fair run queue's placement with another kernel's run queue

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
