# object changes

## Title: A snapshot child gets Write and loses Execute, as Zircon's does (2026-10-02)

**Files**
- `kernel/lib/syscall/src/vmo.cpp` — `vmo_create_child()`: for `SnapshotFull` and `SnapshotOnWrite`, the child's rights are the parent handle's with `Execute` removed and `Write` added; a slice's stay the parent handle's, unchanged. The doc comment says why for each.
- `examples/shell/shell.cpp` — `check_vmo_create_child()`: a snapshot of a handle without `Write` can be written, and the source keeps its bytes. `check_vma_map()`: a snapshot of an executable VMO cannot be mapped RX, can be mapped RW, and a store through that mapping is not seen through the executable VMO's own RX mapping.

**Symptom**
A snapshot took the parent handle's rights unchanged. A snapshot of a read-only VMO, which is what a program's data segment is cloned from, could not be written, so it was of no use; and a snapshot of an executable VMO kept `Execute`, so a program could write into it and then run it without going through `vmo_replace_as_executable()`.

**Cause**
10a accepted the snapshot options and left the rights as the slice had them. For a slice that is right: a slice writes the parent's own pages, so it must not be able to write more than the parent handle could. A snapshot is different on both counts. A write to it can only ever reach its own copy of a page, never the parent's, so `Write` takes nothing from the parent and is what the snapshot is made for. And being writable, it must not also be executable by default: writable-then-executable code is exactly what `vmo_replace_as_executable()` and the `vmex` resource exist to gate.

**Change**
```cpp
if (options == VmoChildOptions::SnapshotFull || options == VmoChildOptions::SnapshotOnWrite) {
    rights &= ~Rights::Execute;
    rights |= Rights::Write;
}
```

**Reference**
Zircon `sys_vmo_create_child()` (`zircon/kernel/lib/syscalls/vmo.rs:325`): the child's rights are the parent's minus `RESIZE` (plus `GET_PROPERTY`/`SET_PROPERTY`); with `ZX_VMO_CHILD_NO_WRITE` it removes `WRITE`, otherwise, for every snapshot kind, it removes `EXECUTE` and adds `WRITE`. `NoWrite` is not accepted here yet, so only the second branch exists; the property rights are left out as before (design §10).

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed, 0 skipped), rel PASS (13 passed, 10 skipped; the executable-VMO check is skipped there, as rel has no `vmex`), and the shell's `vmo_create_child` and `vma_map` checks passed in both.
- Mutation: the two lines removed. dbg FAIL with `vmo_write(a snapshot of the read-only copy) = AccessDenied`, `vma_map(RX, its snapshot) = Ok` and `vma_map(RW, its snapshot) = AccessDenied`; rel FAIL on the first. Restored.

**Deliberately not done**
- `NoWrite` on a snapshot: still `NotSupported`; nothing here asks for a read-only snapshot.
- A snapshot of the BootFS. It was the first test written and it answers `BadState`: the BootFS VMO is made by `VmoDispatcher::create_paged()`, which makes it Pinned, and `VmObjectPaged::create_clone()` refuses a Pinned VMO (a pinned page must not move into a hidden node or be forked away from its owner; Zircon refuses a clone of a VMO with pinned pages the same way). The checks were moved onto `vmo_create` VMOs, which are lazy. Whether the BootFS has to be Pinned is a decision for the launcher step, which needs to snapshot it.

**How it was found**
Planned as part of step 10; the user approved Zircon's rule on 2026-10-01. The BootFS refusal was found by the first version of this commit's test.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a snapshot be given `Write` that the parent handle lacks, when a slice cannot?
- What would a program be able to do if a snapshot of an executable VMO kept `Execute`?
- Why does a clone refuse a Pinned VMO?

## Title: The BootFS is committed, not pinned, so a file in it can be snapshotted (2026-10-02)

**Files**
- `kernel/object/include/ours/object/vmo_dispatcher.hpp`, `kernel/object/vmo_dispatcher.cpp` — `VmoDispatcher::create_committed()`: anonymous memory with every page committed and zeroed when it is made, neither `Lazy` nor `Pinned`.
- `kernel/lib/userabi/src/user_bootfs.cpp` — `make_user_bootfs()` makes the BootFS VMO with it instead of `create_paged()`.
- `examples/shell/shell.cpp` — `check_vmo_create_child()`: a `SnapshotFull` of the `vdso` entry of the BootFS can be written, and the BootFS still reads the ELF magic.

