# kernel-top changes

## Title: top, kshell and object answer for every Status they used to drop (system-abi-surface step 2, part 3) (2026-10-03)

**Files**
- `kernel/top/main.cpp` — the laborer's and the two sample threads' `detach()` assert Ok in a debug build; `resume()` is dropped explicitly.
- `kernel/lib/kshell/kshell/shell.cpp` — the console thread, the same way; a comment's stale line number into main.cpp dropped.
- `kernel/object/thread_dispatcher.cpp` — `activate()`'s `suspend()` and `resume()` dropped explicitly.

**Symptom**
Ten of the 44 silent drops counted for step 2: six in kernel/top, two in kshell, two in object. None was wrong today.

**Cause**
They are the spawn-then-start pair seen in kernel/task, in three more callers:
- `Thread::detach()` can fail, but only on a thread already Dead, where it must reap it through `join()`, which is not supported. Each caller detaches a thread `spawn()` has just built, which is in Initial, so it cannot be Dead. That assumption is now checked with `DEBUG_ASSERT(Status::Ok == detached)`.
- `Thread::resume()` answers only Ok (a Dead thread is left alone, and still Ok), and so does `Thread::suspend()`, which is `suspend_or_kill()` setting a signal. These are dropped with `static_cast<void>` and the reason.

**Change**

```cpp
// kshell's console thread, before
thread->detach();
thread->resume();
// after
// detach() fails only reaping a thread that is already Dead, which one
// just spawned is not; resume() answers only Ok.
[[maybe_unused]]
auto const detached = thread->detach();
DEBUG_ASSERT(ours::Status::Ok == detached);
static_cast<void>(thread->resume());
```

**Reference**
Zircon's `Thread::Resume()` returns void, so it has nothing to drop. `Thread::Detach()` returns `zx_status_t` without `[[nodiscard]]`, and its kernel callers drop it the same way ours did: `lk_main()` in `zircon/kernel/top/main.cc` calls `t->Detach(); t->Resume();` on the bootstrap2 thread, and most other callers use `Thread::DetachAndResume()` (`kernel/thread.cc`), whose status they drop too (`lib/lockdep/lock_dep.cc`, `lib/crypto/global_prng.cc`). Once `ours::Status` is `[[nodiscard]]` as a type, every one of these callers has to say what it assumes, which is the point of this step.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); the kshell prompt came up, so the console thread's assertion held, and userboot ran, so the laborer's did.

**Deliberately not done**
- The remaining 8 drops (arch/x86, the i8253 driver, ktl's `Name`), and the attribute itself, in the next commits.

**How it was found**
The count for step 2.

**The user's prediction**
None for this part; see the mm entry of the same day.

**Three questions you should be able to answer**
- Why can a caller that has just spawned a thread assert that `detach()` succeeds?
- What would `detach()` have to do for a thread that is already Dead, and why can it not today?
- Why is a `static_cast<void>` on `resume()` safe now, and what change to `resume()` would make it unsafe?
