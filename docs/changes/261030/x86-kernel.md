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

## An AP moves onto the kernel's own root as soon as it is in the kernel (2026-10-30)

**Files**
- `kernel/arch/x86/kernel/mp.cpp` — `leave_trampoline_tables()` switches the CPU to the kernel address space. `x86_start_nonboot_cpu()` calls it on a CPU's first start, after `x86_init_percpu()`; a CPU coming back through hotplug reaches it as its `AddressSpace` hotplug hook.
- `kernel/include/ours/cpu_hotplug_hook.hpp` — a new lowest level, `AddressSpace`, below `Tick`.
- `kernel/arch/x86/kernel/bootstrap.cpp` — the comments on `fill_bootstrap_tables()` and `free_bootstrap16_buffer()` say the page-table copy lasts only through the trampoline, and which CPUs that covers.
- `kernel/kernel/time/tick_device.cpp` — the Tick hook's comment no longer calls Tick the lowest level.
- `.agents/skills/ours-kernel-design/references/task.md` — the list of hotplug levels names AddressSpace, and why it comes first.
- `docs/repairs/261030/x86-kernel.md` — deleted; its one item is this one.

**Symptom**
Latent, cannot be triggered today: it needs the kernel root above 4 GiB, which physload never produces, and a kernel built with `cpu_hotplug=false`. Reproduced on 2026-10-30 in a `cpu_hotplug=false` build with the copy forced and the copy's page zeroed just before the buffer is freed, standing in for the frame's next owner: every AP still had `cr3 0xf000`, the copy, after it came online, and both images ended `QEMU exited (rc 0) before the shell finished -- reset or triple fault?` right after the zeroing. With the change, each AP reports `cr3 0xf000` before the switch and `0x1fedb000`, the kernel root, after it, and both images boot to the end with the page zeroed.

**Cause**
The objects involved:

- The kernel root, at `0x1FEDB000` on the default machine, and the trampoline's copy of it in the buffer's third page, at `0xF000`. The previous entry in this file explains why the copy exists and when it is taken.
- CR3 on each AP. The trampoline loads it with whatever the trampoline data names: the root, or the copy where the root is above 4 GiB.
- The two CR3 writes in the kernel after that: `ArchVmAspace::switch_context()` (`kernel/arch/x86/mm/arch_vm_aspace.cpp`), reached from the scheduler, and a TLB flush that rewrites CR3 with its own value.
- The trampoline buffer's lifetime. With `cpu_hotplug=true` it is kept for CPUs started later; with `cpu_hotplug=false`, `free_bootstrap16_buffer()` returns all three pages to the frame allocator at `InitLevel::Platform`, as soon as `init_platform()` has brought the APs up.

What happened, in order:

1. An AP came out of the trampoline with CR3 on the copy.
2. Nothing on its way into the kernel wrote CR3: not `x86_start_nonboot_cpu()`, not `x86_init_percpu()`, not `start_nonboot_cpu()`.
3. The scheduler calls `switch_aspace()` only when the outgoing and incoming threads have different address spaces (`kernel/kernel/task/sched/scheduler.cpp`). Every kernel thread, the AP's idle thread included, has the kernel's (`kernel/kernel/task/thread.cpp`), so the AP stayed on the copy until it first ran a user thread.
4. With hotplug off, the buffer was freed at `InitLevel::Platform`, before `userboot` creates the first user process. Every AP was still walking its page tables from a frame the allocator could hand to anyone. What happens next depends on what the new owner writes there: a page of zeroes, as in the experiment, resets the machine at the next page walk that misses the TLB; a page that leaves the kernel's entry alone changes nothing; anything else is a fault or silent misbehaviour.

The copy was always meant to be a trampoline-only thing -- the buffer's lifetime assumes it, and so does hotplug copying over it to start the next CPU -- but no code ended it. The previous entry in this file made this reachable under its condition: before it, the same copy reset the machine at the first AP, so no AP ever got far enough to be stranded on it.

**Change**
The switch is the scheduler's own, `VmAspace::switch_aspace()`, rather than a bare CR3 write, so its other effects are kept as for any switch between threads: this CPU's bit in the address space's active mask, and the address space's I/O ports in the CPU's TSS. Those effects put two conditions on where it can run, and they are why it is in two places. Before, the end of `x86_start_nonboot_cpu()` wrote CR3 nowhere:

```cpp
x86_init_percpu(cpunum);

if (coming_back) {
    idler.come_back();
}

start_nonboot_cpu();
```

After:

```cpp
CPU_INIT_CODE
auto leave_trampoline_tables() -> void {
    mm::VmAspace::switch_aspace(nullptr, mm::VmAspace::kernel_aspace());
}

#ifdef CONFIG_CPU_HOTPLUG
CPU_HOTPLUG_HOOK(KernelRoot, ::ours::CpuHotplugLevel::AddressSpace, nullptr,
                 &leave_trampoline_tables)
#endif

// in x86_start_nonboot_cpu():
x86_init_percpu(cpunum);

if (coming_back) {
    idler.come_back();      // does not return; runs the hook after its claim
}

leave_trampoline_tables();  // a first start
```

