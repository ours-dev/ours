# sampler changes

## kernel/lib/prof is gone: its buffers join the sampler's core, and its unbuilt trace sites are deleted (2026-10-07)

**Files**
- `kernel/lib/prof/include/prof/{record_buffer,sink}.hpp` → `kernel/lib/sampler/include/sampler/{record_buffer,sink}.hpp`:
  - The namespace `prof` became `sampler`.
  - Their comments no longer promise a tracer that would share them.
- `kernel/lib/prof/include/prof/trace.hpp`, `kernel/lib/prof/examples/trace_examples.cpp`, `kernel/lib/prof/{BUILD.gn,README.md,.clang-format,.gitignore}` — deleted.
- `kernel/lib/sampler/BUILD.gn` — `:core.headers` lists the two headers and no longer depends on `//kernel/lib/prof:headers`.
- `kernel/lib/sampler/include/sampler/{buffer,recorder}.hpp`, `src/dump.cpp`, `test/recorder_test.cpp`, `README.md` — `prof::RecordBuffer`, `prof::RecordSink`, `prof::BufferStats`, `prof::LineWriter` and `prof::write_hex_line` lose the `prof::`.
- `.agents/skills/ours-kernel-design/references/task.md` — there is no separate prof library.

**Symptom**
Latent: a library with no reason to exist. After the sampler's core moved out, `kernel/lib/prof` held three headers:
- Two had exactly one user, the sampler.
- The third, `trace.hpp` (`Tracepoint`, `TraceScope`), had none. Its driver, the `Tracer`, was never written. Its only example, `examples/trace_examples.cpp`, was in no `BUILD.gn`, so it had never been compiled. No design document plans a tracer.

**Cause**
The buffers were split out as "what a sampler and a tracer have in common" when a tracer was expected. A shared abstraction built for a second user that never came costs a library boundary, a namespace and a dependency edge for nothing, and its comments described a design that did not exist.

**Change**
- The two headers moved as they were, apart from the namespace and the comments. `RecordBuffer` and `RecordSink` stay templates on the record type: nothing in them depends on what a record holds, and making them concrete would be a change of its own.
- `trace.hpp` and its example were deleted. This was the user's call (option A), since a planned-looking scaffold is not deleted on cleanup alone. A tracer that comes later can take them back from git, along with a shared buffer if it then has a real second user.
- Rejected, option B: keeping prof for `trace.hpp` alone. The trace sites would then have needed either a buffer of their own or a dependency on the sampler, the wrong way round.

**Reference**
None, this project's own layout. Zircon's sampler, `zircon/kernel/lib/thread_sampler`, keeps its buffers inside the one library too.

**Verification**
- A fresh output directory (`out/fresh-prof`), in `.worktrees/wip-proc-self` at bb3d2b18 plus this change. `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`.
- Host: `sampler/test/recorder` 20/20, `sampler/test/format` 4/4.

**Deliberately not done**
- Making `RecordBuffer`/`RecordSink` non-templates.
- Splitting the kernel-stack unwinding out of `Recorder` (the user's next request), a change of its own.

**How it was found**
The user asked whether prof still needed to exist ("prof这个库还有存在的必要吗").

**The user's prediction**
None.

**Three questions you should be able to answer**
- What did keeping `RecordBuffer` in a separate library cost while it had one user?
- Why could `trace_examples.cpp` sit in the tree without anyone noticing it no longer compiled, or never had?
- Why would keeping prof for `trace.hpp` alone have reversed the dependency between the two libraries?