**Symptom**
`vmo_create_child(bootfs, SnapshotFull, ...)` answered `BadState`, so a program's data segment could not be made as a snapshot of its file, which is what the launcher step needs.

**Cause**
The BootFS VMO was made by `create_paged()`, which makes it `Pinned`. `VmObjectPaged::create_clone()` refuses a Pinned VMO, rightly: a clone moves the original's pages into a hidden node, and a later write by the original forks its page, so the frame under a pinned offset would change. Zircon refuses the same clone (`ZX_ERR_BAD_STATE` for a VMO with pinned pages). The BootFS never needed the pin: it is data the kernel writes before user space starts, and nothing reaches it by physical address. Only the commit was wanted, so that the gaps between files read as zero; `create_paged()` gave both because it was the only factory that committed.

**Change**
```cpp
// Neither Lazy nor Pinned: VmObjectPaged::create() commits every page of a
// VMO that is not Lazy, and pins them only for Pinned.
mm::VmObjectPaged::create(mm::GAF_USER, size, mm::VmoFlags{}, &vmo);
```

**Reference**
Zircon's BootFS VMO is an ordinary paged VMO, never pinned: userboot decompresses the ZBI's BootFS into a VMO it makes with `zx_vmo_create`, and writes every page of it.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped; rel has no BootFS, so the new check runs in dbg only).
- Mutation: the BootFS made by `create_paged()` again. dbg FAIL with `vmo_create_child(bootfs, SnapshotFull, vdso) = BadState (expected Ok)`. Restored.

**Deliberately not done**
- `create_paged()` itself, still Pinned for userboot's stack and the ELF images kelfload loads. Its comment says the pin is there because nothing could service a fault yet, which is no longer true for lazy VMOs; whether those two still need it is its own question.

**How it was found**
The first version of the previous change's test snapshotted the BootFS and was answered `BadState`. The user chose on 2026-10-02 to make the BootFS unpinned rather than allow a clone of a pinned VMO.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What would go wrong if a clone of a pinned VMO were allowed?
- What did the BootFS need from `create_paged()`, and what did it get that it did not need?
- Why does the new check run only in the dbg image?

## Title: SnapshotModified is accepted, as a full snapshot, and the snapshot kinds are listed once (2026-10-02)

**Files**
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `VmoDispatcher::is_snapshot()`: the one list of the snapshot kinds, now with `SnapshotModified`.
- `kernel/object/vmo_dispatcher.cpp` — `create_child()` makes a clone for whatever `is_snapshot()` names; its comment says what each kind asks and why one clone answers all three.
- `kernel/lib/syscall/src/vmo.cpp` — `vmo_create_child()` gives Write and takes Execute away for whatever `is_snapshot()` names.
- `system/public/ours/syscall.hpp` — the comments: `SnapshotModified` is built.
- `examples/shell/shell.cpp` — a `SnapshotModified` of a handle without Write can be written, and does not see the source's later writes; the `NotSupported` example is now `Reference`.

**Symptom**
`vmo_create_child(SnapshotModified)` answered `NotSupported`, where Zircon answers the same request on an anonymous VMO with a full snapshot.

**Cause**
`SnapshotModified` was deferred as a pager feature. Its contract (`zircon/vdso/vmo.fidl`) is that every page of the parent not backed by a pager behaves as if copied eagerly, and pager-backed pages at least copy-on-write. With no pager, no page is pager-backed, so the contract is exactly a full snapshot; it is not an upgrade, as `SnapshotOnWrite` answered with a full snapshot is. Zircon does the same: `VmCowPages::CreateCloneLocked()` takes the unidirectional clone only when `can_unidirectional_clone_locked()`, which requires a pager-backed root, and otherwise the bidirectional one.

The kinds were also listed twice, in `create_child()` and in `vmo_create_child()`'s rights; a kind added to one and not the other would make a clone with the parent's rights, or rights for a child that was never a snapshot. They are listed once now, in `is_snapshot()`.

**Change**
```cpp
static constexpr auto is_snapshot(VmoChildOptions const options) -> bool {
    return options == VmoChildOptions::SnapshotFull ||
           options == VmoChildOptions::SnapshotOnWrite ||
           options == VmoChildOptions::SnapshotModified;
}
```

