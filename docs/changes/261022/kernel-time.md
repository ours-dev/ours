# kernel-time changes

## Timer has an any member hook and is no longer a hook (2026-10-22)

**Files**
- `kernel/include/ours/time/timer.hpp` — `Timer` no longer derives from `SetBaseHook<>`; it has a private `AnyMemberHook<> m_hook`, a public `is_linked()` that forwards to it, and the option `Timer::ActiveHook` that names the member for a `Set`
- `kernel/include/ours/time/timer_queue.hpp` — `ClockBase::active` is `Set<Timer, Timer::ActiveHook>`

**Symptom**
Not a defect: the user's instruction of 2026-10-22, to make `Timer`'s base hook a member hook, an any hook, with the queue's set storing it.

**Cause**
A `Timer` that inherits from a set hook is a node of a set, by its type: it cannot be put anywhere else, a reader of `Timer` sees a base class that is not a thing a timer is, and the style of this tree prefers a member to a base (`modern-cpp-development`, composition over inheritance). An any hook is one node that the algorithms of any container can work on, so the one member also serves a list or another kind of container later, without a second hook beside it. The timer is in one at a time, which is already true of its queue.

**Change**
```cpp
// before
class Timer: public ustl::collections::intrusive::SetBaseHook<> { ... };
ustl::collections::intrusive::Set<Timer> active;

// after
class Timer {
    ...
    auto is_linked() const -> bool { return m_hook.is_linked(); }
  private:
    ustl::collections::intrusive::AnyMemberHook<> m_hook;
  public:
    using ActiveHook = ustl::collections::intrusive::AnyToSetHook<
        ustl::collections::intrusive::MemberHook<Self, decltype(m_hook), &Self::m_hook>>;
};
ustl::collections::intrusive::Set<Timer, Timer::ActiveHook> active;
```

`is_linked()` is the old call, now a method of `Timer`, so its callers (`timer.cpp`, `timer_queue.cpp`, the destructor's assertion) are unchanged; `insert()`, `erase(iterator_to(...))`, `begin()` and `empty()` on the set work as before, so neither `.cpp` is touched. The alias follows `WaitQueue`'s (`WaiterState::ManagedOption`). The container side of the any hook did not exist and is the previous change (`docs/changes/261022/ustl.md`).

**Reference**
None, this project's own design. Zircon's `Timer` (`zircon/kernel/include/kernel/timer.h:33`) is a `fbl::DoublyLinkedListable` base, and its queue keeps `fbl::DoublyLinkedList<Timer*>` lists with a sorted insertion (`InsertIntoTimerList()`); this tree already differs from it in keeping the armed timers in an ordered set, and now in the hook being a member.

**Verification**
- `sizeof(Timer)` is 104 bytes before and 104 after. I expected it to grow, an any node being larger than a set node in the usual port; in ustl the set base hook, the set member hook and the any member hook are all 32 bytes (measured with a failing `static_assert` in a scratch file), so it does not.
- `ours-verify` on the default build (one-shot): dbg PASS (39 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (37 passed, 8 skipped, 0 failed, 6/6 required); host-tests PASS (146). On a `time_oneshot=false` build both images boot and `timer.test`, `sleep.test` and `preempt.test` are ok.
- `preempt.test`, `timer.test` (the lateness of 1 ms timers, a median of 383 µs here, against 294 to 383 µs in earlier runs; and the hold-off check), `timershare.test` and `timershare.test big`, `sleep.test`, `semaphore.test`, `waitqueue.test`, `tickreplace.test`, `tickbcast.test`, `tickfallback.test` and `mutex.stress 2000`: ok.
- `clang-tidy -p out/bringup.x86` on `timer.cpp` and `timer_queue.cpp`, which include both headers: no diagnostic in them. `scripts/format_cpp.py`: clean.
- Not verified: another container using the timer's hook (nothing does; the test of that is `set-any-hook-test` in ustl).

**Deliberately not done**
- A list of timers on the hook. This only makes it possible.
- Touching `kernel/kernel/time/timer_queue.cpp`: the checkout where the work is merged has an uncommitted edit of someone's in that file (a whitespace change on the `hold_off_until` line), and nothing here needed the file.

**How it was found**
None: an instruction. The step that was not in it, the container side of the any hook, was found by trying the declaration in a scratch file first.

**The user's prediction**
Not given.

**Three questions you should be able to answer**
1. What does `Timer` gain by having its hook as a member and not a base, and what stays the same for the queue?
2. Why is the size of `Timer` unchanged, and what would have made it grow?
3. Why can a timer be in only one container at a time even with an any hook?
