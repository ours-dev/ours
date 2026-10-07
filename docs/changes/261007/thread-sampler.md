# thread-sampler changes

## A thread keeps the sampler's request as one opaque value, not three loose fields (2026-10-07)

**Files**
- `kernel/lib/thread_sampler/include/thread_sampler/stack_sample_request.hpp`, `kernel/lib/thread_sampler/BUILD.gn` — new `sampler::StackSampleRequest` in its own header-only target, `:request`
- `kernel/lib/thread_sampler/src/thread_sampler.cpp`, `include/thread_sampler/thread_sampler.hpp` — the timer builds the request; `StackSampleRequest::record()` hands it to `Session::sample_user_stack()`
- `kernel/include/ours/task/thread.hpp`, `kernel/kernel/task/thread.cpp`, `kernel/kernel/task/BUILD.gn` — `Thread` holds one `m_stack_sample`; `request_stack_sample()` takes the request; `process_pending_signals()` copies it out with the signal bit and calls `record()`; task depends on `:request` instead of all of the sampler's headers
- `kernel/arch/x86/kernel/BUILD.gn`, `kernel/arch/x86/mm/BUILD.gn`, `kernel/kernel/irq/BUILD.gn`, `kernel/kernel/sync/BUILD.gn`, `kernel/kernel/task/sched/BUILD.gn`, `kernel/kernel/time/BUILD.gn`, `kernel/lib/logz4/BUILD.gn`, `kernel/mm/BUILD.gn` — depend on `:request`, since each compiles a file that includes `thread.hpp`
- `docs/repairs/261006/thread-sampler.md` — removed: its one item is this

**Symptom**
`Thread` declared `m_sample_pc`, `m_sample_fp` and `m_sample_session`, wrote them in `request_stack_sample()` and read all three back to call the sampler. The user asked why the thread should see these at all.

**Cause**
The three values are one fact, and it is the sampler's: what they mean, that they travel together, and that the run number lets a late request be dropped. Held as loose members of `Thread`, that invariant had no owner, and a change to it meant editing the task layer.

**Change**
The sampler owns the type and the thread only carries it:

```cpp
// The timer, in thread_sampler.cpp
task::Thread::Current::request_stack_sample(
    StackSampleRequest(frame->pc(), frame->fp(), self->m_generation));

// On the way back to user space, in thread.cpp
auto request = sampler::StackSampleRequest{};
{ /* PI lock */ clear SampleStack; request = current_thread->m_stack_sample; }
arch::AutoInterruptEnabler const irqs_on;
request.record();
```

The thread never names pc, fp or the run. The request is copied out under the lock that clears the bit, so what is walked is the request that bit stood for.

The type has a header of its own with nothing beyond the prelude, because `Thread` holds it by value and so needs the complete type, without the rest of the sampler. `thread.hpp` is included across the kernel, and this repository gives each target the include paths of what it uses, as every user of `thread.hpp` already lists ktl's and gktl's; the eight targets that include it now also list `:request`. `record()` is declared in the header and defined in the sampler's library, which the kernel links.

This follows the repair item another session filed on 2026-10-06, with one difference: the timer constructs the request and hands it over whole, instead of the thread calling a `set()` with the three values, so the values never pass through the task layer.

Rejected: a pointer to an incomplete type (the request would need storage elsewhere, and an allocation in the timer interrupt); including all of the sampler's headers from `thread.hpp`.

**Reference**
None, this project's own design rule: the object owns its invariant (modern-cpp-development).

**Verification**
`git grep m_sample_ kernel/include/ours/task/thread.hpp` finds nothing. In a clean worktree at HEAD with only this change: a full build, and `boot_check.py`: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), `profile: 41 kernel samples, 9 user samples, 0 dropped`. In the shared worktree both images also report `shell: vmo_create_child checks FAILED`, from another session's uncommitted renumbering of `VmoChildOptions` in `system/public/ours/syscall.hpp`; the clean worktree passes it.

**Deliberately not done**
A headers target for `<ours/task/*.hpp>` that carries what those headers include, so that a new include in `thread.hpp` needs no edit in every user; a structural change of its own.

**How it was found**
The user selected the three fields and asked for them to be abstracted; the repair item described the same problem.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the request's header depend on nothing but the prelude?
- Why is the request copied out under the PI lock, together with clearing the bit?
- Why did eight unrelated targets need a new dependency, and what would remove that need?