**Reference**
Zircon `ZX_VMO_CHILD_SNAPSHOT_MODIFIED` (`zircon/vdso/vmo.fidl:857`); `VmCowPages::CreateCloneLocked()` (`zircon/kernel/vm/vm_cow_pages.cc`) and `can_unidirectional_clone_locked()` (`zircon/kernel/vm/include/vm/vm_cow_pages.h:1074`); RFC-0236, which says a `SNAPSHOT_MODIFIED` clone of an anonymous VMO is "upgraded to have snapshot semantics".

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped), the shell's `vmo_create_child` checks passed in both.
- Mutation: `SnapshotModified` dropped from `is_snapshot()`. Both images FAIL with `vmo_create_child(the read-only copy, SnapshotModified) = NotSupported (expected Ok)`. Restored.

**Deliberately not done**
- The pager semantics of `SnapshotModified`: there is no pager.

**How it was found**
The user's questions about the three snapshot kinds; checked against the Fuchsia source on the user's request, and changed on the user's decision on 2026-10-02.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which pages does `SnapshotModified` freeze, and which does it leave following the parent?
- Why is `SnapshotModified` on an anonymous VMO a full snapshot by its own definition, while `SnapshotOnWrite` on one is a full snapshot only because the cheaper clone is not available?
- What could go wrong if the snapshot kinds were listed in two places?

## Title: vma_allocate places a sub-region where the kernel finds room, and makes one that can be mapped into (2026-10-02)

**Files**
- `kernel/object/vma_dispatcher.cpp` — `VmaDispatcher::allocate()`: Specific is optional (without it the kernel places the sub-region and the offset must be 0), and CanMapRead, CanMapWrite and CanMapExecute become the sub-region's allowance, never wider than this region's (AccessDenied). A sub-region with none of them is still a guard.
- `kernel/object/include/ours/object/vma_dispatcher.hpp`, `kernel/lib/syscall/src/vma.cpp` — the comments.
- `system/public/ours/syscalls.gni`, `system/lib/ou/include/ou/vma.hpp` — the call's documentation.
- `examples/shell/shell.cpp` — `check_vma_allocate()`: the two checks that expected NotSupported now expect the new behaviour, and nine new ones.

**Symptom**
The K23 launcher had no way to reserve a program's span: `vma_allocate` took only Specific with no CanMap bit, a guard that nothing can be mapped into. Without a sub-region to map into, the launcher would have had to map each segment straight into the root region at fixed offsets it chose itself.

**Cause**
K22 built only the shape it needed then, a stack guard. Zircon's `zx_vmar_allocate` does two more things, and a loader uses both. Objects: the root region, the whole user address space; and the image's sub-region, as long as the program's segments together. Events:
1. The launcher allocates the sub-region without Specific, so the kernel picks where it goes (which is also where ASLR will randomise, later, without touching the launcher), with CanMapRead, CanMapWrite and CanMapExecute.
2. It maps every segment into the sub-region with Specific, at the segment's offset from the program's first address.
The segments keep their places relative to each other, which the program's code assumes, while the span as a whole lands wherever the kernel put it. Mapping each segment straight into the root region at fixed offsets would have routed around the missing interface with an existing one, which the design rules call a stand-in (ours-kernel-design, layering.md "Sequencing a feature"); the user agreed to extend `vma_allocate` first.

Two rules come with it, both Zircon's. A sub-region may narrow its parent's allowance and never widen it; the system call already asks the parent handle for Read, Write or Execute for each CanMap bit, and the dispatcher checks the region itself as well, for a caller that has no handle to ask. And an offset without Specific is InvArgs: it would be a request nothing reads.

**Change**
```cpp
auto where = mm::VmPlacement::anywhere();
if (!!(options & VmOptions::Specific)) {
    // offset inside this region, else InvArgs
    where = mm::VmPlacement::at(m_vma->base() + offset);
} else if (offset != 0) {
    return Status::InvArgs;
}
auto allows = mm::VmaFlags::None;   // Read/Write/Exec from CanMapRead/Write/Execute
if ((allows & RWX & ~m_vma->permissions()) != mm::VmaFlags::None) {
    return Status::AccessDenied;
}
m_vma->create_subvma(where, rounded, allows, "vma_allocate", mm::VmMapOption::None, &child);
```

