# phys changes

## Title: phys ElfImage reads whether an image may carry symbolic tables from its ELF header instead of storing it (2026-10-07)

**Files**
- `kernel/phys/include/ours/phys/elf_image.hpp` — `bool m_allow_symbolic_tables` removed; a private `allows_symbolic_tables()` declared in its place
- `kernel/phys/elf_image.cpp` — `init()` no longer sets the flag; `allows_symbolic_tables()` reads `e_type` from the parsed binary; `relocate()` asks it
- `kernel/phys/include/ours/phys/boot_options.hpp` — `m_aslr_disabled` says why it stays a `bool`

**Symptom**
Latent, cannot be triggered today. `ElfImage::m_allow_symbolic_tables` was a second copy of a fact the image already held, and `phys::BootOptions::m_aslr_disabled` kept a `bool` with no comment saying why, both against the rule of 2026-10-03 (modern-cpp-development).

**Cause**
`init()` set `m_allow_symbolic_tables` to `e_type == ET_DYN` right after storing the parsed binary in `m_binary`, and reset it to false at the top, where it also reset `m_binary`. So the flag was true exactly when `m_binary` held an ET_DYN image: the header's `e_type`, stored a second time. A copy like that is correct only while every path that changes `m_binary` remembers to change the flag too; reading the header each time leaves one place to look. The read is a field load from an object already in memory, on a path (`relocate()`) that runs once per image.

`phys::BootOptions::m_aslr_disabled` is the phys stages' only yes/no option. The rule keeps a lone `bool` when there is nothing to share a word with, provided its comment says so; the comment now does, and says a second option would start the word. The class comment also asks that the field order not change, since it is the handoff layout, which a Flags word of one bit would not have changed but would not have improved either.

**Change**
Before:

```cpp
    m_allow_symbolic_tables = m_binary.value().elf_header().e_type == 3 /* ET_DYN */;
    ...
    bool m_allow_symbolic_tables{false};
```

After:

```cpp
auto ElfImage::allows_symbolic_tables() const -> bool {
    return m_binary.is_some() && m_binary.value().elf_header().e_type == 3 /* ET_DYN */;
}
```

and `relocate()` passes `allows_symbolic_tables()` to the relative pass.

**Reference**
None, this project's own rule.

**Verification**
- The build of `out/bringup.x86` is clean; `clang-tidy` reports nothing in `elf_image.cpp`.
- Host test `boot-options-smoke`: exit 0.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present). Both images load physboot and the kernel through `ElfImage`, and phys modules, which are ET_DYN, through `relocate_and_bind()`.

**Deliberately not done**
The literal `3 /* ET_DYN */` stays: eftl's `SHARED_OBJECT` constants are private to its classes, and adding a public one is a change to eftl, not to this member.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which two statements in the old `init()` had to stay in step for the flag to be right?
- Why does a lone `bool` in `phys::BootOptions` stay, when three in the kernel's options became a word?
- What would a Flags word have changed in the phys handoff layout?
