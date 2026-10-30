# x86-kernel changes

## The AP trampoline always boots on a GDT copy inside its own buffer, so a machine with RAM above 4 GiB brings its CPUs up (2026-10-29)

**Files**
- `kernel/arch/x86/kernel/bootstrap.cpp` — `fill_bootstrap_tables()` always copies the GDT into the trampoline buffer, one structure past the data rather than `sizeof(*data)` structures past it; `make_bootstrap_area()` identity-maps the buffer and nothing else.
- `scripts/qemu_boot.py` — the generated machine's memory comment no longer cites a 4 GiB ceiling.
- `.agents/skills/ours-verify/SKILL.md` — the gotcha that RAM above 4 GiB does not boot becomes how to reach the code only such a machine runs.
- `docs/repairs/261029/x86-kernel.md` — the item is gone; a latent ordering problem in the same function, found while fixing it, is filed in its place.

**Symptom**
Every machine with RAM above 4 GiB -- on QEMU's q35 anything over `MEM=2816` -- failed on both images just before the application processors were woken:

```text
[trace] Bootstrap GDT at 0xEC40
...
[trace] Bootstrap PGD at 0x7FEDB000
[trace] Bootstrap16 data at v:0xFFFFFF800000E000, p:0xE000

OURS KERNEL PANIC
[cpu0 error] Assertion failed on Status::Ok == status
```

`MEM=2560` and below passed. After the change `MEM=3072`, `4096` and `8192` boot both images with every CPU online.

**Cause**
The objects involved:

- The trampoline buffer: three pages at `0xD000`, low enough for a startup IPI to name (its vector is a page number below 1 MiB; the kernel keeps the buffer below 64 KiB). The first holds the 16-bit code, the second the `BootstrapData` the code reads, the third a copy of the top-level page table when the real one is above 4 GiB.
- The GDT the AP boots with. `bootstrap16.S` loads GDTR with `lgdtl` in real mode, from the data page, as a 32-bit physical base.
- The kernel aspace's lower half. physload identity-mapped all usable RAM there, and the kernel unhooked it early (`x86_init_mmu_early()`, then `x86_setup_mmu()` clearing `pgd[0]`), so that a stray low address faults instead of reaching physical memory.

What happens to them when an AP starts:

1. Before paging, `lgdtl` records the GDT's physical base.
2. The AP loads CR3 with the kernel's root and sets CR0.PG. The instruction pointer does not change, so the very next fetch, still at `0xD0xx`, goes through that root.
3. `lretl` loads the 64-bit code selector. That reads a descriptor at GDTR's base -- now a linear address, translated through the same root.

So at that moment the kernel's root must map, each to itself, the trampoline's code and data and the page holding the GDT. The lower half is empty by then, so `make_bootstrap_area()` maps back exactly what the AP needs. It did that with two identity maps, both `ErrorIfExisting`: one of the buffer, and one of the GDT's page.

Where the GDT was depended on the machine. The kernel's own GDT is in the kernel image, which physload places near the top of RAM. Below 4 GiB, the trampoline used it in place -- hence the second map. Above 4 GiB, a 32-bit GDTR cannot reach it, so `fill_bootstrap_tables()` copied it into the buffer's data page and pointed GDTR at the copy. But the second map was unconditional: it identity-mapped `gdt_base` whatever it was, and on the copy path `gdt_base` was `0xEC40`. `map()` refuses a virtual address that is not page aligned with `InvArgs` (`range_is_valid()` in `page_tables.hpp`), and had it been aligned, `0xE000` was already mapped by the first call, which `ErrorIfExisting` answers with `AlreadyExists`. Either way the `ASSERT` fired. The copy never needed a map of its own; it was inside the one already made.

Why it waited until today. The copy path runs only when the kernel image lies above 4 GiB, which physload does only on a machine with RAM there, and no such machine got through phys before the two phys fixes earlier today (`docs/changes/261029/phys.md`). The code was already in the tree at `4e27a3fc` (2026-08-30) and had never executed. A path that only one class of machine takes is a path that is not tested until that machine turns up, and then it fails with all its bugs at once -- this one had two.

