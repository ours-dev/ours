# x86-kernel outstanding repairs

## 1. At boot, the trampoline's page-table copy is taken before the trampoline is mapped

**Severity**
Medium. Wrong, and not reached today: only a kernel root above 4 GiB is copied, and physload keeps the root below 4 GiB.

**Symptom**
Latent, cannot be triggered today. If the kernel aspace's root ever lay above 4 GiB, the first application processor started at boot would take a page fault on its first instruction fetch after turning paging on, with no IDT to handle it, and triple-fault, which resets the machine. A CPU started later through hotplug would come up.

Reproduced on 2026-10-29 by forcing the copy on the default machine (the condition in `fill_bootstrap_tables()` made always true): `Bootstrap PGD at 0xF000`, `Attempt to wake up CPU[1]`, and QEMU exits -- `reset or triple fault? ... reached: kernel`.

**Cause**
`make_bootstrap_area()` in `kernel/arch/x86/kernel/bootstrap.cpp` calls `fill_bootstrap_tables()` first, and identity-maps the trampoline buffer into the kernel aspace after. When the root is above 4 GiB, out of reach of the 32-bit CR3 an AP loads in protected mode, `fill_bootstrap_tables()` copies it into the buffer's third page and points the AP at the copy. The map then writes into the live root, not the copy: the kernel unhooked the lower half early (`x86_setup_mmu()` clears `pgd[0]`), so unless something has mapped below 512 GiB since, the map creates `pgd[0]` -- and the copy, taken a moment earlier, does not have it. `x86_bring_aps()` starts every boot-time AP from that copy. Only `prepare_bootstrap_area()`, the hotplug path, copies again, and it runs after the map.

It is not reached because physload allocates the root below 4 GiB (`aspace.set_allocation_bounds(0, 4_gb)` in `kernel/arch/x86/phys/aspace.cpp`) and the kernel aspace keeps physload's root. `fill_bootstrap_tables()`'s own comment already says a copy is a snapshot that goes stale; the boot path takes it at the one moment it is guaranteed to be stale.

**Change**
Move the `fill_bootstrap_tables()` call in `make_bootstrap_area()` below the map. Since the GDT copy (`docs/changes/261029/x86-kernel.md`) the map no longer depends on anything that call computes, so nothing else has to move except the two trace lines: the GDT line prints the call's return value, and the PGD line reads the data page, which before the call still holds the zeroes it was cleared to (`Bootstrap PGD at 0x0`). Tried with the copy forced, on 2026-10-29: both images boot and CPUs 1 to 3 come online.

**Reference**
None, this project's own design: the trampoline is pointed at the kernel's own root, so a copy of it has to be taken after the last change the AP needs.

**Acceptance**
With `fill_bootstrap_tables()` temporarily made to take the page-table copy whatever the root's address, the default machine boots both images with every CPU online, and `hotplug.test` passes on the dbg image; without the fix, the same forced copy resets the machine at the first AP, as reproduced above.
