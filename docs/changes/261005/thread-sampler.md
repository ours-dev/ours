# thread-sampler changes

## The sampling session moves to kernel/lib/thread_sampler, namespace sampler (2026-10-05)

**Files**
- `kernel/lib/thread_sampler/` — new kernel-only library, scaffolded with copier from `tools/templates/cpp_library`, its host test dropped since nothing in it runs on the host. `include/thread_sampler/thread_sampler.hpp` is the old `<ours/sampler/mod.hpp>` and `src/thread_sampler.cpp` the old `kernel/kernel/sampler/sampler.cpp`, unchanged but for the namespace
- `kernel/include/ours/sampler/`, `kernel/kernel/sampler/` — removed
- `kernel/kernel/BUILD.gn`, `kernel/kernel/task/BUILD.gn`, `kernel/object/BUILD.gn` — task and the object headers depend on the library's headers, the object sources on the library itself, which is what puts it in the link
- `kernel/kernel/task/thread.cpp`, `kernel/object/include/ours/object/sampler_dispatcher.hpp` — include `<thread_sampler/thread_sampler.hpp>`
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/README.md`, `kernel/kernel/time/tick_sched.cpp` — name the new place
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `PLAN.md`, `RESULT.md`, `.agents/skills/ours-kernel-design/references/task.md` — D6 is now the user's decision, with its reason

**Symptom**
None at run time. The user asked why the sampler sat in `kernel/kernel`, with its header under `<ours/sampler/>`, beside task, time, irq and sync.

**Cause**
D6, where the kernel-side session lives, was taken on 2026-10-04 without the user, under their request to go ahead without questions, and from an option list that left out the reference's own answer. Zircon keeps the session in `zircon/kernel/lib/thread_sampler`, a kernel static library whose namespace is `sampler`, and both `kernel/thread.cc` and `object/sampler_dispatcher.cc` depend on it. This repository's layering rule says the same from the other side: `kernel/kernel` holds what the kernel needs for itself (threads, timers, interrupts, locks), and a profiling session is a facility user space asks for through a Sampler object.

The two constraints that were real are both met by a kernel-only library under `kernel/lib`, the kind userabi, syscall and kshell already are. The session is made of the kernel's timers, IPIs and locks, so it cannot join `kernel/lib/prof`, which must stay free of kernel headers to be tested on the host. And `kernel/kernel/task` (a marked thread walks its user stack) and `kernel/object` (SamplerDispatcher) both call it, so it must sit below `kernel/object`. Neither constraint asked for `kernel/kernel`.

The namespace was `ours::sampler`, nested in `ours`, and named so that it would not hide the core library's `prof` from code inside `ours` (an `ours::prof` would). A library of its own takes a top-level namespace, as every kernel library here does (`prof`, `unwind`, `user_copy`, `userabi`, `kshell`), and it is `sampler`, as Zircon's is.

**Change**
Before and after:

```text
kernel/include/ours/sampler/mod.hpp                                  namespace ours::sampler
kernel/kernel/sampler/sampler.cpp                                    linked through //kernel/kernel

kernel/lib/thread_sampler/include/thread_sampler/thread_sampler.hpp  namespace sampler
kernel/lib/thread_sampler/src/thread_sampler.cpp                     linked through //kernel/object
```

The header names what it takes from `ours` in full, as library headers here do:

```cpp
// Before, inside namespace ours::sampler
static auto create(Config const &config, ustl::Box<Session> *out) -> Status;
static sync::Mutex s_lock;