The second is in the copy's placement: `data + sizeof(*data)`. `data` is a `BootstrapData *`, and adding to a typed pointer steps whole structures, so this stepped 56 structures of 56 bytes, 3136 bytes, `0xC40` -- the low bits of the `0xEC40` in the log. The intent was "just past the structure", 56 bytes, which rounds to `0x40`. It worked only because the structure is small: `0xC40` plus the 56-byte GDT still fits in the page, but at 64 bytes the step is exactly a page, and the copy would have landed on the page-table copy's page and failed the `ASSERT` that bounds it.

**Change**
The GDT is copied into the buffer on every machine, and the second map is gone. Before, in `fill_bootstrap_tables()`:

```cpp
auto gdt_base = mm::virt_to_phys(VirtAddr(x86_get_gdt()));
if (gdt_base > MAX_BOOTSTRAP_PHYSICAL_ADDRESS - (GDT_SIZE - 1)) {
    auto gdt_start = reinterpret_cast<u8 *>(
        ustl::round_up(reinterpret_cast<VirtAddr>(data + sizeof(*data)), 16));
    ASSERT(gdt_start + GDT_SIZE <= area + PAGE_SIZE * 2);
    memcpy(gdt_start, x86_get_gdt(), GDT_SIZE);
    gdt_base = BOOTSTRAP16_BUFFER_BASE + static_cast<PhysAddr>(gdt_start - area);
}
ASSERT(gdt_base <= MAX_BOOTSTRAP_PHYSICAL_ADDRESS - (GDT_SIZE - 1));
```

After:

```cpp
auto *const gdt_start =
    reinterpret_cast<u8 *>(ustl::round_up(reinterpret_cast<VirtAddr>(data + 1), 16));
ASSERT(gdt_start + GDT_SIZE <= area + PAGE_SIZE * 2);
memcpy(gdt_start, x86_get_gdt(), GDT_SIZE);
auto const gdt_base = BOOTSTRAP16_BUFFER_BASE + static_cast<PhysAddr>(gdt_start - area);
```

The 32-bit range check on the GDT went with the branch: the copy is inside the buffer, which `set_bootstrap16_buffer()` keeps below 64 KiB. In `make_bootstrap_area()`, the second of these two calls is deleted, and the first passes `nullptr` rather than `0` for its pointer argument:

```cpp
status = mm::VmAspace::kernel_aspace()->arch_aspace().map(
    gdt_base, gdt_base, 1, mm::MmuFlags::PermMask | mm::MmuFlags::Discache,
    mm::ArchMapOptions::ErrorIfExisting | mm::ArchMapOptions::UndoOnFailure, 0);
ASSERT(Status::Ok == status);
```

The GDT is copied again each time a CPU is started through hotplug (`prepare_bootstrap_area()` calls the same function). It would not need to be -- the trampoline reads only the kernel code selector, which never changes -- but the page-table copy beside it does, and copying 56 bytes costs nothing.

The AP stops using the copy once `x86_start_nonboot_cpu()` has set up its CpuLocal area and local APIC and reaches `x86_init_percpu()`, whose `x86_load_gdt()` points the AP's GDTR at the kernel's real GDT by its virtual address. Until then the GDTR still names the copy, which stays in the buffer for as long as the buffer does.

Rejected:

- Skipping the second map only when the GDT is the copy. The smallest change, and an experiment showed it boots `MEM=3072` and `MEM=8192`. It keeps two paths, each run by one class of machine, so the next bug in either waits for that machine again. It also keeps an identity mapping of a kernel-image page -- writable, executable and uncached -- in the kernel aspace's lower half for good, alongside the cached mapping the kernel uses for the same page.
- Keeping physload's identity map until the APs are up, so no map is needed at all (the user asked about this). Hotplug starts CPUs again at any time, so the map could never be removed; `pgd[0]` by then aliases `pgd[511]`, where the low addresses the trampoline runs at fall on the physmap's entries, which are never executable, and the trampoline turns on NXE before paging, so its next fetch would fault; and the lower half would stay mapped through the whole of early boot, hiding null dereferences.
- A page table of the trampoline's own, so the kernel aspace's lower half stays empty for good: the shape the references below use, and the right one long term, but larger than this fix.

