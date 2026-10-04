# mm changes

## Title: Frame flag setters stop erasing each other, and Reserved stays a flag beside the role (flag-consolidation step 2) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `set_flags()` and `mark_pinned()` go, along with `Flags::Pinned`; `mark_reserved()` ORs its bit in and asserts it was clear; Reserved and Initialized are renumbered to bits 0 and 1, which shrinks the Flags field of the Status word from 10 bits to 2; Role and Flags document which is a use and which an attribute; `become()`'s doc no longer claims to be the role's only writer
- `kernel/mm/memmap_init.cpp` — `stamp_reserved_frames()` skips a frame already Reserved, out loud; page tables are no longer marked Pinned
- `kernel/mm/init_vmm.cpp` — the zero page is no longer marked Pinned
- `kernel/mm/include/ours/mm/pmm.hpp` — `unwire_frame()`'s doc no longer leans on the bug
- `docs/repairs/261004/x86-mm.md` — new: physboot's identity-map page tables are unhooked and never freed, found by this change's review
- `docs/repairs/261003/mm.md` — deleted: item 1 was its only item
- `.agents/tasks/flag-consolidation/DESIGN.md` — D1 decided

**Symptom**
Latent. Measured on both images before the change: all 12 boot page-table frames ended with Role::Mmu and only `Pinned` set, without `Reserved`. Every reserved frame ended without `Initialized`, and the zero page lost `Initialized` too. The guard that no Reserved frame reaches the buddy (`free_frames_phys_range()`) could not see a page table.

**Cause**
The objects:
- A frame descriptor's Flags field: three flags (Pinned = bit 1, Reserved = bit 8, Initialized = bit 9) in a 10-bit field of the Status word.
- `set_flags()`: `m_status.set<Index::Flags>(flags)`, which writes the whole field. Every `mark_*()` went through it.
- `stamp_reserved_frames()`: for each reservation's frames, skip a descriptor without Initialized, mark it Reserved, give a page table Role::Mmu, and add one to the node's and zone's reserved count.

The events at boot:
1. `init()` sets Initialized on every usable frame. A hole gets `init()` and then `mark_reserved()`, so its flags become Reserved alone: Initialized is gone.
2. The stamp marks a frame Reserved, which also erases Initialized. A page table is then marked Pinned, which erases Reserved.
3. If a second reservation covered the same frame, or one covered a hole, the stamp would find no Initialized and skip it.

So the overwrite was doing two jobs nobody had written down. It lost Reserved on page tables, which was the bug. It also de-duplicated the stamp: a frame could not be counted twice, because the first mark made the second visit skip it. The repair item proposed an OR and said the counts "may grow, which is the fix showing". That was wrong: an OR alone would count holes and shared frames, which is a new bug. The counts must stay the same, and the de-duplication must become a stated test.

The temporary counters (2026-10-03) found 636 hole frames on each image. No reservation covered a hole, and none shared a frame with another. So the de-duplication never fired on the QEMU images. It is still needed, because a firmware memory map can produce either case.

**Change**
Before:

```cpp
constexpr auto set_flags(Flags flags) -> void { m_status.set<Index::Flags>(flags); }
constexpr auto mark_pinned() -> void { set_flags(Flags::Pinned); }
constexpr auto mark_reserved() -> void { set_flags(Flags::Reserved); }
...
frame->mark_reserved();
if (role != page::Role::None) {
    frame->set_role(role);
    frame->mark_pinned();
}
```

After:

```cpp
auto mark_reserved() -> void {
    DEBUG_ASSERT(!(flags() & Flags::Reserved), "frame reserved twice");
    m_status.set<Index::Flags>(flags() | Flags::Reserved);
}
...
if (!!(frame->flags() & page::Flags::Reserved)) {
    continue; // shared with an earlier reservation, or a hole
}
frame->mark_reserved();
if (role != page::Role::None) {
    frame->set_role(role);
}
```

`init()` is now the only whole-field write. Pinned goes rather than being fixed: nothing read it, and in Zircon a pin is a count on the page, not a frame flag. Its two writers were page tables and the zero page. Page tables are covered by Role::Mmu. The zero page is covered by nothing, and needs nothing today: nothing here reclaims, migrates or frees a VMM page on its own. Zircon marks its zero page WIRED (`zircon/kernel/vm/vm.cc`, `vm_init_preheap()`); the equivalent here would be Reserved, not done, because Reserved also means being on the boot-time reserved books.

Decision D1 of `.agents/tasks/flag-consolidation/DESIGN.md` chose between this and making Reserved a role, `Role::Wired`, as in Zircon's one-state-per-page model. The user chose the flag: Role is what a frame is used for, and "held since boot" is not a use but a fact about a frame whatever its use. So a boot page table is Mmu and Reserved at once, and the kernel image is None and Reserved. The Role and Flags docs now say so.

**Reference**
- Linux keeps the same split: `page_type` is the use, and `PG_reserved` is a separate bit set with `SetPageReserved()`, a `set_bit()` (`include/linux/page-flags.h`). `free_pagetable()` in `arch/x86/mm/init_64.c` tests `PageReserved` on a table.
- Zircon instead gives a page one `vm_page_state`. Physboot's page tables become `MMU` at hand-off (`X86ArchVmAspace::HandoffPageTablesFromPhysboot()`), and the other reservations become `WIRED` (`PmmNode::InitReservedRange()`). That is option B, not taken.