// After, inside namespace sampler
static auto create(Config const &config, ustl::Box<Session> *out) -> ours::Status;
static ours::sync::Mutex s_lock;
```

The source opens `namespace sampler` with `using namespace ours;`, as userabi's does, so its body did not change. Neither did the callers: inside `namespace ours::object` or `ours::task`, the unqualified `sampler::Session` is looked up in `ours::object`, then `ours`, then the global namespace, and with `ours::sampler` gone it finds the top-level `sampler`.

Rejected: folding the session into `kernel/lib/prof` as a second, kernel-only target, which keeps the profiler in one directory but makes a library half host-testable and half not; and keeping `kernel/kernel/sampler` with the departure recorded.

**Reference**
Zircon `zircon/kernel/lib/thread_sampler/`: `BUILD.gn` declares `static_library("thread_sampler")`, and `include/lib/thread_sampler/thread_sampler.h` declares `namespace sampler { class ThreadSampler; }`. `zircon/kernel/kernel/BUILD.gn` and `zircon/kernel/object/BUILD.gn` both depend on it.

**Verification**
In a separate worktree at the same commit: a full build, and `boot_check.py`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required), with `profile: 61 kernel samples, 12 user samples, 0 dropped` and `shell: sampler checks passed`. Then in this worktree: the build, host tests unchanged (prof sampler 22/22, format 4/4, unwind 9/9, cfi 11/11), and `boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required), with `profile: 230 kernel samples, 26 user samples, 5585 dropped`, a run on the slow CPU of `docs/repairs/261005/kernel-top.md`. git records the two moved files as renames, 84 and 99 percent alike, so their history follows them.

**Deliberately not done**
`<ours/boot/>` (the handoff and the boot options, implemented in `kernel/top`) departs from Zircon's layering the same way: Zircon has `kernel/lib/boot-options` and `phys/handoff.h`. It is not this task's, and moving it is a change of its own for the user to decide.

**How it was found**
The user asked why the session was there.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What decides whether a kernel facility belongs in `kernel/kernel` or in a library under `kernel/lib` here?
- Why can the session not live in `kernel/lib/prof`, and what would stop working if it did?
- Why did `sampler::Session` inside `namespace ours::object` keep compiling, unchanged, when the namespace moved from `ours::sampler` to `sampler`?

## Each start of a session draws a new generation, so a restart refuses the last run's marks (2026-10-05)

**Files**
- `kernel/lib/thread_sampler/include/thread_sampler/thread_sampler.hpp` — `m_generation` is per run, no longer `const`
- `kernel/lib/thread_sampler/src/thread_sampler.cpp` — `start()` draws it; the constructor no longer does

**Symptom**
Latent, found by an audit, no run has shown it. After `sampler_stop` and `sampler_start` on the same handle, which the shell's sampler check does, a user-stack mark left by the first run can be recorded into the buffers the second start has just emptied.

**Cause**
7a105fe4 tied a mark to the session that made it with a generation drawn once, in the constructor. But the buffers belong to a run, not to a session: every `start()` empties them, and a run's records must be the samples taken while it ran. The check in `sample_user_stack()` compares the mark's number with the running session's, and a restarted session still has the same number, so a mark from before the stop passes.

The window is the one 7a105fe4 described: a thread marked in user mode acts on the mark only on its way back to user space, and it can be preempted or suspended first. Meanwhile the profiler stops, reads and starts the same session again.

**Change**

```cpp
// Before: once per session, in the constructor
, m_generation(s_next_generation.fetch_add(1, std::memory_order_relaxed)) {}

// After: once per run, in start(), before the session becomes visible
m_state = State::Running;
m_generation = s_next_generation.fetch_add(1, std::memory_order_relaxed);
s_active.store(this, std::memory_order_release);
```

A plain field is enough. It is written only under the control lock, while none of this session's timers is armed and `s_active` does not name it. Its readers are the timer handler, armed by the IPIs `start()` sends after the write, and `sample_user_stack()`, which reads it through the `s_active` acquire that pairs with the release after the write. The counter stays global, so no run of any session reuses a number.

**Reference**
Zircon `ThreadSampler::StopLocked()` (`zircon/kernel/lib/thread_sampler/thread_sampler.cc`) does `session_id_.fetch_add(1)` on every stop, so a restarted sampler refuses the old run's marks; this draws the number at start instead, which comes to the same.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required); `shell: sampler checks passed`, which stops and starts one session, and `profile: 239 kernel samples, 17 user samples, 6056 dropped` on the slow CPU. No test provokes a late mark: that needs a thread preempted between its mark and its return to user space while the session stops and starts.

