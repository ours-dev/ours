# sync-lock-design: review of the branch fix/sync-lock-design (70e32d09..55a37013)

**Date**: 2026-10-21

## Scope and method

The branch has 18 commits on top of `70e32d09` (2026-08-18), `baseline/20260824` being 1,240 commits past that point. `origin/fix/sync-lock-design` is at `9efdb861`; the local branch carries three more that were never pushed: `800c3210` (SeqLock), `a6cbb1db` (`OURS_DEBUG`) and `55a37013` (a work-in-progress checkpoint of 2,839 files). The checkout of the branch, `ours.sync-locks`, has 590 uncommitted paths on top of `55a37013`.

Verdict first: **nothing here needs merging.** The 17 real commits, 2026-08-19 to 2026-08-22, repair the sync module as it was then, and every one of them is in the baseline today, rewritten as the tree moved (table below). The one commit that is not, `55a37013`, must never land.

Read: every commit message and the diffs of `bd9f44ec`, `0f64ad02`, `4ba62933`, `8ee2d372`, `d4efb7d4` and `800c3210`; against them, the baseline's `kernel/kernel/sync/spin_lock.cpp`, `kernel/include/ours/sync/spin_lock.hpp`, `seqlock.hpp`, `kernel/include/ours/task/wait_queue.hpp`, `kernel/kernel/task/wait_queue.cpp` and `idle_thread.cpp`. Authorities: `modern-cpp-development`, `ours-kernel-design` (its `task.md`), and Linux's `include/linux/spinlock_api_smp.h` for the lock-release order.

Where each commit stands in the baseline, by reading it there:

| Commit | What it did | In the baseline |
|---|---|---|
| `50213a2d`, `ed16d56a`, `d31c15ab`, `c9573b42` | moved the sync module to `kernel/kernel/sync`, GN `source_set`, renames, reformat | `kernel/kernel/sync/` with its `BUILD.gn` |
| `8ee2d372` | `arch::spin_hint()` instead of `halt` in the spin loops | `spin_lock.cpp:90` (include), `:157` (use) |
| `bd9f44ec` | wait queue ordered by `(vdeadline, arrival)`, wakeups that work, dequeue reports whether it removed the waiter | `wait_queue.hpp:59-101` (`QueueKey`), `wait_queue.cpp:150-181`, `wait_queue.hpp:237` (`dequeue_waiter() -> bool`) |
| `0f64ad02` | preemption option mapping inverted, interrupts restored before the lock dropped, `DisableIrq` ignored, `try_acquire` with a weak exchange | `spin_lock.hpp:106-108` (release, then restore), `:135-137` (`DisableIrq` is a `static_assert`), `spin_lock.cpp:126` (strong exchange); the option mapping is gone with the old option machinery |
| `4ba62933`, `9efdb861` | spin on a load, failure order relaxed; why the lock waits as it does | `spin_lock.cpp:1-85` (the same argument as the file header), `:126`, `:150-160` |
| `d4efb7d4` | one idle thread per CPU | `idle_thread.cpp:136-148`: the constructor and `init()` take the CPU they build for |
| `a3ca6623`, `ca3349bc`, `7e0d3172`, `d046f988`, `093108fb` | placeholder options documented, `TA_*` names, `CpuStopper` and the wait queues out of sync into `ours::task` | `task/cpu_stopper.cpp`, `ours/task/wait_queue.hpp`, `TA_*` in `compiler.hpp` |
| `800c3210` | `SeqCount` and `SeqLock` | `seqlock.hpp:54` (`SeqCount`), `:156` (`SeqLock`) |
| `a6cbb1db` | `OURS_DEBUG` defined wherever `KERNEL_DEBUG_LEVEL` is | referenced in three files of the baseline |
| `55a37013` | checkpoint of the working tree | **no, and it must not be** (H1) |

Not verified: that the branch's own code compiles. Its messages say it does not (`bd9f44ec`: "wait-queue.cpp itself still fails"), and the tree around it is the Aug-era Bazel-to-GN migration, so clang-tidy against a compile database was not possible. `clang-format --dry-run` on the branch's 16 sync source files with today's `.clang-format` flags 15 of them; the config changed since, and that says nothing about the code, so it is not counted below.

## What is done well

