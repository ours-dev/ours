# object changes

## Title: a VMO is the vDSO's when its vDSO code segment is not empty, and the separate m_is_vdso bool is gone (2026-10-06)

**Files**
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `bool m_is_vdso` removed; `is_vdso()` reads whether `m_vdso_code` is empty; `mark_vdso()` asserts the segment it is given is not empty

**Symptom**
Latent, cannot be triggered today. `VmoDispatcher` kept two members for one fact: `m_is_vdso`, and `m_vdso_code`, which is meaningful only when the bool is set. The bool cost 8 bytes per VMO: one byte of `bool`, then seven of padding before the 8-byte-aligned `VdsoCode`.

**Cause**
The codebase's rule for yes/no state (modern-cpp-development, the user's rule of 2026-10-03) is that facts about an object go in a Flags word, not in separate `bool` members. The user pointed at this one.

A Flags word with this one bit would still be a word of its own with its own padding, and the class has no other yes/no fact to share it with. The fact needs no storage at all, though. "This VMO is the vDSO's" and "this VMO's vDSO code segment is not empty" are the same fact:

- an unmarked VMO holds a default `VdsoCode`, whose `size` is 0;
- the vDSO always has code, since its one executable segment is what every process's system calls run.

Two members for one fact is also the shape the style skill warns about: one invariant with two owners can answer two ways. Before this change, a `mark_vdso()` given an empty segment would have set the bool, and `map()` would then have required an executable mapping of exactly zero bytes.

**Change**
Before:

```cpp
    auto mark_vdso(VdsoCode const code) -> void {
        m_is_vdso = true;
        m_vdso_code = code;
    }
    auto is_vdso() const -> bool {
        return m_is_vdso;
    }
    bool m_is_vdso = false;
    VdsoCode m_vdso_code;
```

After:

```cpp
    auto mark_vdso(VdsoCode const code) -> void {
        ASSERT(code.size != 0, "the vDSO's code segment is empty");
        m_vdso_code = code;
    }
    auto is_vdso() const -> bool {
        return m_vdso_code.size != 0;
    }
    VdsoCode m_vdso_code;
```

`ASSERT` rather than `DEBUG_ASSERT`: it runs once per boot, and a release kernel that marked an empty segment would refuse every process's vDSO mapping, a failure far from its cause.

Rejected options:

- **A `VmoFlags` word with a `Vdso` bit.** It follows the rule's letter, but a one-bit word saves nothing over the `bool` and still keeps the second fact.
- **`ustl::Option<VdsoCode>`.** Option keeps its own engaged flag, the same `bool` and padding inside another type.

**Reference**
None, this project's own rule. The idea is the one Linux uses in `mm->context.vdso`: a pointer that is null until the vDSO is mapped serves as both the flag and the value.

**Verification**
- The build of `out/bringup.x86` is clean.
- `scripts/format_cpp.py --check` is clean on the header, and `clang-tidy` reports nothing in it.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required). Every process maps the vDSO at start, so each boot runs `is_vdso()` on the real vDSO VMO, and a wrong answer would fail every process's start.

**Deliberately not done**
The other 35 `bool` members in the tree, outside the third-party trees, are left for a sweep of their own. Each needs its protection checked before it can share a word, and some, like this one, need no storage at all. They are recorded as `docs/repairs/261006/cpp-style.md` item 1.

**How it was found**
The user read `bool m_is_vdso = false;` and pointed at the rule.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a one-bit Flags word save nothing here, while removing the bool saves 8 bytes per VMO?
- What would `map()` have demanded of a VMO marked with an empty code segment before this change, and what stops that now?
- When is a `bool` member better replaced by a fact the object already holds, rather than by a bit in a Flags word?