## The sampler's host-testable core moves into thread_sampler, and prof::Sampler becomes sampler::Recorder (2026-10-07)

**Files**
- `kernel/lib/thread_sampler/include/thread_sampler/{recorder,format,buffer,stack_window}.hpp`, `src/{recorder,dump}.cpp` — moved from `kernel/lib/prof` (`prof/sampler.hpp` became `recorder.hpp`, `src/sampler.cpp` became `src/recorder.cpp`). Namespace `prof` became `sampler`, and the class `Sampler` became `Recorder`.
- `kernel/lib/thread_sampler/test/{recorder_test,format_test}.cpp`, `test/BUILD.gn` — the host tests, moved with it. The `sampler` test became `recorder`, and its fixture `RecorderTest`.
- `kernel/lib/thread_sampler/cli/` — prof-fold, moved; the binary keeps its name.
- `kernel/lib/thread_sampler/BUILD.gn` — two halves:
  - New `:core` and `:core.headers`, buildable in every toolchain, and a `tests` group.
  - The session's `:thread_sampler` and `:headers`, both inside `if (toolchain_environment == "kernel")`.
- `kernel/lib/thread_sampler/include/thread_sampler/thread_sampler.hpp`, `src/thread_sampler.cpp` — `prof::Sampler m_sampler` became `Recorder m_recorder`; the header comment describes one library.
- `kernel/lib/thread_sampler/README.md` — moved from prof and rewritten for the new names and layout.
- `kernel/lib/prof/BUILD.gn`, `kernel/lib/prof/README.md` — prof keeps only `record_buffer.hpp`, `sink.hpp` and `trace.hpp`, header-only; its `prof` source set and `tests` group are gone.
- `BUILD.gn` — `//:default` builds `//kernel/lib/thread_sampler:tests` and `thread_sampler/cli:prof-fold` instead of prof's.
- `kernel/top/BUILD.gn` — drops a stale dependency on `//kernel/lib/prof`. Nothing in `kernel/top` included its headers; only a comment named it.
- `.agents/skills/ours-kernel-design/references/task.md` — the Prof decisions name the new split.

**Symptom**
Latent, a design defect rather than a fault.
- The sampler was split across two libraries by "host-testable versus kernel", not by what each part is.
- `prof::Sampler`'s own comment said it decides "what a sample contains and where it is stored, never when one is taken". That makes it a recorder. The thing that samples was `sampler::Session`, and the object user space holds is a Sampler too: three "samplers" for three things.
- Nothing but thread_sampler used prof's sampler code, so "prof" was in effect thread_sampler's core under another name.

**Cause**
prof was written first, as a standalone, host-testable core, before the kernel object existed. It took its name from what it was then, the whole profiler. When the session moved into `kernel/lib/thread_sampler` (2026-10-05), only the kernel half moved, because the core's value is its host tests, and the old library kept the name and the class. The rule worth keeping was "the core builds and is tested on the host", and that is a property of a GN target, not of a directory. One library with two targets keeps the rule and puts the sampler in one place.

**Change**
The core moved as it was. Its logic is untouched; only names and includes changed:

```cpp
// before                               // after
#include <prof/sampler.hpp>             #include <thread_sampler/recorder.hpp>
prof::Sampler m_sampler;                Recorder m_recorder;          // in namespace sampler
prof::SampleContext{...}                SampleContext{...}
```

What a sampler and a future tracer share stays in prof and is named from the core as `prof::RecordBuffer`, `prof::RecordSink`, `prof::BufferStats` and `prof::LineWriter`. That keeps the dependency pointing one way, thread_sampler → prof.

In GN, `:core` is a plain `source_set` defined in every toolchain, while the session and its headers are kernel-only:

```gn
if (toolchain_environment == "kernel") {
  source_set("thread_sampler") { ... deps = [ ":core", ... ] }
  cpp_interface("headers") { public_deps = [ ":core.headers", "//kernel/mm:headers", ... ] }
}
source_set("core") { ... }   # host, kernel, user alike
```

Rejected, as laid out to the user:
- Option B, keeping two libraries and only renaming `prof::Sampler`. It changes less, but "prof" stays a name for half of something.
- Option C, moving the core into the existing kernel-only target. That loses 24 host tests and breaks the recorded rule.

The double "is it running" state (`Recorder`'s atomic, `Session`'s locked state and `Session::ACTIVE`) is untouched. It is a concurrency question of its own.