**Reference**
Zircon `sys_vmar_allocate()` (`zircon/kernel/lib/syscalls/vmar.cc:30`), `VmAddressRegionDispatcher::Allocate()` and `split_syscall_flags()` (`zircon/kernel/object/vm_address_region_dispatcher.cc:46-152`), and `VmAddressRegion::CreateSubVmarInner()` (`zircon/kernel/vm/vm_address_region.cc:122`), whose `vmar_flags & ~flags_ & VMAR_CAN_RWX_FLAGS` is the AccessDenied here.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped); the shell's `vma_allocate checks passed` in both.
- The new checks: a kernel-placed sub-region with CanMapRead and CanMapWrite, page-aligned; a RW mapping at page 1 of it lands at its start plus a page and can be written and read; an executable mapping in it is AccessDenied, and so is one past its end (InvArgs); a nested sub-region asking for Execute is AccessDenied; a parent handle without Execute cannot give CanMapExecute; Specific with CanMapRead is Ok; an offset without Specific is InvArgs; CanMapSpecific is NotSupported.
- Mutation: every sub-region made a guard again; both images FAIL (`vma_map(RW, Specific, page 1 of the sub-region) = AccessDenied (expected Ok)`). Restored.

**Deliberately not done**
- CanMapSpecific: every region here already takes a Specific mapping, so the bit would mean nothing; it stays NotSupported rather than being accepted and ignored.
- Alignment bits, Compact, OffsetIsUpperLimit and SpecificOverwrite: NotSupported until a caller needs them.
- Randomised placement (ASLR): the kernel takes the lowest room that fits.

**How it was found**
Introducing K23's executor: the plan was to place the program at fixed offsets because `vma_allocate` could not place or be mapped into; the user asked why, and that plan was withdrawn for this.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a loader reserve a sub-region for a program instead of mapping its segments straight into the root region?
- Which two checks keep a sub-region from allowing more than its parent, and why are both there?
- Why must the offset be 0 when the kernel chooses the place?

## Title: vma_allocate's comments and the shell's checks of it say what it does (2026-10-02)

**Files**
- `system/public/ours/syscall.hpp` — `VmOptions`: what CanMap* means on vma_allocate; Zircon's alignment field is not named, so it is InvArgs.
- `kernel/object/include/ours/object/vma_dispatcher.hpp` — `allocate()`'s summary no longer says `[offset, offset + size)`.
- `kernel/lib/syscall/src/vma.cpp` — vma_map's doc block moved back above `vma_map()` (it sat above `vma_allocate()`); vma_allocate's summary; the Sealed exception to vma_map's undo.
- `examples/shell/shell.cpp` — `check_vma_allocate()`: a sub-region with CanMapRead alone refuses a writable mapping; the store is read back through `vmo_read`; a guard placed by the kernel holds its range; a sub-region's range stays reserved after its handle is closed; the Specific sub-region is at its offset; the address checks print when they fail.

**Symptom**
The previous entry and its checks claimed two things that were not so:
- "An executable mapping in it is AccessDenied." The VMO refused it, not the sub-region: `vmo_create` gives no Execute, so the check passed whatever the sub-region allowed. `check_vma_map()` already pins that refusal on the root.
- "Alignment bits: NotSupported." `VmOptions` does not name them, so the system call refuses them with InvArgs before anything else.

The comments were out of date too:
- `syscall.hpp` still said a program may not pass CanMap*, and that vma_allocate would take these options "later".
- Two summaries said `[offset, offset + size)`, which is wrong without Specific.
- vma_map's doc block sat above `vma_allocate()`.

**Cause**
A refusal check is evidence only if nothing but the code under test can refuse. vma_map computes `both = vma_rights & vmo_rights` and refuses PermExecute unless Execute is in both (`kernel/lib/syscall/src/vma.cpp`). That runs before the sub-region's allowance matters. With a VMO that lacks Execute, the answer is AccessDenied for every region, so the check could not tell a correct sub-region from one that allowed everything. The cure is to ask for something every other party grants and only the sub-region withholds. Write fits: the VMO's handle has it through Io, and so does the root region's.

**Change**
The executable check is replaced by one that only the sub-region can refuse:

