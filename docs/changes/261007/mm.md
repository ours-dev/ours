# mm changes

## Title: kernel/mm stores no fact twice: a zone asks its frame set whether it is attached, and a placement's address is optional (2026-10-07)

**Files**
- `kernel/mm/include/ours/mm/priv/frame_buddy.hpp` — `FrameBuddy::is_attached()`: whether it has a frame array
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `FrameSet::is_attached()` forwards to it; `PmZone::m_fset_attached` removed
- `kernel/mm/pm_zone.cpp` — `ensure_fset_attached()` asks `m_fset.is_attached()`
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — `VmPlacement` holds `ustl::Option<VirtAddr>` in place of an address and `bool m_fixed`; `GapAugment::Aggregate::m_empty` says why it stays a `bool`

**Symptom**
Latent, cannot be triggered today. Three `bool` members against the rule of 2026-10-03 (modern-cpp-development): two restated a fact the object already held, and one had no comment saying why it stays.

**Cause**
The rule asks, of every `bool` member: if it restates a fact the object already holds, derive it; otherwise give it a bit of the class's Flags word; keep a lone `bool` only with a comment saying why.

- `PmZone::m_fset_attached` was set in the statement after `m_fset.attach()`, and attach() is what gives the buddy allocator its `m_frames` pointer, which a default-constructed `FrameBuddy` holds as null. So "attached" was stored twice, once as the pointer and once as the flag. Two answers to one question agree only as long as every writer remembers both; the next change that attached or detached a set another way would have had to know about the flag in the zone. The frame set now answers for itself.
- `VmPlacement::m_fixed` said whether `m_base` meant anything. That is exactly what an optional value is: an address or none. The class comment already said the shape it replaced, "a flag beside an address", was the problem; the fix had kept the shape inside the class. With `ustl::Option<VirtAddr>`, `is_fixed()` is `m_base.is_some()`, and `base()` on `anywhere()` traps through the Option in every build, where the `DEBUG_ASSERT` checked only debug builds and a release build returned 0, an address the VMM would have used.
- `GapAugment::Aggregate::m_empty` is the augmented B-tree's identity marker: a subtree with no areas, whose span fields mean nothing. It is the aggregate's only yes/no fact, so there is no word to share. A sentinel could replace it, since every non-empty aggregate has `m_min_start <= m_max_last` and an "impossible" span could stand for empty, but each reader of the aggregate would then have to know the trick. It stays, and its comment says so.

**Change**
The zone, before:

```cpp
auto PmZone::ensure_fset_attached() -> void {
    if (!m_fset_attached) {
        m_fset.attach(MemMap::get().pfn_to_frame(m_start_pfn),
                      static_cast<usize>(m_spanned_frames.load()), m_start_pfn);
        m_fset_attached = true;
    }
}
```

After:

```cpp
auto PmZone::ensure_fset_attached() -> void {
    if (!m_fset.is_attached()) {
        m_fset.attach(MemMap::get().pfn_to_frame(m_start_pfn),
                      static_cast<usize>(m_spanned_frames.load()), m_start_pfn);
    }
}
```

with `FrameBuddy::is_attached()` returning `m_frames != nullptr`.

The placement, before:

```cpp
    static constexpr auto at(VirtAddr base) -> Self {
        return Self{base, true};
    }
    constexpr auto is_fixed() const -> bool {
        return m_fixed;
    }
    constexpr auto base() const -> VirtAddr {
        DEBUG_ASSERT(m_fixed, "VmPlacement::anywhere() has no base to read");
        return m_base;
    }
    ...
    VirtAddr m_base{};
    bool m_fixed{};
```

After:

```cpp
    static constexpr auto at(VirtAddr base) -> Self {
        return Self{base};
    }
    constexpr auto is_fixed() const -> bool {
        return m_base.is_some();
    }
    constexpr auto base() const -> VirtAddr {
        return m_base.value();
    }
    ...
    ustl::Option<VirtAddr> m_base;
```

**Reference**
None, this project's own rule. Zircon's `VmAddressRegion::CreateSubVmar()` takes the address and a `VMAR_FLAG_SPECIFIC` option side by side, the shape `VmPlacement` replaced.

**Verification**
- The build of `out/bringup.x86` is clean.
- `scripts/format_cpp.py` on every changed file; `clang-tidy` on `pm_zone.cpp` and `vm_area.cpp` reports nothing on the changed lines (pre-existing `modernize-use-nodiscard` in `frame_buddy.hpp` and a `lists_` naming warning in `pm_zone.hpp`).
- Host test `frame-buddy-smoke`: OK.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present).
- `kshell_run.py vmaseal.test usercopy.test` on dbg: `vmaseal.test: ok -- 13 checks`, `usercopy.test: ok -- 18 checks`; both place mappings with `VmPlacement::at()`.

**Deliberately not done**
- `ensure_fset_attached()` runs on the allocation and free paths without the zone lock, so two CPUs reaching a zone's first use at once would both attach. Both write the same three values, and the first use of every zone with free frames comes during boot on one CPU, so it is latent. The derived check is not quite the same race as the flag: `attach()` writes `m_frames` first of its three fields, where the flag was written after `attach()` returned, so a second CPU could see "attached" before `m_count` and `m_base_pfn` are written. Equivalent single-threaded, which is the only way the attach is correct anyway. Making the attach happen once, at zone setup, is a change to the PMM's initialisation order, not to a member's type: `docs/repairs/261007/mm.md` item 1.
- `Aggregate` keeps its `bool`, for the reason above.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What made `m_fset_attached` a second answer rather than a fact of its own, and what would have gone wrong had someone added a detach() to the buddy?
- What did a release build return from `VmPlacement::anywhere().base()` before this change, and what does it do now?
- Why does the aggregate's `m_empty` stay, although a span with start above last could stand for it?
