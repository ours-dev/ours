# riscv-paging changes

## Title: riscv's arch headers are listed at the paths they are at, so `gn check` reads them (2026-10-28)

**Files**
- `kernel/arch/riscv/BUILD.gn` — `//kernel/arch/riscv:headers` lists its headers under `headers` instead of `public`

**Symptom**
`gn check out/bringup.riscv "//kernel/arch/riscv/*"` stopped with `Source file not found` for `//kernel/arch/riscv/arch/riscv/boot_marker.hpp`, and `gn desc` showed all seven public headers under `//kernel/arch/riscv/arch/riscv/`, a directory that does not exist. Nothing failed to compile: the include directory itself was right, so every `#include <arch/riscv/...>` resolved.

**Cause**
`cpp_interface()` (`build/cpp/cpp_interface.gni`) takes the header list in one of two spellings. `public` is forwarded to the `source_set` as it is, so GN reads each entry relative to the BUILD.gn file; `headers` is the spelling that gets `include_dir` put in front of each entry. riscv's list was written with `public` but in the include-relative form (`arch/riscv/sbi.hpp`), so GN looked for every header one `lib/arch/include` too high. The headers were therefore owned by no target: `gn check` cannot tell a dependent that includes one without depending on `:headers` from one that does, and an IDE indexing the target finds none of them. x86 made the same mistake once and fixed it the same way (its comment in `kernel/arch/x86/BUILD.gn`).

**Change**
Before:

```text
cpp_interface("headers") {
  include_dir = "lib/arch/include"
  public = [
    "arch/riscv/boot_marker.hpp",
```

After:

```text
cpp_interface("headers") {
  include_dir = "lib/arch/include"
  headers = [
    "arch/riscv/boot_marker.hpp",
```

with a comment saying why. Landed before the Sv39 headers join the list, so that they are listed where they are from the start.

**Reference**
None, this project's own build template; x86's `//kernel/arch/x86:headers` is the precedent.

**Verification**
- `gn check out/bringup.riscv "//kernel/arch/riscv/*"`: `Header dependency check OK`; `gn desc ... public` lists `//kernel/arch/riscv/lib/arch/include/arch/riscv/boot_marker.hpp` and the rest.
- riscv `bootshim.bin` still links.
- `ours-verify --out out/x86-paging`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (150 passed); x86 does not read this file.

**Deliberately not done**
None.

**How it was found**
Adding the Sv39 headers to this list meant reading `cpp_interface()` to see which spelling the entries needed; `gn check` and `gn desc` then confirmed the existing entries were already wrong.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did every `#include <arch/riscv/...>` still compile while GN could not find a single one of those headers?
- What does `gn check` lose when a header is listed under no target?
- Why does a header-only library have a `source_set` at all?

## Title: Sv39 page tables for the generic paging engine, checked bit for bit on the host (2026-10-28)

**Files**
- `kernel/arch/riscv/lib/arch/include/arch/paging/arch_mmu_flags.hpp` — new: `RiscvMmuFlags`, the eight flag bits of an entry, and `MmuFlagsConv<RiscvMmuFlags>`
- `kernel/arch/riscv/lib/arch/include/arch/paging/riscv_pagings.hpp` — new: `RiscvPagingLevel`, `RiscvSatpMode`, the entry `RiscvPte<Level>`, the scheme `RiscvPaging<TopLevel, Mode>` with `satp()`, and `PagingDispatcher<RiscvPagingLevel::Level2>` for Sv39
- `kernel/arch/riscv/BUILD.gn` — both headers join `//kernel/arch/riscv:headers`
- `kernel/lib/arch/test/riscv_paging_smoke.cpp` — new host smoke test
- `kernel/lib/arch/BUILD.gn` — `riscv-paging-smoke`
- `BUILD.gn` — the test joins `//:default` on the host toolchain, so `ours-verify` runs it

**Symptom**
None, new feature. physload's `Aspace` reaches page tables through `ours/phys/arch_paging.hpp`, which names `ArchLowerPaging` and `ArchUpperPaging`; riscv had no paging type for that header to name, so nothing in phys that maps memory could compile for riscv.

**Cause**
The paging engine (`kernel/lib/arch/include/arch/paging/mod.hpp`) is generic over a paging scheme, but its only scheme was x86's, and three things x86's entry format gets for free are spelled differently on riscv. Each has to be expressed in the riscv entry type, or the engine walks the wrong way:

