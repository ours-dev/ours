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

## A sampler handle's rights gate its calls: Write to start and stop, Read to read (2026-10-11)

**Files**
- `kernel/object/include/ours/object/sampler_dispatcher.hpp` — `RIGHTS_DEFAULT_SAMPLER` gains Read and Write; its comment says which call asks for which.
- `kernel/lib/syscall/src/sampler.cpp` — `sampler_start` and `sampler_stop` look the handle up with `Rights::Write`, `sampler_read` with `Rights::Read`, where all three asked for `Rights::None`.
- `system/public/ours/syscalls.gni` — the four calls' documentation: the handle's rights, and AccessDenied without the one asked for.
- `system/app/shell/shell_tests.cpp` — `check_sampler()`: a copy with Transfer, Duplicate and Inspect only is refused by all three calls.
- `docs/repairs/261007/sampler.md` — item 1 removed.

**Symptom**
Any handle to a session, however it was duplicated, could start it, stop it and read every record: a copy with every right removed still stopped the session. There was no way to hand an analysis tool a handle that reads but cannot control, or a controller that cannot read.

**Cause**
The SAMPLING resource checked at `sampler_create` was the only gate, and the handle's own rights were never asked. `.agents/tasks/prof-sampler-object/DESIGN.md` lists "no right gates it" among the reasons the profiler became a kernel object, and the object brought the resource check but not the rights. The resource answers "may this process make a session at all"; the rights answer "what may this holder do with this one", and nothing was answering the second.

**Change**
Before, in each of the three calls:

```cpp
handle_table().get_dispatcher(handle, Rights::None, &sampler);
```

After:

```cpp
handle_table().get_dispatcher(handle, Rights::Write, &sampler); // sampler_start, sampler_stop
handle_table().get_dispatcher(handle, Rights::Read, &sampler);  // sampler_read
```

and the default rights become `Transfer | Duplicate | Inspect | Read | Write`, so the creator's handle can still do everything. The lookup checks rights before the call looks at the session, so a handle without the right is refused whatever state the session is in. Ruled out: checking the SAMPLING resource on every call, which would make a session handle pointless to pass on.

**Reference**
Zircon's rights convention: reading an object's data asks for ZX_RIGHT_READ (`zx_vmo_read()`), changing its state ZX_RIGHT_WRITE (`zx_vmo_write()`); ours's own `vmo_read`, `vmo_write` and `interrupt_bind` follow it, and the shell checks each.

**Verification**
- clang-format clean on the changed lines; clang-tidy (`out/bringup.x86`) reports nothing on them; `format_gn.py` clean.
- In `.worktrees/wip-proc-self` at baseline `55879de8` plus only this change: build ok, `dbg PASS 35 passed, 3 skipped, 0 failed`, `rel PASS 33 passed, 8 skipped, 0 failed, 6/6 required`, host tests 144 passed; both images log `sampler_start(without Write) = AccessDenied`, `sampler_stop(without Write) = AccessDenied`, `sampler_read(without Read) = AccessDenied`; profile still records on rel (`213 kernel samples, 43 user samples`).
- With `sampler.cpp` back at `Rights::None`: `rel FAIL`, all three `= Ok (expected AccessDenied)`.

**Deliberately not done**
None.

**How it was found**
Reviewing the sampler while writing the offline report's walkthrough (2026-10-07): `sampler.cpp` asked for `Rights::None` in all three lookups.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What does the SAMPLING resource decide, and what do the handle's rights decide, and why does neither replace the other?
2. Why is a handle without Write refused even when the session is in a state where the call would otherwise answer BadState?
3. Which handle would you give a tool that should only read a finished profile, and how would you make it?

## A sampling session gives its claim back when its memory goes, not when its last handle closes (2026-10-11)

**Files**
- `kernel/lib/sampler/include/sampler/session.hpp` — `Session::Claim`, a private member type whose destructor gives `CLAIMED` back, declared as the first member; the comments on the class, `create()`, `release()` and `CLAIMED` say the claim lasts the session's life.
- `kernel/lib/sampler/src/session.cpp` — `Claim::~Claim()` takes the control lock and clears `CLAIMED`; `release()` no longer clears it; `create()` clears it by hand only when no session object could be built.
- `kernel/commands/sampler_claim_test.cpp`, `kernel/commands/BUILD.gn` — `samplerclaim.test`.
- `docs/repairs/261007/sampler.md` — item 2 removed.

**Symptom**
Latent with today's programs. Closing a session's last handle cleared the claim at once, while a `sampler_read` copying out of it still held a reference, so the session's buffers (up to `SAMPLER_MAX_BUFFER_SIZE` for every possible CPU) stayed allocated. A second `sampler_create` then succeeded and allocated a full set of its own; repeating create, start, stop, a read that stalls, close stacked one set per round.