```cpp
ok &= expect("shell: vma_allocate(CanMapRead, anywhere, 1 page) = ",
             ou::vma_allocate(vma, VmOptions::CanMapRead, 0, PAGE, &read_only, &read_only_at),
             Status::Ok);
ok &= expect("shell: vma_map(RW, into a sub-region with CanMapRead only) = ",
             ou::vma_map(read_only, VmOptions::PermRead | VmOptions::PermWrite, 0, vmo, 0, PAGE,
                         &mapped),
             Status::AccessDenied);
```

A store through the sub-region's mapping is now read back with `vmo_read`, not through the same address: a mapping of the wrong page would read back its own store just as well. The guard placed by the kernel, dropped when the rest was rewritten, is back, now expecting Ok. `root_base = at - SPOT` turns an address the kernel chose into an offset, so that a Specific mapping over the guard, and over the sub-region after its handle is closed, can be shown refused (AlreadyExists).

**Reference**
None, this project's own tests and comments.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped); `vma_allocate checks passed` in both.
- In the logs: `vma_allocate(no CanMap bit, anywhere, 1 page) = Ok`, `vma_map(Specific, over the kernel-placed guard) = AlreadyExists`, `vmo_read(what the sub-region's mapping stored) = Ok`, `vma_map(Specific, over the sub-region, handle closed) = AlreadyExists`, `vma_map(RW, into a sub-region with CanMapRead only) = AccessDenied`.
- No mutation run. Two layers refuse that writable mapping, the sub-region's handle and its allowance in mm, so breaking either alone would not flip the check. Breaking both would.

**Deliberately not done**
- An executable mapping in a sub-region with CanMapExecute needs an executable VMO (vmex). The K23 launcher will exercise it: its image region is made with CanMapExecute and maps the program's code read and execute.
- The status codes, and Zircon's CanMapSpecific rule, are the user's to decide. The status-code questions: NotFound for no room where Zircon answers NO_RESOURCES; an oversize request; NotSupported for bits Zircon refuses on allocate.
- The allowance check in `VmaDispatcher::allocate()`: `docs/repairs/261002/object.md` item 4.

**How it was found**
The adversarial review of the change above: the correctness, Zircon and tests reviewers each found the vacuous check, and the Zircon reviewer the stale comments.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did `vma_map(RX, into the sub-region) = AccessDenied` pass whatever the sub-region allowed?
- What has to be true of every other party for a refusal check to be evidence about the one it names?
- Why read a store back through the VMO rather than through the mapping that made it?

## Title: A region takes a place its user chose only with CanMapSpecific, as in Zircon (2026-10-02)

**Files**
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — `VmaFlags::CanMapSpecific`.
- `kernel/mm/vm_aspace.cpp` — every root area has it.
- `kernel/mm/vm_area.cpp` — `create_subaom_preamble()` and `reserve()` refuse a fixed placement into an area without it (AccessDenied); a mapping never takes the flag from its area.
- `kernel/mm/init_vmm.cpp` — the kernel's preset areas, which `reserve()` records mappings in, are made with it.
- `kernel/object/vma_dispatcher.cpp`, `kernel/object/include/ours/object/vma_dispatcher.hpp` — `allocate()` accepts CanMapSpecific and gives it to the sub-region; `map()` and `allocate()` leave the range of a Specific request to mm, so the flag is asked first; the rule in their comments.
- `kernel/lib/syscall/src/vma.cpp`, `system/public/ours/syscall.hpp`, `system/public/ours/syscalls.gni`, `system/lib/ou/include/ou/vma.hpp` — the rule in the ABI's words.
- `system/lib/launch/src/launch.cpp` — the image's region and the stack's region ask for it.
- `examples/shell/shell.cpp` — `check_vma_allocate()`: the regions it maps into at chosen places ask for it, and the rule is checked.
- `kernel/commands/vma_seal_test.cpp` — its sub-area asks for it.
- `docs/design/260923/userspace-interface.md` — vma_allocate's row and the P2 deferral.

**Symptom**
A request copied from Zircon failed. elfldltl's image region asks for CanMapRead, CanMapWrite, CanMapExecute and CanMapSpecific, and vma_allocate answered NotSupported to CanMapSpecific. The only spelling that worked left the bit out, which is the spelling Zircon's rule refuses. Every region here took a Specific placement whatever it was made with.

**Cause**
The objects:
- A region (VMA): a range of an address space that holds mappings and sub-regions.
- Kernel placement: no place given, the kernel chooses. Zircon randomises it.
- Specific placement: the caller gives the offset.
- CanMapSpecific: a flag on the region, set when it is made.

