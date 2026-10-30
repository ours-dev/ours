# x86-kernel outstanding repairs

## 1. A boot-time AP keeps running on the trampoline's page-table copy, which a kernel without hotplug frees

**Severity**
Medium. Wrong, and not reached today: it needs the kernel root above 4 GiB, which physload never produces, and a kernel built with `cpu_hotplug=false`.

**Symptom**
Latent, cannot be triggered today. With the root above 4 GiB and `cpu_hotplug=false`, every application processor started at boot would keep its CR3 on a page the kernel has returned to the frame allocator. Once that page is handed out again and written, the next page walk on any of those CPUs reads garbage as its top-level table: a fault with whatever IDT it has, most likely a triple fault and a reset, at a time unrelated to its cause.

**Cause**
When the kernel root is above 4 GiB, out of reach of the 32-bit CR3 an AP loads in protected mode, `fill_bootstrap_tables()` in `kernel/arch/x86/kernel/bootstrap.cpp` copies the root's page into the trampoline buffer's third page, and the trampoline loads that copy into CR3 (`kernel/arch/x86/kernel/bootstrap16.S`).

The copy was meant to last only through the trampoline, but nothing replaces it afterwards. `x86_start_nonboot_cpu()` (`kernel/arch/x86/kernel/mp.cpp`) and `x86_init_percpu()` (`kernel/arch/x86/kernel/init.cpp`) never write CR3. The only other CR3 write is `ArchVmAspace::switch_context()` (`kernel/arch/x86/mm/arch_vm_aspace.cpp`), which the scheduler calls only when the outgoing and incoming threads have different address spaces (`kernel/kernel/task/sched/scheduler.cpp`, `curr->aspace() != next->aspace()`). Kernel threads all have the kernel aspace (`kernel/kernel/task/thread.cpp`), so an AP stays on the copy until it first runs a user thread.

Without `CONFIG_CPU_HOTPLUG`, `free_bootstrap16_buffer()` returns all three buffer pages to the frame allocator at `InitLevel::Platform`, which `kernel/top/main.cpp` sets as soon as `init_platform()` has brought the APs up. No user thread exists yet, so every AP's CR3 still names the third page. The comment over it, "Its identity mapping is left: nothing reaches it", is right about the identity map and wrong about CR3.

With hotplug on, the buffer is kept and nothing is freed. `prepare_bootstrap_area()` does copy the root over that same page while the boot-time APs are running on it, but the contents it writes are the same entries, so that is harmless today.

Found in review of `docs/changes/261030/x86-kernel.md`; older than that change. The forced-copy experiments recorded there all ran with the default `cpu_hotplug=true`, so they could not show it.

**Change**
Have each AP load the real kernel root as soon as it runs in the kernel half, early in `x86_start_nonboot_cpu()` -- by switching to the kernel aspace the way the scheduler would, so that the aspace's active-CPU mask stays right, rather than by a bare CR3 write. The copy then lives only for the trampoline, which is what freeing the buffer and re-copying on hotplug both assume. Ruled out: keeping the third page when a copy was taken. It stops the use-after-free, but leaves every AP on a table that `prepare_bootstrap_area()` overwrites in place, and the active-CPU mask of the kernel aspace still never names the APs.

**Reference**
Linux starts an AP on its trampoline's own page table, `trampoline_pgd`, and `secondary_startup_64` in `arch/x86/kernel/head_64.S` loads the kernel's page table into CR3 before the AP goes on to C, so no AP keeps running on the trampoline's table.

**Acceptance**
With `fill_bootstrap_tables()` temporarily made to take the page-table copy whatever the root's address, a kernel built with `cpu_hotplug=false` boots both images with every CPU online, and so does the default build with `hotplug.test` passing on the dbg image. A log line or kshell check shows each AP's CR3 equal to the kernel root once it is online.