**Cause**
The claim exists for one invariant, and it is about memory: at most one session's buffers are allocated at any time. That is why `create()` claims before it allocates: several creates allocating side by side could run the kernel out of frames, where the allocator retries instead of failing (`docs/repairs/260908/heap.md` item 3). `release()` gave the claim back when the session ended for its holders, which is a different moment from when its memory goes whenever a reader still holds the object. Read as "one session that callers can use", the old code was right; read as the invariant the claim was made for, it was not. Which of the two the invariant is decided whether this was a bug.

Who touches it concurrently: `create()` (sets it, and clears it on failure), the session's end (cleared it in `release()`, from `on_zero_handles()` or `~Session()`), and every holder of a reference to the `SamplerDispatcher`, which never touches `CLAIMED` but decides when the memory goes. The worst interleaving: a reader takes its reference and stalls in a page fault mid-copy; another thread closes the last handle; a third creates.

**Change**
Members are destroyed in reverse order of declaration, so a member declared first goes last. `Claim` is that member: it holds nothing, and its destructor gives the claim back.

```cpp
class Claim final {
  public:
    Claim() = default;
    ~Claim();
    ...
};
[[no_unique_address]] Claim m_claim; // the first member

Session::Claim::~Claim() {
    Guard<sync::Mutex> guard(LOCK);
    CLAIMED = false;
}
```

`~Session()` runs `release()` first, which takes and drops the control lock; then the members go, `m_claim` last, which takes the lock again. The two holds are one after the other, never nested, so the non-recursive mutex is safe. `create()`, before and after:

```cpp
// Before: the claim was given back by hand on every failure.
// After: a session holds it from the moment it exists.
auto session = ustl::Box<Session>::adopt(new (mm::GAF_KERNEL) Session(...));
if (!session) {
    Guard<sync::Mutex> guard(LOCK);
    CLAIMED = false; // no session was built to hold it
    return Status::NoMemory;
}
if (auto const status = session->allocate(records_per_cpu); status != Status::Ok) {
    return status; // the session's destructor gives it back, after its memory
}
```

For a session no one else holds, nothing changes: its last handle closing destroys it on the spot, and the next create succeeds as before. Rejected: making `release()` wait for readers, which would block a handle close on a user copy.

**Reference**
None, this project's own design. The idiom, a member whose destructor runs after every other member's, is standard C++ (`[class.dtor]`: members are destroyed in reverse order of declaration).

**Verification**
- clang-format clean on the changed lines and on the new file; `format_gn.py` clean; clang-tidy (the worktree's `out/sampler-iso`) reports nothing on them.
- In `.worktrees/wip-proc-self` at baseline `521c6664` plus only this change: build ok, `dbg PASS 35 passed, 3 skipped, 0 failed`, `rel PASS 33 passed, 8 skipped, 0 failed, 6/6 required`, host tests 144 passed. `kshell_run.py samplerclaim.test`: `ok -- 3 checks`.
- With `session.hpp` and `session.cpp` back at baseline and the test kept: `samplerclaim.test: FAIL  another is AlreadyExists while a reference keeps the released one's memory`, `FAILED -- 1 of 3 checks`.

**Deliberately not done**
None.

**How it was found**
Reviewing the sampler while writing the offline report's walkthrough (2026-10-07), recorded as `docs/repairs/261007/sampler.md` item 2.

**The user's prediction**
Asked for the invariant first: "CLAIMED 保护同一时间只有一个 Sampling Session 存在或活跃" (one session exists or is active at a time). That is the claim's outside meaning, and under it the old code was correct; the invariant it was made for is the memory bound, which `create()`'s comment states. The rest of the argument was not given.

**Three questions you should be able to answer**
1. Why was the old `release()` correct under "one usable session at a time" and wrong under "one session's memory at a time", and which of the two does `create()`'s claim-before-allocate exist for?
2. Why does declaring `m_claim` first make it run after the buffers are freed, and why is taking the control lock in its destructor safe although `~Session()` takes it too?
3. Why can a program not reproduce the stalled read, and what in `samplerclaim.test` stands in for it?

## The dump header says which user ticks num_unmatched cannot count; a user record's CPU and time stay the walk's (2026-10-11)

**Files**
- `system/public/ours/sampler.hpp` — `SamplerDumpHeader::num_unmatched`: the user ticks that came to nothing are counted nowhere, and how many that can be.
- `docs/repairs/261007/sampler.md` — item 3 closed without a code change; the reasons follow.

**Symptom**
Item 3 named two things. A user tick that marks a thread can come to nothing without a trace: the thread is killed before it walks, or walks after the run has stopped or another has started. And a user record's timestamp and CPU are those of the walk, not of the tick.

**Cause**
Neither turned out to be a defect in the code, but the first was one in the documentation.
- The record's CPU and time are already defined by the ABI as the walk's: `SamplerRecord::cpu` is "the CPU whose buffer holds the record", and for a user record that must be the walking CPU, since a buffer has one producer, its own CPU with interrupts off. Writing the tick's CPU into the field while the record sits in another CPU's buffer would split one documented meaning into two for little gain: the timestamp "only orders samples", and on one-shot timers the walk follows the tick within microseconds unless the thread is preempted between them.
- The lost ticks cannot be counted safely in two of their three cases: when the run has stopped or another has started, the run that marked the thread may already be destroyed, and a stopped one is being read, so there is nowhere to count them without a race. What the header did wrong was to promise, in `num_unmatched`'s comment, that records, drops and unmatched ticks add up to the session's ticks.

**Change**
`num_unmatched`'s comment now says the sum is short by the user ticks that came to nothing, why they are counted nowhere, and the bound: at most the threads still marked when a run ends, plus the killed ones. Rejected: the two counting schemes the repair item offered (count a killed thread's tick against a still-matching run; reserve the count at the tick and take it back on success), since neither covers the stopped-run case, and the second would race a late decrement with a read. This closes the item on the assistant's call while the user had asked not to be asked again (2026-10-11); it is open to overruling.

