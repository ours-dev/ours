# sampler changes

## The sampler stress test stops while the buffers have room, now that timers fire at their own deadlines (2026-10-11)

**Files**
- `system/app/shell/shell_tests.cpp` — `check_sampler_stress()`: buffers of 64 records per CPU instead of 16, a tenth of the work per run, and a count of the stops that found room in every buffer, in its summary line.
- `system/public/ours/sampler.hpp` — `SamplerConfig::period`: the period is kept by a kernel on one-shot timers, and at most one sample a tick on the periodic tick.
- `docs/repairs/261009/sampler.md` — deleted: its one item is fixed here.

**Symptom**
With the dev images booting one-shot (ae421697), the stress test's record count jumped from about 60 to 1536 on dbg, which is 24 reads times 4 CPUs times 16 records: every buffer was full at every stop. A full buffer takes no slot write (`RecordBuffer::try_produce()` only advances the cursor), so the stop still met no writer mid-write, the race the test exists for.

**Cause**
The runs were sized for the 10 ms tick, where a run of a millisecond or two took at most one sample per CPU. At a real 10 µs period, 16 records fill in well under a run, and from then on the writers race only on the cursor. The repair item of 2026-10-09 assumed that filling the buffers would mean meeting live writers; it means the opposite.

**Change**
- `RECORDS_PER_CPU` 16 -> 64, and the work per run 200 iterations -> 20, so a run ends before a CPU sampled every 10 µs fills its buffer.
- A run whose header reports no dropped record stopped with room in every buffer; the summary counts them:

```cpp
if (header.num_dropped == 0) {
    ++stopped_with_room;
}
```

Userspace has no clock call, so the runs are bounded by work rather than by time; the count is what shows whether the sizing still holds on a given host.

**Reference**
None, this project's own test.

**Verification**
- clang-format clean on the changed lines.
- In `.worktrees/wip-proc-self` at baseline `ae421697` plus only this change: build ok, `dbg PASS 35 passed, 3 skipped, 0 failed`, `rel PASS 33 passed, 8 skipped, 0 failed, 6/6 required`, host tests 144 passed.

| Image | Before (16 records, 200 iterations) | After (64 records, 20 iterations) |
|---|---|---|
| dbg | 1536 records, every stop full | 498 records, 24 of 24 stops with room |
| rel | 1482 records | 320 records, 24 of 24 stops with room |

- The reset in `Session::start()` removed: `rel FAIL`, `shell: sampler stress: run 5 record 0 is not a whole record of this run`.
- Not measured: how often a stop lands on a write in progress; the count shows only that writers could still write slots when it came.

**Deliberately not done**
- An on-demand stress program with thousands of runs: with writers live at every stop, the boot-time check now exercises the race on every boot.
- profile still drops samples (`profile: 214 kernel samples, 42 user samples (32 in this program), 179 dropped` on rel): its 64 records per CPU fill at a real 1 ms period. Its self-check passes, and its buffer is sized by `docs/repairs/261005/prof.md` item 5's question, not this one.

**How it was found**
Reading the first boot after one-shot became the default: 1536 is exactly 24 x 4 x 16.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does a full buffer mean the stop meets no writer mid-write, even with timers firing every 10 µs?
2. Why are the runs bounded by work and not by time, and what in the summary line tells you the sizing still holds?
3. What made 1536 a suspicious number rather than a good one?

## build/config's frame-pointer comment names sampler::walk_frame_pointers(), not prof:: (2026-10-11)

**Files**
- `build/config/BUILD.gn` — the comment on `frame_pointers_in_leaves`.

**Symptom**
The comment said the sampling profiler walks user stacks with `prof::walk_frame_pointers()`, a namespace that went when prof was folded into `kernel/lib/sampler` (bb3d2b18, 051b17d6).

**Cause**
The rename and the deletion swept the code, but a GN comment is not compiled, so nothing caught it.

**Change**
`prof::walk_frame_pointers()` -> `sampler::walk_frame_pointers()`; `git grep "prof::"` over build, kernel, system and examples now finds nothing.

**Reference**
None, a comment.

**Verification**
`format_gn.py` clean. In `.worktrees/wip-proc-self` at baseline `f3d1465e` plus only this change: dbg PASS (35 passed), rel PASS (33 passed, 6/6 required), host tests 144 passed.

**Deliberately not done**
None.

**How it was found**
Writing the implementation walkthrough of the offline report (2026-10-07).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did a renamed namespace survive in a GN file when every C++ reference to it was caught?
2. What does `frame_pointers_in_leaves` buy the user-stack walk, and what does it cost?
3. Which code in the kernel walks user stacks, and which unwinds kernel stacks?
