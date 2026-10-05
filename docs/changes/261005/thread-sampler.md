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