**Reference**
Zircon keeps the whole sampler in one library, `zircon/kernel/lib/thread_sampler/` (`include/lib/thread_sampler/thread_sampler.h`). There one `ThreadSampler` class holds the state (`SamplingState`), the per-CPU buffers (`PerCpuBufferRef`), the read token (`ReadToken`) and the sampling itself, and its tests run in the kernel (`tests/`). ours keeps Zircon's location and splits the class in two, so that the recording half can be tested on the host.

**Verification**
- A fresh output directory (`--out out/fresh-move`, so that no object left from the old paths could satisfy a link), in `.worktrees/wip-proc-self` at ee6be06c plus this change. `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`. `shell: sampler checks passed` on both, and `shell: profile checks passed` on dev.
- Host: `thread_sampler/test/recorder` 24/24 and `thread_sampler/test/format` 4/4, the same tests as before under their new names.
- prof-fold from `thread_sampler/cli/` folds the dev log as before, user frames included (`...;main;user_work();spin_inner [user] 5`).
- A clean worktree at ee6be06c generated without error, which is how the GN failure below was pinned on this change.

**Deliberately not done**
- Collapsing the three running states into fewer: a concurrency decision for the user, to be argued separately.
- Moving `trace.hpp`: it is the tracer's, which is not written yet, and stays in prof with the buffers it shares.
- Renaming the `prof-fold` binary or the `PROF1`/`SAMPB` line tags: they are the dump's wire format and the tool's interface, not library names.

**How it was found**
The user asked whether prof's `Sampler` belonged in thread_sampler.

The move then hit a GN trap worth knowing. The first `gn gen` failed in `//kernel/phys/phys_executable.gni` (`No value named "link_output_suffix" in scope "ours_toolchain"`), reached through `//kernel/mm`, a file the sampler never named directly.

The steps to the cause:
1. `gn gen` in a clean worktree at HEAD passed, so the change caused it.
2. `gn desc` there showed that `//kernel/lib/thread_sampler:headers` did not exist in the default toolchain at all.
3. `//BUILD.gn` now named `//kernel/lib/thread_sampler:tests` without a toolchain, so GN loaded thread_sampler's BUILD.gn in the default toolchain for the first time.
4. In that toolchain every target in the file is defined, and the session's unconditional `cpp_interface("headers")` pulled `//kernel/mm:headers`, then `//kernel/phys`, into a toolchain that cannot load them. prof's tests group had never had a kernel-only target beside it.

A first guess, gating with `!is_host_toolchain`, did not help, because the default toolchain is not the host toolchain: there `is_host_toolchain` is false while `toolchain_environment` is "host". The fix gates the session's headers on `toolchain_environment == "kernel"`, the same condition as its source set.

**The user's prediction**
None. The user chose option A (one library, two targets) over B (rename only) and C (move into the kernel-only target).

**Three questions you should be able to answer**
- Why is "the core builds and is tested on the host" a property of a GN target rather than of a library directory, and what would option C have cost?
- Why did naming `//kernel/lib/thread_sampler:tests` from `//BUILD.gn` make GN try to load `//kernel/phys`, and why did `!is_host_toolchain` not stop it?
- Which pieces stayed in prof, and why does the dependency between the two libraries have to run thread_sampler → prof and not the other way?

## One gate for the sampler's writers: Session::ACTIVE, and the recorder keeps no running state (2026-10-07)

**Files**
- `kernel/lib/thread_sampler/include/thread_sampler/recorder.hpp`, `src/recorder.cpp`:
  - `Recorder::start()`, `stop()`, `is_running()`, the `State` enum and its `std::atomic<State> m_state` are gone, and `reset()` empties the buffers.
  - `record_sample()`, `record_user_stack()` and `note_unmatched()` write unconditionally. When they may run is the owner's to say, and the class comment states that contract.
- `kernel/lib/thread_sampler/src/thread_sampler.cpp`:
  - `start()` resets, then publishes with the release store of `ACTIVE`.
  - `stop_locked()` only clears `ACTIVE` and sends the barrier.
  - `on_sample_due()` writes only while `ACTIVE` names its session.
- `kernel/lib/thread_sampler/include/thread_sampler/thread_sampler.hpp` — `ACTIVE`'s comment: the one gate every writer passes.
- `kernel/lib/thread_sampler/test/recorder_test.cpp`:
  - The four tests of the recorder's own state are gone (`StartsAndStops`, `StartingTwiceIsRefused`, `SamplesAreRefusedWhileStopped`, `RecordUserStackIsRefusedWhileStopped`).
  - The start-empties test became `ResetEmptiesThePreviousRunsBuffers`.
  - The other tests write without a start.
