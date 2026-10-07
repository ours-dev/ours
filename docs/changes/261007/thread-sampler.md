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
