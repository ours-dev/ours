# kernel-time outstanding repairs

## 1. Two `publish_anchor` calls can overlap and leave the vDSO sequence even on a torn payload

**Severity**
Medium: the seqlock protocol is then unable to tell a coherent snapshot from a torn one, but only if two source enrolls overlap after the vDSO is bound. Boot enrolls today run one after another on the laborer.

**Symptom**
Latent, cannot be triggered today unless two CPUs call `ClockSource::enroll()` at once after `lay_out_vdso()` has bound the page. A vDSO `ours_clock_get_monotonic` would then scale a mix of two anchors and could go backwards or jump.

**Cause**
`SeqCount` rule 2 (`kernel/include/ours/sync/seqlock.hpp`): writers must be serialised; the sequence does not do it. `publish_anchor()` (`kernel/kernel/time/anchor_page.cpp`) races on the globals `SEQ` and `STORE`. `Clock::m_seqlock` covers only the in-kernel `Anchor`. `SteadyClock::notify()` (`kernel/kernel/time/clock.cpp`) drops that lock, then calls `publish_anchor(CLOCK.reading())`. `ClockSource::enroll()` (`kernel/kernel/time/clock_source.cpp`) drops `REGISTERED_SOURCES_MUTEX` before `SteadyClock::notify(best)` and assigns `CURRENT_CLKSRC` only after. Bind at userboot is single-threaded and is not this race.

**Change**
Hold a lock across the whole odd/even VMO store, or make `CURRENT_CLKSRC` / notify a single-writer. Ruled out for the published-data finder cut: that change does not touch `publish_anchor()`. Do not pretend `m_seqlock` already covers it.

**Reference**
`SeqCount` rule 2 in `kernel/include/ours/sync/seqlock.hpp`. Linux `update_vsyscall()` takes the timekeeper's seqlock write side around the vvar store (`vdso_write_begin` / `vdso_write_end` in `kernel/time/vsyscall.c`). Zircon `VDso::SetMonotonicTicksOffset()` is a single atomic store to one field, not a multi-word payload.

**Acceptance**
Two overlapping enrolls after bind cannot leave `TimeValues::seq` even while `ticks` / `nanoseconds` / `counter` disagree. A unit or kshell test that runs two enrolls, or a lockdep/lock-order note that a named lock covers the store, is enough.

## 2. `publish_anchor` still stores an even sequence after a failed payload write

**Severity**
Medium: a reader would then accept torn words, but only if `VmoDispatcher::write` of the payload fails after the odd sequence has been stored. That write is into a pinned, already-laid-out vDSO VMO and is not known to fail on the boot path.

**Symptom**
Latent, cannot be triggered today on a healthy vDSO VMO. If the payload `STORE` returned not-Ok and the even `store_u64` then succeeded, `clock.cpp`'s snapshot would see an even sequence and keep a mix of old and new fields.

**Cause**
`publish_anchor()` bumps `SEQ` to even and stores it even when `stored != Status::Ok` for the payload. The odd-store failure path rolls `SEQ` back because "a page left odd makes every read spin"; the payload-failure path does not, and an even store after a failed payload is worse than leaving it odd.

**Change**
On payload failure, do not store even. Either leave the sequence odd (readers spin, which is loud) or roll back to the previous even without writing the new payload. Ruled out for the published-data finder cut: that change does not touch `publish_anchor()`.

**Reference**
None, this project's own protocol on top of `VmObjectPaged::write` / `store_u64`. Linux `vdso_write_end` runs only after the payload stores.

**Acceptance**
A test, or a reading of the function, showing that a failed payload write never publishes a new even sequence. `ours_clock_get_monotonic` either retries or traps, and never returns a mixed anchor.