- `kernel/lib/thread_sampler/README.md`, `.agents/skills/ours-kernel-design/references/task.md` — the data plane's one atomic is `ACTIVE`; the design rule now also forbids a second copy of the same state.

**Symptom**
Latent. Three pieces of state said whether the sampler was running:
- `Recorder::m_state`: atomic, Stopped/Starting/Running.
- `Session::m_state`: under the control mutex, Configured/Running/Released.
- `Session::ACTIVE`: an atomic pointer to the running session.

The first and third meant the same thing, "writers may write into this session's buffers". They were always set and cleared together, under the same lock, one after the other. Nothing was wrong yet, but two copies of one fact invite the day they disagree, and every reader of the code had to work out which one a given writer obeyed: the timer obeyed the recorder's, the user-stack writer both.

**Cause**
The recorder was written as a standalone core before the session existed. Its own atomic state, with a `Starting` state that let it empty its buffers "in the open", was how it protected itself from any caller. Once the session existed, the session already kept every writer out at the moments that state guarded:
- During a start, this session's timers are not armed and `ACTIVE` names no session.
- After a stop, `ACTIVE` is cleared and the IPI barrier has waited out every writer in flight.

So the recorder's state guarded nothing the session did not, and the `Starting` claim was redundant with the control mutex every `start()` already held.

**Change**
The argument for one gate, as the change rests on it:
- **The invariant.** No write into a session's buffers overlaps a reset of them, or a read or free of them, and every write lands in buffers whose reset it can see.
- **Who touches them concurrently.** The writers are the timer handler on each CPU (interrupt context) and a thread recording its walked user stack (thread context, briefly with interrupts off). The others are `start()`'s reset and the reads, `dump_size()` and `copy_dump()`, both run only under `Session` control.
- **The gate.** Each writer loads `ACTIVE` with acquire and writes only if it names its session. The load and the write are inside one interrupts-off section: by nature in the handler, by `AutoInterruptDisabler` in `sample_user_stack()`.
- **Start:** `m_recorder.reset()`, set the generation, `ACTIVE.store(this, release)`, then arm the timers. Every writer that sees `this` in `ACTIVE` sees the emptied buffers and the generation.
- **Stop:** `ACTIVE.store(nullptr, release)`, then `sync_exec` an IPI to every online CPU and wait. A CPU takes the IPI only between interrupts-off sections, so once all have taken it no write is in flight, and every later writer sees `nullptr`.
- **The worst interleaving.** CPU 1's handler loads `ACTIVE == self` just before CPU 0's stop clears it, and is still writing when the IPIs go out. It is safe because CPU 1 cannot take the IPI until its interrupts-off section ends, and `sync_exec` does not return until it has. The same holds for a user-stack writer that loaded `ACTIVE` and then had its write delayed: it is delayed with interrupts off, so the barrier waits for it too.
- **What changed for the handler.** It used to check `recorder.is_running()`, which the stop cleared one store earlier than `ACTIVE`. The window between the two stores held no writer that the barrier did not already cover, so checking `ACTIVE` instead loses nothing.

What stays, and why it is not a copy: `Session::m_state` (Configured/Running/Released, with `m_readers`) is the control plane. The control calls read it under the mutex to answer BadState, refuse a start under a read, and know a session was released. No writer ever reads it, and it could not, in interrupt context, under a sleeping lock.

Rejected:
- Keeping all three with better comments: the redundancy stays.
- Removing `ACTIVE` and gating on the recorder's state: the user-stack writer needs to find which session runs, and to check its generation, so the pointer cannot go.

**Reference**
Zircon `ThreadSampler` (`zircon/kernel/lib/thread_sampler/thread_sampler.cc`, `include/lib/thread_sampler/thread_sampler.h`) keeps exactly one atomic, `state_`. It packs the `SamplingState` together with counts of writers in flight (`pending_operations_mask`), and a stop waits for those counts to drain. ours keeps one atomic too, but waits out writers with an IPI barrier instead of counting them, which the interrupts-off writers make possible. The control plane is a separate mutex-guarded state.

**Verification**
- In `.worktrees/wip-proc-self` at 2995b908 plus this change, a fresh output directory: `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`. That includes the shell's sampler checks (start, stop, restart, read, a kernel-only session counting unmatched ticks) and profile on dev.
- Host: recorder 20/20 (24 less the four removed), format 4/4.
- Not measured: whether the handler's load of a global (`ACTIVE`) instead of a session member costs anything. Both are one acquire load.
- Correctness rests on the argument above, not on the boots: no test forces the stop-versus-write interleaving.