- The commit messages find the root cause and say it. `0f64ad02` explains that `kIsAutoPreemptDisable` tested `Option::NoTrace` instead of `Option::DisablePreempt`, so with `_bit` being `1 << N` the mapping came out exactly inverted and the default `SpinLock` did not disable preemption. `d4efb7d4` finds that `init_idle_task_percpu` took a `CpuNum` and used the running CPU's in both lookups. A reader a year on gets the bug, not only the patch.
- `bd9f44ec` keeps the lock order straight: the key is snapshotted at enqueue because a thread's scheduling context is guarded by its PI lock, "which must never be taken beneath a queue's spin lock"; re-keying on a PI change is a separate, explicit call. The baseline kept the sentence (`wait_queue.hpp:59-61`).
- `800c3210` states at the top of the file the three properties the type cannot enforce (read functions without side effects, serialised writers, no reader on the writer's CPU), and names the 8-bit counter's wrap after 128 writes as the bug it was. The baseline's `seqlock.hpp` has the same split and the same four fences (`:110-147`).
- `bd9f44ec` and `0f64ad02` record what they did not do: the first says the file still fails to build; the second says why the saved interrupt state is not kept inside the lock ("breaks under nesting").

## High severity

### H1. `55a37013` commits toolchains and build output

The commit adds 414,130 lines in 2,839 files: 1,694 under `prebuilt/clang`, 83 under `prebuilt/gcc`, 244 under `out/bringup.x86` (including `.ninja_deps`, `.ninja_log`), 20 of them binary. `CLAUDE.md` says toolchains come from upstream releases into `prebuilt/` and are not versioned; a build directory is output, not source. It is local only (`origin/fix/sync-lock-design` stops at `9efdb861`), so nothing is lost yet.

Why it matters: pushed, it stays in the history of the remote for good, and a rewrite of a published history is the expensive kind. Merged, it puts a compiler in the baseline. The branch's own useful commits are not affected, so the fix is to leave this one out: do not push the branch as it is, and take nothing from `55a37013`. Nothing needs it: the migration it checkpoints has landed.

## Medium severity

### M1. 590 uncommitted paths sit in `ours.sync-locks`

578 modified, 3 deleted and 9 untracked, in `system/lib` (182), `kernel/lib` (162), `kernel/arch` (57), `kernel/mm` (36), `kernel/include` (29) and others, on top of the checkpoint. They are the continuation of the Aug-era GN migration, which the baseline did by another route, so most of this is probably superseded, but git cannot say which part is not. Read-only judgement.

Fix: before the checkout is removed, keep a patch of it (`git diff > ...`, plus the nine untracked paths), and look through it once for anything that is not in the baseline. Nothing in the review depended on it, and I did not touch it.

### M2. "Verified" meant a probe compile, not a build and a boot

`bd9f44ec`: "semaphore.cpp now compiles … wait-queue.cpp itself still fails". `800c3210`: "a probe TU instantiates both types". `0f64ad02`: "a probe TU instantiates all three shipping variants, and a negative probe confirms the DisableIrq guard fires". These are honest statements, and they leave claims such as the lock discipline of enqueue and dequeue, and the race between a timeout and a notify, untested at the time. The baseline has the same code and `waitqueue.test`, `semaphore.test` and `mutex.stress` over it. Worth knowing when reading the messages: "verified" there is weaker than `ours-verify` today.

## Minor and style

- Naming: `kIsAutoPreemptDisable` and the other `k`-prefixed constants of the Aug-era headers are the legacy case style the repo has since dropped (`SCREAMING_SNAKE_CASE`); hyphenated file names (`spin-lock.cpp`, `wait-queue.hpp`) became underscores. The baseline has both fixed.
- The branch name says "lock design" and its commits are mostly repairs; the design (`QueueKey`, test-and-test-and-set, the `SeqCount`/`SeqLock` split) is in the messages and in the file headers, which is where the baseline carries it.

## Teaching opportunities

### T1. A predicate nobody tests can be inverted for a long time

`kIsAutoPreemptDisable` asked `Option::NoTrace` where it meant `Option::DisablePreempt`, and "nothing anywhere ever tested `Option::DisablePreempt`" (`0f64ad02`). The mapping came out inverted: the lock that said it was safe was not, and the one marked as lightweight was. Two things catch this class of bug. A test that reads the property it names (hold the lock, assert that preemption is off). And a design in which the wrong thing does not compile: the baseline's `lockdep_option_required()` makes a `Guard` that names no option a compile error (`spin_lock.hpp:62` and `:202`), and `DisableIrq` a `static_assert` (`:135-137`). The second is the stronger one, since it works for the next caller too.

### T2. Release the lock, then restore interrupts

`release_irq_restore` restored the interrupt state before dropping the lock. The interleaving: CPU A holds `L` with interrupts off and has restored them; before A has stored the release, an interrupt arrives on A, its handler takes `L` and spins on a word that only A, now inside the handler, could release. The invariant: while a CPU holds `L`, nothing on that CPU that takes `L` may run. The order that keeps it is release first, restore second, and that is what the baseline does (`spin_lock.hpp:106-108`) and what Linux does in `__raw_spin_unlock_irqrestore()` (`include/linux/spinlock_api_smp.h:172-180`: `do_raw_spin_unlock()`, then `local_irq_restore()`, then `preempt_enable()`). Reversing it is not a performance choice; it is the lock's correctness, and only an interrupt on that exact CPU in that window shows it.

### T3. Test-and-test-and-set, and the failure order of the exchange

A failed compare-exchange still takes the line exclusively, so waiters that retry by exchanging take it from each other and from the holder trying to release. Waiting on a relaxed load keeps the line Shared until the holder's release invalidates it once. The exchange then names its failure order: failing acquires nothing, so relaxed, and the default (SeqCst) would put a full barrier on the path that runs most (`4ba62933`; `spin_lock.cpp:1-85`). What it leaves unsolved is fairness, which the file header states as a ticket lock's problem; a good example of writing down what a design does not do.

### T4. A checkpoint of a working tree is not a commit

`55a37013` is what `git add -A` does to a tree that has toolchains and a build directory in it. If a snapshot is wanted, `git stash push -u`, a patch, or a commit of a path list keeps it out of the history. A branch with such a commit is not one to push.

## Suggested follow-up order

1. Do not push `fix/sync-lock-design` as it is, and take nothing from `55a37013`.
2. Keep a patch of the 590 uncommitted paths of `ours.sync-locks` (M1), then look through it once.
3. Remove the checkout `ours.sync-locks`, then the local branch. Nothing of the 17 real commits is lost: they are in the baseline, and 15 of them are also on `origin/fix/sync-lock-design`.
4. Whether `origin/fix/sync-lock-design` stays is the user's call; it holds 15 of the 17 and none of the three unpushed ones; the two it lacks, SeqLock and `OURS_DEBUG`, are in the baseline.
5. No merge.
