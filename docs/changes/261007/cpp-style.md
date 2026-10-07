# cpp-style changes

## Title: every bool member left in the tree says why it stays, and cpp-style repair item 1 is closed (2026-10-07)

**Files**
- `kernel/include/ours/task/auto_preempt_disabler.hpp` — both scope objects' `m_disabled` say why they stay
- `kernel/include/ours/task/auto_migrate_disabler.hpp` — `m_disabled` says why it stays, and that nothing sets it yet
- `kernel/lib/arch/include/arch/auto_interrupt_disabler.hpp` — `m_restore` says why it stays
- `kernel/lib/arch/include/arch/auto_interrupt_enabler.hpp` — `m_were_disabled` says why it stays
- `kernel/lib/gktl/include/gktl/defer.hpp` — `m_invoked` says why it stays
- `kernel/arch/riscv/lib/arch/include/arch/riscv/interrupt.hpp` — `m_sie` says why it stays
- `kernel/include/ours/time/timer.hpp`, `kernel/lib/prof/include/prof/trace.hpp`, `kernel/lib/logz4/kernel.cpp` — the lone `std::atomic<bool>` members `m_cancelled`, `m_enabled` and `m_abandoned` say why they stay
- `docs/repairs/261006/cpp-style.md` — deleted: item 1 was its only item
- `docs/repairs/261007/cpp-style.md` — new: the `bool` fields of plain structs, which item 1's search did not reach

**Symptom**
Latent, cannot be triggered today. After the sweeps recorded today in `object.md`, `mm.md`, `kernel-boot.md`, `phys.md`, `kernel-lib.md` and `system-lib.md`, six `bool` members in `git grep -nE '^[[:space:]]+bool[[:space:]]+m_[a-z_]+'` still had no comment saying why they stay, all in scope objects, and three lone `std::atomic<bool>` members and one `bool const` (`AutoInterruptEnabler::m_were_disabled`), which that search does not match, had none either.

**Cause**
The rule of 2026-10-03 (modern-cpp-development) lets a lone `bool` stay only if its comment says why, so that the next reader does not have to rediscover whether it could be derived or share a word. The repair item put RAII scope objects out of scope for conversion, because their `bool` says whether the object still holds the state it took and is the object's whole reason to exist; but its acceptance asked that the search list only members whose comment says why they stay, so they needed the sentence too. The three atomics are each their object's only yes/no fact, guarded by their own atomicity rather than a lock the object's other state shares, which is exactly why a word would not do; their comments said what they mean but not that.

**Change**
One or two comment lines at each declaration, for example:

```cpp
  private:
    /// Whether this scope still holds preemption off. A scope object's one
    /// fact, so it has no word to share.
    bool m_disabled = false;
```

```cpp
    /// See the class comment. The timer's one yes/no fact, so it has no word
    /// to share; an atomic of its own because cancel() sets it with no lock
    /// held while a handler on another CPU polls it.
    std::atomic<bool> m_cancelled;
```

With these, the item's search lists 21 members, every one with a comment saying why it stays, and item 1 is deleted from `docs/repairs/261006/cpp-style.md`, which leaves that file empty, so it goes. Of the 34 members the item listed, 13 were derived or folded into a Flags word: 2 in kernel/object, 2 in kernel/mm, 4 in the boot path and phys, 3 in kernel/lib and 2 in system/lib; 34 less 13 is the 21 the search lists now.

**Reference**
None, this project's own rule.

**Verification**
- The build of `out/bringup.x86` is clean; the change is comments only. `scripts/format_cpp.py` on every changed file.
- Host test `defer-test`: exit 0.
- `boot_check.py` on the final tree: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present).
- The riscv header is not built by `out/bringup.x86`; its change is a comment.

**Deliberately not done**
- The `bool` fields of plain structs, with no `m_` prefix, such as `elfload`'s `Program { write, execute, relro }`, are not matched by the item's search and were not converted. Some of them cross a boundary (`PackagePayloadHandoff::present` is in the phys handoff), so whether the rule reaches them is a question for the user. They are `docs/repairs/261007/cpp-style.md` item 1.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1's acceptance search.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is a scope object's `bool` out of scope for a Flags word, when an exceptionate's single shutdown fact was not?
- What protects `Timer::m_cancelled`, and why could it not share a word with `m_active_cpu`?
- Which kinds of lone `bool` did today's sweeps keep, and which kinds did they derive or fold into a word?