- What makes an entry a table. On x86 a present entry above the final level is a table unless its PageSize bit says leaf, and a table entry may carry Writable and User (x86 ANDs permissions down the levels, so a table grants everything and the leaf decides). On riscv the R, W and X bits are the leaf bit: a valid entry with R = W = X = 0 is a pointer to the next table, and with any of them it is a leaf, at any level. So a riscv table entry must be V and the PPN alone; the specification also reserves U, A and D in it. Copying x86's "table grants everything" would make every table entry a 1 GiB or 2 MiB leaf over the table's own frame. The smoke test shows it: with that one change, the second map() of the test fails with `Existent`, because the walk now reads the root entry it wrote a moment before as a gigapage.
- Permissions. x86 has a no-execute bit and no read bit: every present page is readable, so the neutral `MmuFlags::Readable` is implied there and callers often leave it out. riscv has three positive bits, and two of their combinations are not a page at all: W without R is reserved (the hart faults on it), and none of the three is the pointer encoding above. The engine cannot refuse flags (`MmuFlagsConv::to_arch()` and `Pte::make()` cannot fail), so the entry settles it: `leaf_access()` adds R in both cases. A request for "no access" written as asked would have made a data frame into a page table for the hardware walker. Execute-only, which riscv can encode, stays execute-only.
- Accessed and Dirty. x86 sets them in hardware. The riscv specification allows a hart either to update them or to raise a page fault when it finds them clear, and nothing in phys or the kernel handles such a fault, so a leaf is written with A set, and D when writable.

The rest is geometry: three levels of 512 entries (VPN[2..0], bits 38..12), 4 KiB pages with 2 MiB and 1 GiB leaves, the PPN in bits 53..10 counting 4 KiB frames, and addresses sign-extended from bit 38 (`VirtAddrExt::Canonical`), so one root covers both halves exactly as x86's does and `Aspace` stays single-rooted (`DUAL_ASPACE` false).

**Change**
The engine is unchanged; everything is in the two riscv headers. The entry, x86 for comparison:

```cpp
// x86: table entries grant everything; PageSize marks a large leaf.
if (!terminal) {
    constexpr auto TABLE_FLAGS =
        PteVal(X86_MMUF_PRESENT | X86_MMUF_ACCESSED | X86_MMUF_WRITABLE | X86_MMUF_USER);
    return {frame | TABLE_FLAGS};
}
```

riscv:

```cpp
auto const frame = ((addr >> FRAME_SHIFT) << PPN_SHIFT) & PPN_MASK;
if (!terminal) {
    return {frame | PteVal(RiscvMmuFlags::Valid)};
}
auto value =
    frame | leaf_access(flags) | PteVal(RiscvMmuFlags::Valid | RiscvMmuFlags::Accessed);
if ((value & PteVal(RiscvMmuFlags::Writable)) != 0) {
    value |= PteVal(RiscvMmuFlags::Dirty);
}
if (!(flags & RiscvMmuFlags::User)) {
    value |= PteVal(RiscvMmuFlags::Global);
}
```

and a leaf is read from the permission bits, not from a size bit:

```cpp
constexpr auto is_terminal() const -> bool {
    return is_present() && (inner & PERMISSIONS) != 0;   // R | W | X
}
```

Choices made along the way:

- Global goes on every leaf without User, the rule x86's entry already uses ("not User" is "kernel"); table entries get none, since G on a table entry would declare everything below it global.
- `is_terminal()` counts W as a leaf bit too, so the reserved W-alone encoding is never walked through as a pointer. make() never writes it, so no test can tell this from "R or X".
- `MAX_PHYS_ADDR_SIZE` is 56: 44 bits of PPN over 4 KiB frames, in every mode. map() refuses a frame at 2^56 with `InvArgs` rather than writing a PPN that lost its top bit.
- `MmuFlags::Discache` has no riscv bit and is dropped: without the Svpbmt extension a page cannot choose its memory type, the platform's physical memory attributes fix it per address range.
- `RiscvPaging` has two template parameters like `X86Paging`, the top level and the satp MODE, and no base struct: with one scheme there is nothing to share. The dispatcher is keyed by the top level, as x86's is: `PagingDispatcher<RiscvPagingLevel::Level2>::Type` is Sv39.
- `satp(root, asid)` composes MODE 8 << 60 | ASID << 44 | root PPN, for the phys layer to write.
- The entry's raw word is `inner`, not x86's `inner_`: the checked-in clang-tidy naming rule rejects the trailing underscore on a public member, and nothing generic reads the field.