**Deliberately not done**
The audit's other findings, which are design decisions or separate changes; they are listed in `docs/repairs/261005/` (`thread-sampler.md`, `prof.md`, `unwind.md`).

**How it was found**
A five-lens audit of the task on 2026-10-05; three auditors found it independently.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a generation per session not keep a late mark out of a restarted session?
- Why can `m_generation` be a plain field although the timer interrupt and `sample_user_stack()` read it?
- Which buffers does a run own, and which event empties them?

## sampler_create takes the claim before it allocates, so creates cannot pile up allocations (2026-10-05)

**Files**
- `kernel/lib/thread_sampler/src/thread_sampler.cpp` — `Session::create()` claims under the control lock first, builds outside it, and gives the claim back after a failed session is gone
- `kernel/lib/thread_sampler/include/thread_sampler/thread_sampler.hpp`, `kernel/object/include/ours/object/sampler_dispatcher.hpp`, `system/public/ours/syscalls.gni` — AlreadyExists also while another create is building a session; the control lock is no longer said to be held across allocations
- `docs/repairs/261005/thread-sampler.md` — removed: its one item is this

**Symptom**
Latent: today only the shell's profile check holds the SAMPLING resource. A program holding it could start many threads looping `sampler_create` with the largest buffer. Each call allocated up to 4 MiB for every possible CPU, plus the unwind index, before it learned that a session existed. Enough of them at once run the kernel out of frames, where the allocator retries for ever, holding vmalloc's lock, and every large allocation in the system hangs.

**Cause**
89b0d6e4 moved the allocations ahead of the claim check, so that a session that failed would be destroyed outside the control lock: its destructor takes that lock, which is not recursive. That fixed the deadlock but made every create allocate before it could be refused. ed09eb11's limit then bounded one call, not the memory all calls take together. The upstream code refuses AlreadyExists under its lock before it allocates; that pre-check was lost.

**Change**
The claim is taken first, under the lock, and stands for the session being built:

```cpp
{
    Guard<sync::Mutex> guard(s_lock);
    if (s_claimed) {
        return Status::AlreadyExists;
    }
    s_claimed = true;
}
// build outside the lock; a failed session is destroyed in its own block
Guard<sync::Mutex> guard(s_lock);
s_claimed = false; // only after the failed session is gone
```

At most one create allocates at a time, and a create while a session exists allocates nothing. The order of the failure path keeps 89b0d6e4's fix: the session is destroyed first, outside the lock, and as it is still Released its destructor gives nothing back; then the claim is returned under the lock.

The cost: while one create is building, a second answers AlreadyExists even if the first then fails with NoMemory. That is a transient refusal, documented, and the price of a bound that holds.

Rejected: only adding the upstream pre-check. It stops allocation while a session exists, but creates racing while none exists still all allocate at once.

**Reference**
Zircon `ThreadSampler::SetUp()` (`zircon/kernel/lib/thread_sampler/thread_sampler.cc`) refuses `ZX_ERR_ALREADY_EXISTS` under its lock before allocating its buffers.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required); `sampler_create = Ok`, `sampler_create(a second) = AlreadyExists`, and a create after the last handle of a running session was closed, all in `shell: sampler checks passed`. No test runs creates concurrently: nothing in the boot can start threads that hold the resource.

**Deliberately not done**
None.

**How it was found**
The audit of 2026-10-05 (repair item thread-sampler 1, reported by two auditors).

**The user's prediction**
None. The user asked to continue without a decision card, given their remaining quota; the approach is the recommendation.

**Three questions you should be able to answer**
- Why did moving the allocations ahead of the claim fix a deadlock, and what did it break?
- In the failure path, why must the session be destroyed before the claim is given back, and not after?
- What does a second create see while the first is still allocating, and why is that acceptable?
