# Plan: SpinLock stores a CPU number, drops preemption handling

Status: **done, 2026-08-30.** Requested 2026-08-30.

Verified with clang-format (idempotent) and clang-tidy (0 errors across the
TU including all three template instantiations) using host homebrew LLVM
23.1.0 — the same version as the repo's vendored `prebuilt/clang/llvm-23` the
GN compile_commands already reference. The `dev-common` container does not
exist in this Docker instance (daemon freshly started, zero containers), so
the documented host-tool fallback from `.agents/tasks/ustl-box/RESULT.md`
applies. Tidy findings on the touched files are all pre-existing on
declarations this change did not alter (`k`-prefix constexpr naming,
`[[nodiscard]]` on `is_locked`/`is_held`); no new findings on lines written.

## The request

1. The lock word holds a CPU number, not the holder's `Thread *`.
2. `acquire()`/`release()` no longer touch preemption state; that becomes
   lockdep `Guard`'s job (via the already-declared
   `LOCKDEP_STATE_SAVER(SpinLock, PreemptDisable, task::AutoPreemptDisabler)`).

## Design notes

- Encoding: the word holds `cpu + 1`, so `0` stays the unlocked sentinel
  (`kUnlockedState` unchanged) and CPU 0 legitimately reads as `1`.
- `holder() -> task::Thread *` is removed outright (no external callers);
  `is_held()`/`assert_held()` compare against the current CPU instead of the
  current thread.
- `Option::DisablePreempt` stays in the enum (it names the axis and the three
  `k*Options` spellings) but joins Monitored/NoTrace as a placeholder: the
  lock no longer implements it.
- `Guard` today stores the `IrqSave`/`PreemptDisable` containers but never
  instantiates them, so between this change and Guard learning to save state,
  nothing disables preemption around a held spin lock. Accepted by the user;
  recorded here as the follow-up that closes the gap.

## Files

- `kernel/include/ours/sync/spin_lock.hpp`
- `kernel/kernel/sync/spin_lock.cpp`