- After `x86_init_percpu()`, because loading the I/O ports needs this CPU's TSS, which that sets up.
- On a CPU coming back, after `IdleThread::come_back()` has claimed the start. Loading the I/O ports takes the I/O bitmap's spin lock, and `come_back()` documents that nothing before its claim may take a lock (`kernel/kernel/task/idle_thread.cpp`). The reason is a race with a withdrawn start: `transition()` gives up on a CPU that misses its deadline, and a caller that retries at once sends a fresh INIT. If that INIT caught the late CPU inside the I/O bitmap's lock, the lock would never be released, and every later switch between a user and a kernel thread, on any CPU, would spin on it with interrupts off.

`come_back()` runs its online hotplug hooks right after the claim and then never returns, so the come-back half could not simply follow it in `x86_start_nonboot_cpu()`. A new lowest hook level, `AddressSpace`, below `Tick`, puts the switch first among them, so every hook above runs on the kernel's own tables. It has no offline half: a CPU goes offline from its idle thread, on the kernel's root.

The first version of this change made the switch in one place, before `if (coming_back)`, and so took the lock before the claim on the hotplug path. Review caught it against `come_back()`'s comment; `hotplug.test` could not have, since its withdrawn-start case waits for the late CPU to halt before it starts it again. Two alternatives were weighed and the user chose the hook level:

- Switching only on a first start. The use-after-free needs `cpu_hotplug=false`, where no CPU ever comes back, so it fixes the bug; but a restarted CPU would then run on the copy until its first user thread, and the copy would outlive the trampoline after all.
- Doing only the lock-free part before the claim: a CR3 write and the mask bit, without the I/O ports. It needs a second switch path in `ArchVmAspace` beside the scheduler's, for one caller.

Rejected for the bug itself: keeping the third page when a copy was taken. It stops the use-after-free but leaves every AP running on a page that `prepare_bootstrap_area()` overwrites in place to start the next CPU, and the kernel address space's active mask would not name an AP until its first switch back from a user thread.

There is an unused `mm::set_active_aspace()` in `kernel/mm/vmm.cpp` that does the same as the switch here, declared in no header. It was left alone rather than given a declaration and its first caller, since `VmAspace::switch_aspace()` is the public interface the scheduler already uses.

**Reference**
Linux's `secondary_startup_64` (`arch/x86/kernel/head_64.S`) loads the physical address of `init_top_pgt` into CR3 before it goes on to `common_startup_64`, under a comment saying it switches away from the trampoline's page table. An AP there leaves the trampoline's table in its first instructions in the kernel, with a bare CR3 write and nothing else; this kernel does it a little later, through the address-space switch, once the per-CPU state that switch needs is in place.

**Verification**
- `ours-verify` on the default machine: build ok, dbg 44 passed, rel 41 passed and 6/6 required, host tests 151 passed.
- `MEM=8192` and `NUMA=1`, both images: pass, every CPU online.
- `hotplug.test` on the dbg image at `MEM=512` and `MEM=8192`: `ok -- 47 checks`.
- Experiments with temporary changes, not committed: the copy forced, the copy's page zeroed before the buffer is freed, and CR3 logged before and after the switch in `leave_trampoline_tables()`.
  - `cpu_hotplug=false` build (`out/hotplug-off.x86`), with the switch: both images pass, CR3 `0xf000` then `0x1fedb000` on CPUs 1 to 3. Without it: CR3 stays `0xf000`, and both images reset right after the zeroing.
  - Default build, with the switch, running `hotplug.test`: CPU 3 starts six times, once at boot and five times through hotplug, and each time logs `0xf000` before and `0x1fedb000` after -- the five restarts through the `AddressSpace` hook, after the claim. The test passes.
- Logs kept under a private `TMPDIR`, each checked to carry this build's image.
- clang-format clean; clang-tidy reports nothing on `mp.cpp` or `bootstrap.cpp`.

**Deliberately not done**
- The boot CPU is not added to the kernel address space's active mask. It never ran on a copy, so its CR3 was always right, and the kernel's shootdowns go to every online CPU whatever the mask says (`ArchVmAspace::invalidate()`); the mask is only kept right here because the switch keeps it.
- A CPU that `x86_bring_aps()` gives up waiting for may still be running from the buffer when it is freed, from any of its pages, not only the copy. That is older than this change and untouched by it; the buffer comment now says so.
- A CPU whose hotplug start is withdrawn fails `come_back()`'s claim and halts before any hook runs, still on the copy, until its next start resets it. `hotplug.test` exercises it on every run. Moving it off would mean taking the I/O bitmap's lock before the claim, the very thing this change avoids; it is safe because with hotplug the buffer is never freed and every new copy has the same entries, and the comment on `leave_trampoline_tables()` says so.
- No in-tree test forces the copy; the switch is exercised on every AP of every boot regardless, and the forced experiments are recorded here.
- `set_active_aspace()` is not removed or declared (see Change).

**How it was found**
Review of the previous entry's change: a reviewer traced CR3 from the trampoline through the AP's start and found no write before the first user thread, and the hotplug-off free under it; two independent checks traced the same chain and could not refute it. Review of this change's first version then found the lock taken before `come_back()`'s claim, confirmed independently the same way.

**The user's prediction**
Not given: homework is paused. The user chose the hook level over switching only on a first start.

**Three questions you should be able to answer**
1. Why does a kernel thread switch never move a CPU off the trampoline's copy?
2. Why may the switch take a lock on a first start but not before `come_back()`'s claim, and what would a retried INIT do to a CPU holding one?
3. Why could the come-back half not simply follow `idler.come_back()` in `x86_start_nonboot_cpu()`?
