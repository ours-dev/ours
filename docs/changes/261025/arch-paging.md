# arch-paging changes

## Title: the page table's map options are one of three existing-entry actions, Error first, and no longer ask for large pages (2026-10-25)

**Files**
- `kernel/lib/arch/include/arch/paging/controls.hpp` — `MapOptions` loses `TryLargePage` and `TryMerge`, stops being a bitmask, and lists `ErrorIfExisting` first

**Symptom**
None that runs: every caller passes `ErrorIfExisting`, and the page table drops the options anyway (repair item 1 of `docs/repairs/261024/arch-paging.md`, which the next changes fix).

**Cause**
The enum mixed two kinds of thing in one bitmask, and got both wrong.

**The three existing-entry actions are a choice of one.** They were 0, 1 and 2, in a bitmask, with `SkipIfExisting` as 0. So a value nobody chose, or the empty mask, meant Skip: a map that silently leaves whatever was there. Error is the safe default; Skip and Overwrite are what a caller asks for when it knows the range may already be mapped.

**`TryLargePage` (bit 2) and `TryMerge` (bit 3) were requests for leaf sizes.**

- Bit 2 has the value 4, so it did not even sit apart from the three actions above, whose values run up to 2.
- No caller passes either bit anymore. Since the large-pages task, the engine picks the largest leaf each step of a range allows.
- `.agents/tasks/arch-leaf-sizes/DESIGN.md` proposes that this stays the engine's choice for good.

**Change**
```cpp
// before
enum class MapOptions {
    SkipIfExisting,
    OverwriteIfExisting,
    ErrorIfExisting,
    TryLargePage = 2_bit,
    TryMerge = 3_bit,
};
USTL_ENABLE_ENUM_BITMASK(MapOptions);
// after
enum class MapOptions {
    ErrorIfExisting,
    SkipIfExisting,
    OverwriteIfExisting,
};
```

The names stay, so no call site changes. The user chose on 2026-10-25 to implement all three actions, Overwrite included, over dropping the parameter. That comes in the next changes.

**Reference**
Zircon `ArchVmAspaceInterface::ExistingEntryAction` (`vm/include/vm/arch_vm_aspace.h`): `Skip`, `Error`, `Upgrade`, a plain enum. Zircon takes no leaf-size option either; `MapContiguous()` "is allowed to use any page size the architecture allows".

**Verification**
- Builds.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.

**Deliberately not done**
- Honouring the actions in the engine: the next changes.

**How it was found**
While fixing repair item 1 of `docs/repairs/261024/arch-paging.md`: the enum was read to decide what implementing it means.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- What did a `MapOptions` value of zero mean before this change, and why is that the wrong default for a map?
- Why are the existing-entry actions an enum and not bits?
- Who chooses a mapping's leaf sizes now, and why is that not a caller's option?