**Reference**
- RISC-V privileged specification, "Sv39: Page-Based 39-bit Virtual-Memory System" (the PTE layout, the reserved W-alone encoding, superpage alignment), "Virtual Address Translation Process" (R = W = X = 0 is a pointer; the A/D fault-or-update choice), and the `satp` register.
- Zircon `arch::RiscvPagingTraitsBase::TableEntry::Set()` in `zircon/kernel/lib/arch/include/lib/arch/riscv64/paging-traits.h`: table entries with R = W = X = U = 0, leaves with A set and D = writable, `terminal()` as R or X, and `RiscvSatp` (MODE 63:60, ASID 59:44, PPN 43:0) with `kSv39 = 8`. Zircon sets `kMaxPhysicalAddressSize = 57` there; this tree uses the specification's 56.
- Zircon `mmu_flags_to_pte_attr()` and `Riscv64ArchVmAspace::Map()` in `zircon/kernel/arch/riscv64/mmu.cc`: Zircon refuses a mapping without `ARCH_MMU_FLAG_PERM_READ` with `ZX_ERR_INVALID_ARGS` in the address-space layer, where this tree's engine has no way to refuse; the entry's `leaf_access()` is the substitute, and a phys or kernel layer above may still refuse first.
- x86's `X86Pte::make()` in `kernel/arch/x86/lib/arch/include/arch/paging/x86_pagings.hpp`, which the riscv header follows in shape.

**Verification**
- `riscv-paging-smoke` (host): `riscv-paging-smoke: 8 checks passed`. It maps a 4 KiB, a 2 MiB and a 1 GiB leaf into one root whose tables live in host memory under made-up physical addresses (0x8010'0000 + i * 4 KiB), so every table entry has a value worked out by hand in the test's comments; it checks the five entries written and that nothing else was, queries inside each leaf, unmaps each, and checks the W-alone, no-permission and execute-only encodings, protect(), the 56-bit physical limit, and satp.
- Each check bites: a script broke the traits one way at a time, rebuilt, ran and restored. All 19 mutations failed the test; the restored tree passed.

| Mutation | Checks that failed |
|---|---|
| PPN at bit 12 instead of 10 | encodings, unmaps, leaf access, protect, physical address width |
| PPN 43 bits wide | physical address width |
| `address()` returns the PPN unshifted | encodings, queries, unmaps, leaf access, protect, physical address width |
| leaf without A | encodings, leaf access, protect, physical address width |
| D on every leaf | encodings, leaf access |
| G on every leaf | encodings |
| table entry with A | encodings, unmaps |
| table entry granting R, W, U, as x86's does | encodings, queries, unmaps, leaf access, protect, physical address width |
| a leaf is R only (execute-only taken for a table) | leaf access |
| 10 index bits per level | sv39 shape, encodings, queries, unmaps |
| no leaf above level 0 | encodings, queries, unmaps |
| W alone not given R | leaf access |
| no permission not given R | leaf access, protect |
| `with_protection()` clears D | protect |
| 57-bit physical addresses | sv39 shape, physical address width |
| satp MODE 9 for Sv39 | sv39 shape, satp |
| satp ASID at bit 48 | satp |
| `to_arch()` drops User | encodings, queries |
| `from_arch()` drops Readable | queries, leaf access, protect, physical address width |

- The engine instantiated over Sv39 (map, query, unmap, satp) compiles with the riscv phys toolchain's own command line (`--target=riscv64-unknown-none-elf`, freestanding), taken from the boot shim's compile and pointed at a scratch source.
- clang-tidy (`-p out/bringup.riscv`, header filter on the two headers): clean. `scripts/format_cpp.py --check`: clean. `gn check` on `//kernel/lib/arch/*` and `//kernel/arch/riscv/*`: OK.
- `ours-verify --out out/x86-paging`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed, `x86-paging-smoke` and `riscv-paging-smoke` among them).

**Deliberately not done**
- `ours/phys/arch_paging.hpp` for riscv and the rest of the phys arch layer: the next step's, against the type published here.
- Sv48 and Sv57 dispatchers: Sv39 was decided, and each is one more specialization when wanted.
- Svpbmt memory types and Svnapot: QEMU's `virt` needs neither for phys.
- A test of splitting a large leaf with deposits on riscv: the split is engine code, and the entries it writes go through the same `make()` the test checks.
- `sfence.vma` after a change, and ASID allocation: the caller's, as TLB invalidation is on x86.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why would an x86-style table entry that grants Writable and User turn every riscv table into a large page, and what would the hardware then do on an access through it?
- `leaf_access()` adds R to a request for W alone and to a request for nothing. What would each of those entries have meant to the hardware if written as asked?
- Why does a riscv leaf need A set, and D when writable, while x86's could leave both to the CPU?