The events, in Zircon:
1. Every root region is made with the flag.
2. vma_allocate gives a sub-region the flag when the request asks for it, whether or not the parent has it. The sub-region itself was placed by the kernel unless the parent has the flag, so places chosen inside it fix only where things lie relative to each other, never an absolute address.
3. A Specific placement, of a mapping or a sub-region, into a region without the flag is ACCESS_DENIED.

So a region without the flag holds only what the kernel placed. With randomised placement, a program handed only such regions cannot pin anything at an address it picked. The flag is how a region keeps ASLR intact, and how a loader still gets fixed distances: a sub-region the kernel places, with the flag, and every segment at its offset inside it.

This kernel has no ASLR yet, and the kernel places at the lowest room that fits, so today the rule protects nothing. Done now because only the launcher, the shell's checks and `vmaseal.test` spelled the old way. Every caller written later would have to change once ASLR made the rule matter.

**Change**
The rule is mm's, where Zircon has it (`VmAddressRegion::CreateSubVmarInner()`), so the kernel's own callers meet it too. It sits in the fixed branch of `create_subaom_preamble()`, which every mapping and sub-area goes through, in Zircon's order: after the alignment, before the range. `reserve()`, which builds its mapping directly, asks it too.

```cpp
// new, in create_subaom_preamble()'s fixed branch
if (!(m_vmaf & VmaFlags::CanMapSpecific)) {
    return ktl::err(Status::AccessDenied);
}
```

```cpp
// before
constexpr auto ROOT_VMAF = VmaFlags::PermMask | VmaFlags::Share;
// after
constexpr auto ROOT_VMAF = VmaFlags::PermMask | VmaFlags::Share | VmaFlags::CanMapSpecific;
```

```cpp
// before, VmaDispatcher::allocate()
if ((options & ~(VmOptions::Specific | CAN_MAP_RWX)) != VmOptions::None) {
    return Status::NotSupported;
}
// after: CanMapSpecific is acted on, and becomes the sub-region's own
constexpr auto ACTED_ON = VmOptions::Specific | CAN_MAP_RWX | VmOptions::CanMapSpecific;
...
if (!!(options & VmOptions::CanMapSpecific)) {
    allows |= mm::VmaFlags::CanMapSpecific;
}
```

`allocate()` asks no right for the bit. The system call's rule that a program may not pass a CanMap bit to vma_map is unchanged.

For the order to hold through the system calls too, `map()` and `allocate()` no longer refuse an offset past the region's end themselves, before mm: they did, with InvArgs, where Zircon answers ACCESS_DENIED for a Specific request into a region without the flag however far off it points. mm's `check_range()` is in offset form, so an offset that wraps the address is still refused, with InvArgs, after the flag.

Rejected:
- Accepting the bit and enforcing nothing. Callers could copy Zircon's options, but nothing would tell one that forgot the bit, and enforcing it later would break them with no check having caught them.
- Keeping the P2 deferral. Every caller written meanwhile spells it the way the rule refuses, so the cost of adopting it grows with each one.

The kernel's fixed placements are into roots, which have the flag: vmalloc's window, init_vmm's areas, userboot's image and the vDSO through `kelfload::Loader::map()` (`kernel/lib/kelfload/src/loader.cpp:200-202`), userboot's stack, and `vmosnapshot.test`'s mappings. Two were not:
- `vmaseal.test`'s sub-area, which now asks for the flag.
- init_vmm's preset areas, in which `VmArea::reserve()` records the mappings phys made at each area's own base. A first version exempted `reserve()` instead, as the kernel describing memory already mapped. Zircon does not: its `ReserveSpace()` goes through `CreateVmMapping()` with VMAR_FLAG_SPECIFIC and meets the same check, and its handoff VMARs are made with CAN_MAP_SPECIFIC (`ToVmarFlags()`, `zircon/kernel/vm/vm.cc:65`, used at `:122`). So `reserve()` asks, and `entrol_preset_vma()` makes every preset area with the flag.

A mapping never carries the flag. `create_mapping()` used to copy its area's state bits into the mapping, so every mapping in a root came out marked CanMapSpecific. Nothing read it there, but it was a flag nobody asked for, and Zircon's `CreateVmMapping()` does not let it reach a mapping:

```cpp
// before
auto vmaf = (m_vmaf & ~VmaFlags::PermMask) | allowed;
// after
auto vmaf = (m_vmaf & ~(VmaFlags::PermMask | VmaFlags::CanMapSpecific)) | allowed;
```

**Reference**
- Zircon `VmAddressRegion::CreateSubVmarInner()` (`zircon/kernel/vm/vm_address_region.cc:166-169`): specific or upper-limit placement without VMAR_FLAG_CAN_MAP_SPECIFIC is ZX_ERR_ACCESS_DENIED.
- Zircon `VmAspace::Init()` (`zircon/kernel/vm/vm_aspace.cc:169`): the root VMAR is made with the flag.
- `zx_vmar_allocate()` (`zircon/vdso/vmar.fidl:46-48`): "It is NOT an error if the parent does not have ZX_VM_CAN_MAP_SPECIFIC permissions."
- `docs/reference/kernel_objects/vm_address_region.md:35-46`: placement is randomised by default; specific mapping is for guard pages and relative placement.
- elfldltl `VmarLoader::AllocateVmar()` (`src/lib/elfldltl/vmar-loader.cc:73`) asks for it for the image's VMAR, as the launcher now does.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); `vma_allocate checks passed` in both, `launcher checks passed` in dbg, the only image that runs the launcher.
- The new lines in both logs:
  - `vma_map(R, Specific, into a sub-region without CanMapSpecific) = AccessDenied`
  - `vma_map(R, Specific, past the end of a sub-region without it) = AccessDenied`, the flag asked before the range
  - `vma_allocate(Specific, into a sub-region without CanMapSpecific) = AccessDenied`
  - `vma_map(R, anywhere, into a sub-region without CanMapSpecific) = Ok`
  - `vma_allocate(CanMapRead | CanMapSpecific, anywhere, inside it) = Ok`
  - `vma_map(R, Specific, into its sub-region with CanMapSpecific) = Ok`
  - `vma_allocate(Specific | CanMapRead | CanMapSpecific) = Ok`
- `vma_allocate(Compact) = NotSupported` in both logs: the review found that turning the old NotSupported check into an Ok one had left that branch untested.
- Kernel commands, on both images, through `scripts/kshell_run.py` (committed next): `vmaseal.test: ok -- 13 checks`, `vmosnapshot.test: ok -- 205 checks`; on rel, `usercopy.test` and `pmnode.test` too.
- Mutation 1, mm's check removed: both images FAIL (`vma_map(R, Specific, into a sub-region without CanMapSpecific) = Ok (expected AccessDenied)`, with the checks after it). Mutation 2, the launcher's image region without the bit: dbg FAILs with `shell: launch failed: vma_map of a segment: AccessDenied 0x0` and the launcher's FAILED line. Mutation 3, init_vmm's preset areas without the flag: dbg stops at boot with `Assertion failed on Status::Ok == status`, `reserve()` having refused. rel boots on and passes, its assertions compiled out, with none of the kernel's preset areas protected. That is the case for booting both images. All restored; both images pass.

**Deliberately not done**
- SpecificOverwrite and OffsetIsUpperLimit, which in Zircon need the flag too: still NotSupported.
- ASLR: kernel placement stays lowest-first, so the flag guards nothing until then.

**How it was found**
The adversarial review of the vma_allocate extension, which found the rule inverted. It was put to the user as a decision: Zircon's rule now, the bit accepted but not enforced, or the P2 deferral kept.

**The user's prediction**
The user chose Zircon's rule now (A), before the analysis, giving no reason or expected failure. The analysis expected two failure points:
- Every chosen place inside a sub-region needs the bit. That held: the launcher's two regions, three of the shell's checks and one kernel command, and mutation 2 shows what one missed does.
- Where the check lives. Putting it in mm, as Zircon does, reached two kernel paths beyond the roots: `vmaseal.test`'s sub-area, and init_vmm's preset areas through `reserve()`. The second was first exempted, a departure from Zircon the review caught. Mutation 3 shows what missing it does.

**Three questions you should be able to answer**
- Why may a sub-region have CanMapSpecific when its parent does not, without weakening the parent's rule?
- What does the flag protect in a kernel without ASLR, and why was it done now anyway?
- Why is the check in mm rather than in the system call?