**Verification**
- `boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed).
- The six per-type `Reserved N frames (K KiB) of <type>` lines are identical to the baseline taken at `38808623`, on both images.
- In a scratch copy:
  - all 12 page-table frames are Mmu and Reserved;
  - a second call to `stamp_reserved_frames()` logs no counts, so every frame was skipped;
  - with the skip removed and the stamp run twice, dbg stops at "Assertion failed on !(flags() & Flags::Reserved) with message: frame reserved twice", and rel, with no assertions, logs every count twice.

**Deliberately not done**
- `Role::Wired` (option B), for the reason above.
- Retiring Initialized. It is the only way to tell a descriptor nothing initialised (Role::None is zero), as the audit found.
- The page::Status word race (the buddy writes Role and Order under the zone lock, owners write Role and Flags with none). Recorded as an open question in the DESIGN, the same under either option.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`) found that the repair item's own Change and Acceptance were wrong. Temporary counters in `stamp_reserved_frames()`, which classified each visited frame (first visit, hole, visited before, page table visited before, never initialised), measured how often each case occurs. A second check of the D1 arguments against the Linux and Zircon sources found that the premises first given for A did not hold: the managed count is a free count, nothing frees a boot page table, and Zircon does count per state. A now rests on the user's reason instead.

**The user's prediction**
"A", then the reason: Role is a frame's actual use, not an attribute of a kind of page. The analysis first given for A rested on three premises that a second check refuted. The user's reason is different and holds: it is the line Linux draws, and `role_of()` in `memmap_init.cpp` already described None-and-Reserved frames that way. A's one concrete advantage also matches the hint given: `become()` rewrites only the role, so a Reserved flag survives a missed `unwire_frame()`, and the free path's assert catches it.

**Three questions you should be able to answer**
- Which two jobs did the whole-field `set_flags()` do, and why did fixing it with a bare OR need a third change?
- Why is a boot page table both Mmu and Reserved, and what would option B have made of it?
- Why does `mark_reserved()` assert the bit was clear, and what did the scratch run without the skip show in dbg and in rel?

## Title: page::Role keeps only the roles something writes, and its to_string() is exhaustive (flag-consolidation step 3) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — Io, Lru, Heap and Folio go; the enum's doc says why, and None is documented as Zircon's ALLOC; the Role field of the Status word shrinks from 4 bits to 3
- `kernel/mm/pm_frame.cpp` — `to_string(Role)` loses the three dead cases and its default branch, and gains an Mmu case

**Symptom**
`to_string(Role::Mmu)` printed "Anonymous", so a dumped boot page table looked like a frame with no role at all. Nothing else is observable: no code ever set the four removed roles.

**Cause**
The objects:
- `page::Role`: which payload of a frame descriptor is live, one value at a time.
- `to_string(Role)`: a switch with a `default:` that returned "Anonymous".

The four roles were names, not uses. Io had no stated meaning. Lru and Folio are Linux ideas that are not exclusive uses: in Linux, LRU membership is the PG_lru flag and a compound head is PG_head, both beside the page's type. Here, LRU membership is the page queue a Vmm page sits on (`VmmBody::queue`), and compound pages are the `m_compound` stamp, which a Slab of order above 0 carries at the same time as Role::Slab. Heap was the slot the vmalloc design considered and rejected: kmalloc pages are Slab and vmalloc pages are Vmm.

The `default:` branch is why Mmu printed wrong. When Mmu was added, the compiler could not say the switch missed it, because a default makes every switch complete. Without a default, `-Wswitch` names every unhandled enumerator, so the next role added cannot be forgotten here.

**Change**
Before:

```cpp
enum class Role { None, Io, Lru, Vmm, Pmm, Slab, Heap, Folio, Mmu, Count };
...
        case PageFrame::Role::Heap:
            return "Heap";

        default:
            return "Anonymous";
    }
}
```

After:

```cpp
enum class Role { None, Vmm, Pmm, Slab, Mmu, Count };
...
        case PageFrame::Role::Mmu:
            return "MMU";
        case PageFrame::Role::Count:
            break;
    }
    return "Invalid";
}
```

The `Count` case and the return after the switch follow `bootmem::to_string(MemoryType)`. The values move (Vmm 3 to 1 and so on); nothing stores a role outside the Status word, so nothing depends on them.

**Reference**
- Zircon `vm_page_state` (`zircon/kernel/vm/include/vm/page.h`): FREE, ALLOC, OBJECT, WIRED, HEAP, MMU, SLAB, IOMMU, IPC, CACHE. ALLOC is our None, OBJECT our Vmm, SLAB our Slab, MMU our Mmu. Zircon's HEAP is cmpctmalloc's pages, which this kernel does not have.
- Linux `page_type` (`include/linux/page-flags.h`, PGTY_buddy, PGTY_slab, PGTY_table and so on) is exclusive; PG_lru and PG_head are flags beside it.

**Verification**
- `boot_check.py --out out/flag-consolidation`: dbg PASS (24 passed), rel PASS (21 passed).
- clang-tidy on `pm_frame.cpp`: nothing on the changed lines. clang-format: clean.
- `git grep` for `Role::Io`, `Role::Lru`, `Role::Heap` and `Role::Folio` under `kernel/` is empty.

**Deliberately not done**
- Renaming Pmm to Free, Zircon's name for the same state. It is decision D11, step 5.
- Giving runtime page tables Role::Mmu. It is step 4.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the page::Role rows): `git grep` found no writer for the four roles, and found that `to_string()` had no Mmu case.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a frame not have a "Folio" role here, when it can have a Slab role?
- What did the `default:` branch cost when Mmu was added, and what replaces it?
- Which Zircon page state is our Role::None, and which frames carry it?