**Reference**
None, this project's own design.

**Verification**
In `.worktrees/wip-proc-self` at baseline `d6ea735a` plus only this comment: build ok, dbg PASS (35 passed), rel PASS (33 passed, 6/6 required), host tests 144 passed.

**Deliberately not done**
The tick's timestamp in user records, for the reason above.

**How it was found**
Reviewing the sampler for the offline report (2026-10-07); on reading the code to fix it, the ABI turned out to define the CPU and time as the walk's already.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must a user record go into the buffer of the CPU that walks the stack, not the one the tick landed on?
2. In which of the three ways a marked thread can lose its sample is there still a session to count it against, and why not in the others?
3. What bounds how far num_unmatched can fall short?

## prof-fold checks an ELF's section and symbol tables against the file before reading them (2026-10-11)

**Files**
- `kernel/lib/sampler/cli/main.cpp` — `Elf::within()`, a range check that cannot wrap; `read_symbols()` uses it for the section table, before forming the pointer, and for each symbol table.
- `docs/repairs/261005/prof.md` — item 4 removed.

**Symptom**
A crafted ELF crashed prof-fold. With `e_shoff = 2^64 - 0x4000 * 64` and `e_shnum = 0x4000` the bounds check passed and the first section-header read faulted (exit 138, SIGBUS). A second case the repair item did not name: a symbol table whose `sh_offset` lies past the file was never checked at all (exit 139, SIGSEGV).

**Cause**
The section-table check was `e_shoff + e_shnum * sizeof(header) > file size`: with `e_shoff` near 2^64 the sum wraps to a small number and the check passes, and the table pointer had already been formed before it. The symbol table was read at `sh_offset` for `sh_size / 24` entries with no check. Every offset and size in an ELF header is the file's claim, not a fact, so each range has to be checked in a form where no sum can wrap: `offset <= size && length <= size - offset`.

**Change**

```cpp
auto within(u64 const offset, u64 const size) const -> bool {
    return offset <= m_bytes.size() && size <= m_bytes.size() - offset;
}
```

`read_symbols()` refuses a section table outside the file before it forms `shdrs`, and skips a symbol table outside it. The string table was already read through `string_at()`, which bounds each read.

**Reference**
None, this project's own tool.

**Verification**
- clang-format clean; clang-tidy (`out/bringup.x86`) reports nothing on the changed lines.
- Two crafted ELFs in a scratch directory, run as `prof-fold --elf <file> --samples <file>`: baseline exits 138 and 139, the fix exits 1 (no symbols, then no PROF1 header).
- The fixed prof-fold folds rel's real dump against `vmours.with-tests` as before (kernel frames named, `pid 2326;tid 2335;x86_syscall;syscall_dispatch;...`).
- In `.worktrees/wip-proc-self` at baseline `8932da41` plus only this change: dbg PASS, rel PASS (6/6 required), host tests 144 passed.
- No automated test: prof-fold has none yet (`docs/repairs/261005/prof.md` item 8).

**Deliberately not done**
`e_shentsize` larger than the header struct is still read as if equal; the repair item did not name it, and no linker emits one.