**Reference**
Linux keeps the AP's boot GDT inside the real-mode trampoline itself: `tr_gdt` in `arch/x86/realmode/rm/trampoline_64.S`, loaded by `lgdtl tr_gdt` in `trampoline_start`, under the comment "Duplicate the global descriptor table so the kernel can live anywhere" -- this change's reason, in one line. It sits wherever the trampoline does, below 1 MiB, and is covered by the trampoline's own mapping, which is the shape this change moves to. The AP loads its real per-CPU GDT once it runs in the kernel. Linux also starts the AP on a page table of the trampoline's own, `trampoline_pgd`, filled in `setup_real_mode()` (`arch/x86/realmode/init.c`): entry 0 maps the real-mode stub virtual == physical, and the kernel half's top-level entries are copied from `init_top_pgt`, so the kernel's own page table never has to map low memory for it: the rejected third option above.

**Verification**
- Before the fix, a temporary probe in `make_bootstrap_area()` repeated the failing call and a page-aligned one: `original call InvArgs, page-aligned AlreadyExists`. Skipping the second map when the GDT was in the buffer booted `MEM=3072` and `MEM=8192` on both images with all CPUs online.
- `ours-verify` on the default machine (512M, 4 CPUs): build ok, dbg 44 passed, rel 41 passed and 6/6 required, host tests 151 passed.
- `MEM=2560`, `3072`, `4096`, `8192`, `NUMA=1` and `NUMA=33`, both images: all pass. Every dbg log shows `Bootstrap GDT at 0xE040` -- the copy, 0x40 into the data page -- and CPUs 1 to 3 online (1 to 7 on the NUMA machines). Each log was checked to carry this build's image digest: `boot_check.py` keeps its logs in a temporary directory named after the build directory, which another worktree's `out/bringup.x86` shares, and one run's log was overwritten by another session's boot; the NUMA runs were repeated with a private `TMPDIR`.
- `hotplug.test` on the dbg image, at `MEM=512` and `MEM=8192`: `ok -- 47 checks`, CPU 3 coming online six times, each restart taking the GDT copy again through `prepare_bootstrap_area()`.
- clang-format clean; clang-tidy on `bootstrap.cpp` reports nothing after passing `nullptr`.

**Deliberately not done**
- The page-table copy is still taken before the buffer is mapped at boot, which would leave the boot-time APs without the lower-half entry the map creates. It is not reached today, since physload keeps the root below 4 GiB, and it is a separate fix: `docs/repairs/261029/x86-kernel.md`.
- The page tables of physload's identity map stay reserved after the kernel unhooks them; already filed in `docs/repairs/261004/x86-mm.md`.
- No trampoline page table of its own (see Change).

**How it was found**
`MEM=3072` on the dbg image put `Bootstrap GDT at 0xEC40` just before the panic: an address inside the three-page buffer at `0xD000` -- the copy path -- and not page aligned. Reading `map()` showed its alignment check returns `InvArgs`, and the first map already covers the page; the temporary probe confirmed both statuses. The `0xC40` offset matched 56 times 56, the size of `BootstrapData`, while the copy's line was being rewritten.

**The user's prediction**
Not given: homework is paused. The user chose always copying over skipping the map, after asking why a second map was needed at all and whether keeping the identity map until the APs were up would avoid it (see Change).

**Three questions you should be able to answer**
1. The GDT is loaded before paging is turned on. Why must its page still be identity-mapped when the AP executes `lretl`?
2. The copy path had been in the tree since August. Why did nobody see it fail, and what about this change keeps the same thing from happening to the GDT copy again?
3. Why did `data + sizeof(*data)` place the copy 0xC40 bytes into the page, and at what size of `BootstrapData` would it have broken?
