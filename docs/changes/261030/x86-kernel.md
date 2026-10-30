# x86-kernel changes

## The trampoline's page-table copy is taken after the trampoline is mapped (2026-10-30)

**Files**
- `kernel/arch/x86/kernel/bootstrap.cpp` — `make_bootstrap_area()` calls `fill_bootstrap_tables()` after the identity map of the trampoline buffer instead of before it, with the two trace lines that print its results.
- `docs/repairs/261029/x86-kernel.md` — deleted; its one item is this one.

**Symptom**
Latent, cannot be triggered today: it needs the kernel aspace's root above 4 GiB, and physload keeps it below. Forcing the copy on the default machine (the condition in `fill_bootstrap_tables()` made always true) reproduced it on 2026-10-29: `Bootstrap PGD at 0xF000`, `Attempt to wake up CPU[1]`, and QEMU exited -- `reset or triple fault? ... reached: kernel`. With the same forced copy after the change, both images boot with every CPU online.

**Cause**
The objects involved:

- The kernel aspace's root, the table every CPU's CR3 names. The kernel half is one top-level entry of it, `pgd[511]`, covering all 512 GiB of kernel addresses. The lower half has been empty since `x86_setup_mmu()` cleared `pgd[0]`.
- The root's copy in the trampoline buffer's third page. An AP loads CR3 in protected mode, where it is 32 bits wide, so a root above 4 GiB is out of its reach, and `fill_bootstrap_tables()` copies the root's page into the buffer, below 64 KiB, and hands the AP that copy. A copy duplicates the top-level entries only; every table beneath them is shared with the real root.
- The identity map of the buffer, which `make_bootstrap_area()` adds so the AP can keep running at `0xD0xx` once paging is on.

What happened at boot, in order:

1. `make_bootstrap_area()` called `fill_bootstrap_tables()`, which copied the root while `pgd[0]` was still empty.
2. It then identity-mapped the buffer. That map went into the real root, and with `pgd[0]` empty it had to create it: a new lower-half table, hung from a new top-level entry.
3. `x86_bring_aps()` started every boot-time AP from the copy, which had never seen step 2. The AP turned paging on, fetched its next instruction at `0xD0xx` through `pgd[0]` of the copy, found nothing, and took a page fault with no IDT to deliver it to -- a triple fault, which resets the machine.

The kernel half was never at risk: anything the kernel maps there, before or after the copy, hangs below `pgd[511]`, which the copy shares. The one top-level entry the copy could miss was the very one the trampoline needs, and it was missing every time, because the code created it a moment after the copy.

The hotplug path was already safe, by design: `prepare_bootstrap_area()` takes the copy again for each CPU it restarts, precisely so that a top-level entry added after boot is not missed -- and the trampoline map's `pgd[0]` is such an entry. The boot path was the only one that used a copy taken too early. And it stayed hidden because the copy is only taken for a root above 4 GiB, which physload never produces: it bounds the root's allocation to `[0, 4 GiB)` (`kernel/arch/x86/phys/aspace.cpp`).

**Change**
Before, in `make_bootstrap_area()`:

```cpp
auto const gdt_base = fill_bootstrap_tables(bootstrap_area_start, bootstrap_data);
log::trace("Bootstrap GDT at 0x{:X}", gdt_base);
// ... debug symbols, the PGD trace line, the hotplug slot ...
auto const status = mm::VmAspace::kernel_aspace()->arch_aspace().map(
    BOOTSTRAP16_BUFFER_BASE, BOOTSTRAP16_BUFFER_BASE, MIN_BOOTSTRAP16_BUFFER_SIZE >> PAGE_SHIFT, ...);
ASSERT(Status::Ok == status);
```

After:

```cpp
auto const status = mm::VmAspace::kernel_aspace()->arch_aspace().map(
    BOOTSTRAP16_BUFFER_BASE, BOOTSTRAP16_BUFFER_BASE, MIN_BOOTSTRAP16_BUFFER_SIZE >> PAGE_SHIFT, ...);
ASSERT(Status::Ok == status);

auto const gdt_base = fill_bootstrap_tables(bootstrap_area_start, bootstrap_data);
log::trace("Bootstrap GDT at 0x{:X}", gdt_base);
log::trace("Bootstrap PGD at 0x{:X}", bootstrap_data->pgd);
```

