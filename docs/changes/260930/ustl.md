# ustl changes

## Title: An Rc refuses to bring a dying object back, and Rc::try_adopt() asks instead (2026-09-30)

**Files**
- `system/lib/ustl/include/ustl/rc.hpp` — `RefCounted::retain()` stops the program when it takes the first reference of an object that is being destroyed; `RefCounted::is_dying()`; `Rc::try_adopt()`; `strong_add()` returns the count before the addition.
- `system/lib/ustl/test/rc_test.cpp` — `test_dying()`.

**Symptom**
Latent, cannot be triggered today: nothing takes a reference to an object from a bare pointer that may be being destroyed. It would be triggered by a walker of an intrusive list whose nodes drop off it in their destructors, as `VmCowPages::m_children` does: a node whose last reference has been released, and whose destructor is waiting for the lock the walker holds, is still on the list, and `Arc::adopt()` of it takes the count from zero to one. The walker's own release then takes it back to zero, and the object is destroyed twice: once there, and once when the destructor that was waiting gets the lock.

**Cause**
`adopt()` is the way an object gets its first owner, and so it has to accept an object with a strong count of zero; and the same zero is also what an object has once its last owner has let go. Nothing told the two apart, so misusing `adopt()` on the second was silent, and its damage, a double destruction, showed up far from the cause. The user asked that the safety be in the types and not in a rule the next person must remember.

The two are distinguishable. Every strong reference holds one weak reference alongside, and the last of those is dropped only after the payload is destroyed (`rc_finish()`). So a strong count of zero with a weak reference outstanding is an object being destroyed, and a fresh object has neither.

**Change**
```cpp
auto retain() noexcept -> void {
    if (strong_add() == 0 && is_dying()) {
        __builtin_trap();
    }
    weak_add();
}
```
It costs nothing on the path that matters: the weak count is looked at only when the strong count was zero, which is the first owner of a fresh object. It stops in every build, at the retain that did it, as `Option` does on an empty access.

`Rc::try_adopt(T *)` is the way to ask instead: it takes a reference only if the strong count is above zero, with the compare-and-swap `lock_strong()` already had for `Weak::upgrade()`, and answers an empty `Rc` otherwise, which the caller must be ready for.

Rejected: a flag in the object saying "has been owned". It would say the same thing as the two counts, in a third place that must be kept in step. Also rejected: making `adopt()` itself return an empty `Rc` for a dying object: a reference someone believes they hold and do not is worse than one that stops the program.

**Reference**
Linux `refcount_inc_not_zero()` and `kref_get_unless_zero()`, the refuse-if-zero increment; Rust `Weak::upgrade()`, which `lock_strong()` already implements here. Zircon's `fbl::RefCounted` tells a fresh object from a used one by starting the count at `kPreAdoptSentinel` (`zircon/system/ulib/fbl/include/fbl/ref_counted_internal.h:26`) and adopting by a compare-and-swap from that value, which asserts in debug builds on anything else. Here the weak count carries the same distinction without changing the initial value of the counter, and the stop is on in every build.

**Verification**
- `scripts/format_cpp.py --check` clean.
- Host tests built with the host toolchain and run: `rc-test` (all rc tests passed, now with `test_dying()`), `rc-race-test` (`ok=182875 fail=1576`; its "fail" is the upgrade that lost the race, as designed), `rc-doc-examples-test` (silent, exit 0).
- `test_dying()`: a fresh object has a count of zero and is not dying; a live object can be adopted again from its bare pointer and the count goes to two and back to one; once the last owner has let go (its release parked by `rc_recycle()` so the state can be looked at) the object is dying, `lock_strong()` and `try_adopt()` refuse, and the count is still zero afterwards.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts, and `vmosnapshot.test` 95 checks, unchanged. Every `Arc` in the kernel goes through the changed `retain()`, and nothing tripped it.
- Not tested: the stop itself. It ends the process, and there is no death-test harness for host tests here.

**Deliberately not done**
- No change to `adopt()`'s contract: a fresh object still has a count of zero.
- No walker uses `try_adopt()` yet; the merge of the next step does not need it.

**How it was found**
The user's review: the merge must not be able to resurrect a node being destroyed, by construction and not by a comment; an earlier suggestion of theirs was that the `Arc` refuse it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- How does the weak count tell a fresh object from one being destroyed, when both have a strong count of zero?
- Why does the check cost nothing on an ordinary `Arc` copy?
- Why does `try_adopt()` answer empty, and `adopt()` stop the program, for the same object?
