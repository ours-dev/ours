# prof changes

## Dump version 2: each record says its process, and the header counts unmatched ticks (2026-10-07)

**Files**
- `system/public/ours/sampler.hpp` — `SamplerRecord::pid` (records grow from 152 to 160 bytes, ips move from 24 to 32), `SamplerDumpHeader::num_unmatched` (the header grows from 32 to 40 bytes), `SAMPLER_DUMP_VERSION` 2
- `kernel/lib/prof/include/prof/record_buffer.hpp`, `sink.hpp`, `sampler.hpp`, `src/sampler.cpp`, `src/dump.cpp` — a per-buffer skipped count, `Sampler::note_unmatched()`, `SampleContext::pid` stored in the record, the header summing the skips
- `kernel/lib/thread_sampler/src/thread_sampler.cpp`, `BUILD.gn` — the pid of the interrupted thread's process in both contexts; a tick that matches no target is counted
- `kernel/lib/prof/cli/main.cpp` — prof-fold reads versions 1 and 2, puts `pid N;` before the tid, and reports the unmatched ticks
- `kernel/lib/prof/test/format_test.cpp`, `sampler_test.cpp` — the new layout; a test that the pid is recorded and unmatched ticks are counted only while running
- `examples/shell/shell_tests.cpp` — `check_unmatched_ticks()`: a kernel-only session over user arithmetic must count some
- `examples/profile/profile.cpp` — comments: the header and record line lengths
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `RESULT.md` — the open questions move on

**Symptom**
Two things the dump could not say. Which process a user record came from: it had a tid but no process identity, so its addresses could not be matched to a binary, the first need of symbolising user frames. And how many ticks there were in all: a tick that landed where no target was asked for recorded nothing and was not counted, so a user-only profile could not say what share of the time was spent in user code.

**Cause**
The record's only identity was the kernel thread id, and the header had fields only for what was recorded or dropped. Adding either means a layout change, so both wait for one version bump.

**Change**
The record gains the koid of the interrupted thread's process, `user_thread()->process().koid()` on the running thread, 0 for a kernel thread; the thread keeps its own user thread alive, so the read needs no lock even in the interrupt. That needs the object layer's headers in thread_sampler, as the upstream thread sampler has.

An unmatched tick is counted where it happens, on that CPU's buffer, by the same interrupts-off producer that writes its records, so a relaxed counter suffices; `start()`'s reset clears it, and the header sums it with the records and the drops:

```cpp
if (!frame->from_user()) {
    if (kernel target) { record_sample(...); } else { sampler.note_unmatched(cpu); }
} else if (!(user target)) {
    sampler.note_unmatched(cpu);
} else { request the user stack; }
```

prof-fold reads both versions: a version 1 header is the first 32 bytes of today's, and a version 1 record is converted by moving its ips from 24 to 32.

**Reference**
The upstream sampler records a pid and tid with every sample; its profiler resolves each pid's modules by build ID (`src/performance/cpu_profiler/sampler.cc`, `GetProcessModules`).

**Verification**
Host: prof sampler 24/24 (one new), format 4/4, unwind 10/10, cfi 11/11. In a clean worktree at HEAD with only this change (the shared one holds another session's unfinished `VmoChildOptions` change, which fails `vmo_create_child`): a full build and `boot_check.py`: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required). On dbg, `shell: kernel-only session counted 6 unmatched ticks`; on dev, `profile: 48 kernel samples, 8 user samples, 0 dropped`. prof-fold on that dev dump prints kernel threads as `pid 0` and profile's samples as `pid 2316;tid 2325;...`, with `0 ticks unmatched` since profile asks for both targets; on a hand-built version 1 dump it folds the record as before, without a pid.

**Deliberately not done**
`num_dropped` stays a u32 that can wrap (repair prof item 1).

**How it was found**
The first step of symbolising user frames, chosen by the user, who also agreed to count the unmatched ticks in the same version bump.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is reading the process koid in the timer interrupt safe without a lock?
- Why can the unmatched count be a relaxed counter on each CPU's buffer?
- How does prof-fold read a version 1 record into today's layout?