The previous change (`docs/changes/261029/x86-kernel.md`) made the move simpler, not possible. There used to be two maps: the buffer's, which needs nothing the call computes, and the GDT page's, which needs `gdt_base`, the call's return value. The call could always have gone between the two, since the GDT page lies below 512 GiB and its map only adds tables beneath the `pgd[0]` the buffer's map had just made, which the copy shares. With the GDT map gone, it simply goes after the one map left. The two trace lines move with the call: the GDT line prints its return value, and the PGD line reads the data page, which before the call still holds the zeroes `memset` left (`Bootstrap PGD at 0x0`).

Rejected: taking the copy again for each boot-time AP, as hotplug does. It is not needed: once the buffer is mapped, the only top-level entries the copy could fall behind on are lower-half ones, and nothing creates one. Every other mapping into the kernel aspace goes through the kernel `VmAspace`, whose range is the kernel half (`VmAspace::init_kernel_aspace()`, `kernel/mm/vm_aspace.cpp`); the trampoline's map is the only direct arch-level map into it.

**Reference**
Linux fills its trampoline's page table, `trampoline_pgd`, once in `setup_real_mode()` (`arch/x86/realmode/init.c`), before any AP is started: entry 0 maps the real-mode stub virtual == physical, and the kernel half's top-level entries are copied from `init_top_pgt`. The same order as after this change -- every entry the AP needs is in place before the copy is made -- with the difference that Linux's identity entry lives only in the trampoline's own table, while this kernel's lives in the real root and reaches the AP through the copy.

**Verification**
- `ours-verify` on the default machine: build ok, dbg 44 passed, rel 41 passed and 6/6 required, host tests 151 passed; `Bootstrap PGD at 0x1FEDB000`, CPUs 1 to 3 online.
- `MEM=3072`, `MEM=8192` and `NUMA=1`, both images: pass, every CPU online (1 to 7 on NUMA), the root used in place (`0x7FEDB000`, `0x3FEBB000`).
- `hotplug.test` on the dbg image at `MEM=8192`: `ok -- 47 checks`.
- With the copy forced, a temporary change not committed: the default machine and `MEM=8192` boot both images with CPUs 1 to 3 online and `Bootstrap PGD at 0xF000`, and `hotplug.test` passes on both (47 checks). Without the change the same forced copy reset the machine at the first AP.
- All of these ran with the default `cpu_hotplug=true`, which keeps the buffer after boot; see Deliberately not done for what that leaves untested.
- Every boot log was checked to carry this build's image digest, with `TMPDIR` pointed at a private directory so another worktree's `out/bringup.x86` could not write into the same logs.
- clang-format clean; clang-tidy reports nothing on `bootstrap.cpp`.

**Deliberately not done**
- No test that forces the copy in the tree. It would need a switch in `fill_bootstrap_tables()` that exists only for testing; the forced experiment is recorded here and in the repair item it closes instead.
- The trampoline still runs on the real root, or a copy of it, rather than a page table of its own; see the previous change's Rejected list.
- A boot-time AP never leaves the copy for the real root: nothing on its path writes CR3, and the scheduler does only when it switches to a thread of another address space. With `cpu_hotplug=false` the buffer, copy included, is freed while the APs still run on it. Found in review of this change, older than it, and filed separately: `docs/repairs/261030/x86-kernel.md`.

**How it was found**
While fixing the GDT map (the previous change), reading `make_bootstrap_area()` for the order of its calls: the copy came first, the map second, and the boot path never copied again. The forced-copy experiment confirmed the fault before the fix and its absence after.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The copy duplicates only the root's page. Why does it still see a kernel-half mapping the kernel adds after the copy, but not the lower-half entry the trampoline map creates?
2. Why was restarting a CPU through hotplug safe even before this change, when starting one at boot was not?
3. Before the previous change there were two maps. Where could the call have gone then, and why would the copy have seen the second map's tables?
