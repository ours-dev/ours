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

## Walking a stack and storing it come apart: KernelUnwinder turns a kernel frame into addresses, and the Recorder only stores (2026-10-07)

**Files**
- `kernel/lib/sampler/include/sampler/kernel_unwinder.hpp`, `src/kernel_unwinder.cpp` (new):
  - `KernelFrame` holds the interrupted pc, sp and bp, and the stack bounds.
  - `StackRoot` moved here from recorder.hpp.
  - `KernelUnwinder::init(Tables)` takes the `.eh_frame` section, the FDE index and the stack roots.
  - `KernelUnwinder::unwind(frame, ips)` returns an `Unwound`: the depth, and `None` or `Truncated`. This is the old `Recorder::unwind_context()` and `is_stack_root()`, unchanged in logic.
- `kernel/lib/sampler/include/sampler/frame_pointers.hpp`, `src/frame_pointers.cpp` (new) — `walk_frame_pointers()`, moved out of the recorder unchanged.
- `kernel/lib/sampler/include/sampler/recorder.hpp`, `src/recorder.cpp`, `src/dump.cpp`:
  - `SampleContext` keeps only tid, pid, cpu and timestamp. The registers, stack bounds and `from_uspace` are gone from it.
  - `Recorder::Resources` keeps only the buffers, and `init()` returns whether a buffer resolves; `InitFlags` and `has_flag()` are gone.
  - `record_sample()` and `record_user_stack()` became one `record(context, ips, flags)`.
  - `export_dump()` and `copy_dump()` take the kernel's load base, where `Resources::image_base` used to hold it.
- `kernel/lib/sampler/include/sampler/session.hpp`, `src/session.cpp`:
  - The session owns a `KernelUnwinder` beside its `Recorder`.
  - `interrupted_context()` became `current_context()` and `kernel_frame()`, and `kernel_load_base()` goes to the read.
  - The timer handler unwinds, then records; the user-stack writer walks, then records.
- `kernel/lib/sampler/test/recorder_test.cpp`, `test/stacks_test.cpp` (new), `test/BUILD.gn`:
  - The recorder's tests keep buffers and dumps; the copy test now checks the load base it passes.
  - The unwinder's two tests (`AFrameWithNoUnwindInfoEndsTheStackOnlyAtARoot`, `AFrameOffItsStackKeepsItsPcAsAPrefix`), the `StackWindow` tests and the frame-walk tests moved to the new `stacks` executable.
- `kernel/lib/sampler/BUILD.gn`, `README.md`, `.agents/skills/ours-kernel-design/references/task.md` — the new sources, headers and test, and the rule that walking and storing are separate.

**Symptom**
Latent, a design defect the user saw from the API. `Recorder::Resources` held three unrelated things: the buffers, the kernel unwinder's inputs (`eh_section`, `fde_entries`, `stack_roots`), and a dump-header value (`image_base`). The two kinds of stack also took different roads:
- A user stack was walked outside, by the session, and handed in as addresses (`record_user_stack`).
- A kernel stack was unwound inside the recorder (`record_sample`).

To the user, the recorder looked tied to ELF ("为什么要和ELF强关联").

**Cause**
The recorder never parsed ELF: `.eh_frame` reached it as bytes of DWARF call-frame information, and the linker symbols that find them are the session's. What the user sensed was real all the same. The class had two jobs, storing samples and unwinding kernel stacks, because it was first written as the whole profiler, with the PMU and the tick both calling one `record_sample`. When user stacks arrived, they could not be walked in the interrupt (a user read can fault), so they were walked outside. The kernel path stayed where it was, and the asymmetry stayed with it.

**Change**
One road for both kinds of stack: something turns the stack into addresses, then the recorder stores them.

```cpp
// timer handler, kernel frame
u64 ips[MAX_STACK_DEPTH];
auto const unwound = self->m_unwinder.unwind(kernel_frame(*frame), ips);
recorder.record(current_context(), std::span{ips}.first(unwound.depth), unwound.flags);

// a thread on its way back to user space
auto const depth = walk_frame_pointers(pc, fp, {nullptr, read_user}, ips, MAX_STACK_DEPTH);
session->m_recorder.record(current_context(), std::span{ips}.first(depth), RecordFlags::FromUser);
```

What changes at run time: the kernel addresses now go through a 128-byte array on the interrupt stack before they are copied into the record. Before, the unwinder wrote them straight into the record being built. That is one copy of at most 16 words per kernel sample, not measured. The record itself was already built on the stack and copied into the buffer.

The load base moved to the dump calls because it is a fact about the kernel image the kernel addresses belong to, which the session knows. It is not a fact about where records are stored.

Rejected:
- Leaving the unwinding in the recorder and only grouping the unwinder's inputs into a nested struct. That names the two jobs but keeps them in one class, along with the asymmetry.

**Reference**
Zircon's `ThreadSampler` has no kernel-stack unwinding at all: it samples user stacks only, walking them in `SampleThread()` and writing the records itself (`zircon/kernel/lib/thread_sampler/thread_sampler.cc`). Kernel stacks are ours's departure (task.md, Prof). Keeping their unwinder apart from the storage keeps that departure in one class.

**Verification**
- A fresh output directory (`out/fresh-unwind`), in `.worktrees/wip-proc-self` at 051b17d6 plus this change. `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`. `shell: sampler checks passed` on both, and `shell: profile checks passed` on dev.
- Host: `sampler/test/recorder` 9/9, `sampler/test/stacks` 11/11, `sampler/test/format` 4/4. The same 20 recorder-and-stack tests as before, split, plus the load base check.
- prof-fold on the dev log still folds kernel samples to their entry, for example `pid 2320;tid 2329;x86_syscall;syscall_dispatch;...handle_close_shim;...`, so the unwinder unwinds as before.
- Not measured: the extra 16-word copy per kernel sample.

**Deliberately not done**
- Turning `RecordBuffer`/`RecordSink` into non-templates.
- A shared type for a walked stack (addresses plus flags) used by both roads: `std::span` plus `RecordFlags` is enough while there are two.

**How it was found**
The user asked why `Recorder::Resources` was so tied to ELF.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Where does ELF actually appear in the kernel-sampling path now, and why is `KernelUnwinder` still not tied to it?
- Why was a user stack always walked outside the recorder, and what kept the kernel stack inside until now?
- What does a kernel sample cost now that it did not before, and why was that judged not worth measuring here?