**Deliberately not done**
- A host test of the gate itself: it lives in kernel-only code now, so the property "a write after a stop lands nowhere" is checked by the argument and the boots, not on the host.
- A stress test that races stop against writers on many CPUs: worth having (lockdep and stress back a concurrency argument), not written in this change.

**How it was found**
While moving the core into thread_sampler, three "is it running" fields showed up side by side.

**The user's prediction**
None. The user asked to go ahead without the usual written argument; it is spelled out in Change instead.

**Three questions you should be able to answer**
- What exactly does the IPI barrier wait for, and why does it require each writer's load of `ACTIVE` and its write to be in one interrupts-off section?
- Why could `Recorder`'s `Starting` state go, and what now guarantees that `reset()` never runs beside a writer?
- Why is `Session::m_state` not a second copy of the running state, when it also has a `Running` value?

## The library is named sampler, like its namespace: kernel/lib/thread_sampler becomes kernel/lib/sampler (2026-10-07)

**Files**
- `kernel/lib/thread_sampler/` → `kernel/lib/sampler/`:
  - The headers moved from `include/thread_sampler/` to `include/sampler/`, so they are included as `<sampler/...>`.
  - The session's header and source are named for their class: `thread_sampler.hpp` → `session.hpp`, `src/thread_sampler.cpp` → `src/session.cpp`.
  - The kernel target `:thread_sampler` → `:sampler`, so the library's default label is `//kernel/lib/sampler`.
  - Include guards: `THREAD_SAMPLER_*` → `SAMPLER_*`.
  - `.clang-format` puts `<sampler/...>` first.
- Every `BUILD.gn` that named a label:
  - `kernel/object` names the library and its headers.
  - `kernel/arch/x86/kernel`, `kernel/arch/x86/mm`, `kernel/lib/logz4`, `kernel/mm`, and `kernel/kernel/{task,task/sched,irq,time,sync}` name `:request`.
  - `//BUILD.gn` names the tests and prof-fold.
- `kernel/include/ours/task/thread.hpp`, `kernel/object/include/ours/object/sampler_dispatcher.hpp` — the includes. `kernel/kernel/time/tick_sched.cpp` — a comment.
- `kernel/lib/sampler/README.md`, `kernel/lib/prof/README.md`, `kernel/lib/prof/BUILD.gn` — the name.
- `.agents/skills/ours-kernel-design/references/task.md`, `.agents/skills/language/modern-cpp-development/SKILL.md` — the decision and a cited path.

**Symptom**
Latent. The library's namespace was `sampler`, its directory and include prefix `thread_sampler`. Its code read `sampler::Session` from `<thread_sampler/thread_sampler.hpp>` built by `//kernel/lib/thread_sampler:thread_sampler`: two names for one thing.

**Cause**
The directory took Zircon's name, `thread_sampler`, when the session moved there, and the user chose `sampler` for the namespace at the same time (2026-10-05). Now that the whole sampler, recorder and session, is in this one library, the user chose one name for both.

**Change**
A rename, with nothing else changed. Zircon's `kernel/lib/thread_sampler` is still named as the reference in the design rules, since it is where Zircon keeps the counterpart.

**Reference**
Zircon `zircon/kernel/lib/thread_sampler/`, the same place under Zircon's name.

**Verification**
- A fresh output directory (`out/fresh-rename`, so that no object of the old paths could satisfy a link), in `.worktrees/wip-proc-self` at b7e29da2 plus this change. `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`.
- Host: `sampler/test/recorder` 20/20, `sampler/test/format` 4/4.
- prof-fold is built at `host/obj/kernel/lib/sampler/cli/prof-fold`.

**Deliberately not done**
- The `SamplerDispatcher`'s name and file, and `<ours/sampler.hpp>`: already named sampler.
- Past change records and task documents that name the old path: history, append-only.
- The change-record module name: later entries go to `docs/changes/<YYMMDD>/sampler.md`.

**How it was found**
The user asked for it ("改成sampler").

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which GN label does `"//kernel/lib/sampler"` resolve to, and why was the kernel target renamed along with the directory?
- Why does the library's `.clang-format` need to change with its include prefix?
- Why is a rename verified in a fresh output directory rather than an incremental one?