**How it was found**
The audit of 2026-10-05 (tools-tests-10, claims-5), recorded as `docs/repairs/261005/prof.md` item 4; the symbol-table case while fixing it. On this host the first attempt at the second case, `sh_offset = 2^63`, did not crash: arm64 macOS ignores a pointer's top byte, so the read landed back inside the buffer. `2^40` shows it.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `offset + size > file_size` fail as a bounds check, and what form does not?
2. Why does forming the pointer before the check matter, even if nothing reads through it yet?
3. Why did an out-of-file offset of 2^63 not crash on this Mac, and what does that say about testing bounds bugs by crashing?

## The dump header's num_dropped saturates instead of wrapping (2026-10-11)

**Files**
- `kernel/lib/sampler/src/dump.cpp` — `Recorder::make_header()`.
- `system/public/ours/sampler.hpp` — `SamplerDumpHeader::num_dropped`: 0xFFFFFFFF reads "at least".
- `docs/repairs/261005/prof.md` — item 1 removed.

**Symptom**
`make_header()` stored `static_cast<u32>(totals.dropped)`, the low 32 bits of a u64 sum that grows for as long as a full buffer keeps being sampled. On the periodic tick that took months to pass 2^32; with one-shot timers the 10 us period is real, and four CPUs pass it in about three hours. `sampler_read` would then report a small loss for a session that lost billions of samples.

**Cause**
A narrowing cast of a count with no upper bound. The ABI gave the reader no way to tell a wrapped count from a small one.

**Change**
`header.num_dropped = static_cast<u32>(std::min<u64>(totals.dropped, ~u32{0}));`, and the field's comment says that its largest value means "at least". Rejected: widening the field to u64, which changes the header's layout and needs a version 3 for a count that is already telling the reader "you lost almost everything" at 4 billion.

**Reference**
None, this project's own ABI.

**Verification**
clang-format clean; clang-tidy reports nothing in `dump.cpp`. In `.worktrees/wip-proc-self` at baseline `c94d756d` plus only this change: dbg PASS, rel PASS (6/6 required), host tests 144 passed; profile still reports its drops (`56 dropped`). No test: reaching 2^32 drops takes hours of sampling, and `RecordBuffer` has no way to set its cursor from outside.

**Deliberately not done**
None.

**How it was found**
The audit of 2026-10-05 (syscall-security-3), recorded as `docs/repairs/261005/prof.md` item 1; one-shot timers made it reachable in hours.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is a saturated count honest where a truncated one is not, and what does the reader learn from 0xFFFFFFFF?
2. Why did one-shot timers turn this from a months-long case into an hours-long one?
3. What would widening the field have cost the ABI?

## The frame-pointer walk's documentation names the prologue and epilogue windows, not only leaves without a frame (2026-10-11)

**Files**
- `system/public/ours/sampler.hpp` — `SamplerRecordFlags::FromUser`.
- `kernel/lib/sampler/include/sampler/frame_pointers.hpp` — `walk_frame_pointers()`.
- `docs/repairs/261005/prof.md` — item 2 removed.

**Symptom**
Both comments said that only "a function built without a frame pointer leaves its caller out". A reader of a FromUser record would then trust that a stack from code built with frame pointers is whole. It is not: a tick at a function's first byte, or at its `ret`, records the grand-caller's return address straight after the pc, and the caller is missing with no flag. Such ticks are common: 39 of 629 kernel records in the audit's captured runs sat at a function's first byte.

**Cause**
The walk takes the next return address from the frame the interrupted frame pointer names. A function sets its own frame up with `push rbp; mov rbp, rsp` and tears it down with `pop rbp` or `leave` before `ret`; in those two windows rbp still names the caller's frame, in every function. The comment described the one case that `-mno-omit-leaf-frame-pointer` already removed for user code, and not the two that no build flag can.

**Change**
Both comments now say when the caller goes missing: whenever the frame pointer still names the caller's frame, which is in any function's prologue and epilogue, and throughout a function without a frame pointer; the ABI comment adds that user code here keeps frame pointers everywhere, so only the two windows remain.

**Reference**
None, this project's own documentation. Unwinding through `.eh_frame`, as the kernel target does, has no such windows: the CFI describes every instruction's frame.

**Verification**
Comments only. In `.worktrees/wip-proc-self` at baseline `64ccf02c` plus only this change: dbg PASS, rel PASS (6/6 required), host tests 144 passed.

**Deliberately not done**
Flagging such records: telling a prologue pc from any other needs the function's CFI, which a frame-pointer walk exists to do without.

**How it was found**
The audit of 2026-10-05, recorded as `docs/repairs/261005/prof.md` item 2.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. In which instructions of a function does rbp still name its caller's frame, and why?
2. Why does `-mno-omit-leaf-frame-pointer` not close these windows?
3. Why does the kernel's `.eh_frame` unwind not have this problem?
