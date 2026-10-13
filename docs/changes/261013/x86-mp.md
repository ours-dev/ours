# x86-mp changes

## The AP start-up path can run again after boot, and sends the second startup IPI (2026-10-13)

**Files**
- `kernel/arch/x86/kernel/mp.cpp`, `kernel/arch/x86/kernel/include/ours/arch/mp.hpp` — `arch_activate_cpu()`, `arch_deactivate_cpu()` and `arch_cpu_halt_offline()`; `send_startup()` sends two SIPIs 200 µs apart, for boot too; `x86_start_nonboot_cpu()` is `CPU_INIT_CODE` and takes a CPU coming back to its idle thread
- `kernel/arch/x86/kernel/bootstrap.cpp`, `kernel/arch/x86/kernel/include/ours/arch/x86/bootstrap.hpp` — `fill_bootstrap_tables()` factored out; `prepare_bootstrap_area()`; the buffer is freed only without CPU hotplug
- `kernel/arch/x86/apic/local_apic.cpp`, `kernel/arch/x86/kernel/include/ours/arch/apic.hpp` — `setup_local_apic_percpu()` (all but the timer) and `restore_local_apic_percpu()`; the LVT helpers are `CPU_INIT_CODE`
- `kernel/arch/x86/kernel/init.cpp`, `feature.cpp`, `descriptor.cpp` — `x86_init_percpu()`, `x86_enable_syscall()`, `x86_init_feature_percpu()`, `x86_setup_tss_percpu()` are `CPU_INIT_CODE`

**Symptom**
Closes items 1 and 2 of `docs/repairs/260909/x86-mp.md`:
- Item 1: one SIPI where the MP protocol asks for two. Harmless under QEMU; a real processor that misses the first stays down.
- Item 2: `arch_activate_cpu()` and `arch_deactivate_cpu()` answered Unimplemented; they were scaffolding for hotplug.

**Cause**
- Item 1: `x86_bring_aps()` sent one Startup IPI per CPU. The second is the protocol's own retry, for a processor that was not yet waiting for one (Intel SDM Vol. 3A, 9.4.4.1).
- Item 2: no CPU started after boot, and everything it would start through was `INIT_CODE`: the per-CPU set-up, the LVT helpers, and the trampoline buffer, freed at `InitLevel::Platform`.

**Change**
- **Two SIPIs.** `send_startup()` sends SIPI, waits 200 µs, sends SIPI again. Used at boot and for a CPU started again.
- **Activate and deactivate.** `arch_deactivate_cpu()` INITs a halted CPU. `arch_activate_cpu()` makes the trampoline ready for the CPU (`prepare_bootstrap_area()`), sends INIT, waits 10 ms, and sends the two SIPIs. The CPU starts on its idle thread's stack, which is still its current thread.
- **The trampoline tables.** `fill_bootstrap_tables()` re-copies the GDT and the top-level page table below 4 GiB when they lie above it. A copy is a snapshot, so it is taken again for each start.
- **The local APIC.** It is set up again on the way back by everything but `init_apic_timer()`, which enrolls the timer device. The device stays enrolled and bound, and the tick layer puts it back into service.
- **Keeping it all.** With `CONFIG_CPU_HOTPLUG` the start-up functions are plain text and the buffer stays, with its identity mapping. Without it everything is freed as before.

**Reference**
- Zircon `x86_bringup_aps()` (`zircon/kernel/arch/x86/smp.cc`): INIT, 10 ms, STARTUP twice.
- Zircon `arch_mp_cpu_hotplug()` and `platform_mp_cpu_unplug()` (`zircon/kernel/arch/x86/mp.cc`).
- Linux `wakeup_secondary_cpu_via_init()`.

**Verification**
See the 2026-10-13 entry in `docs/changes/261013/kernel-task.md`: `hotplug.test` brings CPU 3 back through this path twice; both images boot with and without `cpu_hotplug`, and boot itself now sends two SIPIs to every AP.

**Deliberately not done**
- Item 3 of the repair file (`movzwl %ds, %ebx` in the trampoline): it does not change with a second start, since INIT resets the registers as at boot.
- Starting the APs in parallel (item 4).

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the SDM ask for a second startup IPI, and why does QEMU never need it?
2. Why is the GDT or page-table copy in the trampoline buffer taken again for every start rather than once at boot?
3. What does `CPU_INIT_CODE` expand to with and without `cpu_hotplug`, and what decides that the buffer is freed?
