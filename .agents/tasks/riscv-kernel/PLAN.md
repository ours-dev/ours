# riscv-kernel: vmours on riscv, from physboot's handoff to user space

**Date**: 2026-10-29
**Status**: Accepted

## Request
Port vmours (`//kernel/products/bin:vmours`) to riscv, now that the phys chain boots the fake kernel on QEMU `virt` (`.agents/tasks/riscv-boot-chain/PLAN.md`). The user, 2026-10-29: "开始", and for step 1a with decision A: "A，开始吧". The port ends where userboot starts the shell on QEMU `virt` and the shell finishes (step 9), then runs under `-smp 2` (step 10); performance items follow, one at a time (step 11). Each step is introduced to the user before it starts; this plan lists them all, and only step 1a is approved so far.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Stub functions | allowed only where something needs one now (the link refers to it, or a path that runs calls it); allowed to hold a place for later | only where needed now | None | the user, 2026-10-29 (`ours-kernel-design` rule 3, `dbc96a47`) |
| How vmours links before the arch layer exists | named placeholders, one per symbol the link lacks, each panicking with its own name, deleted as each is ported; real bodies only, linking once the whole arch layer exists; leave the symbols undefined (`-z undefs`) | named placeholders | None | follows from the stub rule (the user, 2026-10-29): a placeholder is what the link needs now, and nothing runs past one |
| User ABI on riscv before user space exists | A: a `kernel_no_userabi` GN switch, true by default for riscv, that leaves `//kernel/lib/syscall` and `//kernel/lib/userabi` out of the kernel (Zircon `kernel/params.gni:107-108`, defaulted on for riscv64 in `f8c925da99a`); B: build the user ABI layer anyway and write a placeholder for each symbol it lacks on riscv | A | None | the user, 2026-10-29: A |
| riscv per-CPU register (step 1b) | s11 holds the model-B offset; tp holds it; s11 points at a fixed struct, as Zircon | s11, model-B offset | None | open, put to the user with step 1b |
| riscv kernel console (step 1b) | SBI legacy putchar; the ns16550 from the device tree through `Handoff::uart` | SBI putchar | None | open, put to the user with step 1b |
| riscv Spectre V1 primitives (step 1b) | a branchless mask in inline asm (sltu, neg, and); Zircon's ternary | the mask | None | open, put to the user with step 1b |

## Steps
1. vmours links on riscv and runs to its first unported seam. Split in two, each introduced and approved on its own.
   - 1a. The generic kernel stops leaning on x86. Four behaviour-preserving changes for x86, one commit each, in this order; each passes x86 `ours-verify` and the riscv phys check (`boot_check.py --out out/bringup.riscv`, fake kernel `handoff ok`), and has its change record in `docs/changes/261029/`:
     1. `fix`: `//kernel/top` stops depending on x86. `kernel/top/BUILD.gn` drops `//kernel/arch/x86/kernel:headers`, whose include path made every header riscv lacks resolve silently to x86's copy, and `//kernel/driver`, which only x86's `time.cpp` uses and which x86's arch target now depends on. `mm/init_pmm.cpp` and `mm/init_vmm.cpp` drop an unused `<arch/x86/cache.hpp>`; `ours/platform/interrupt.hpp` includes the dispatcher `<arch/interrupt.hpp>`. A sweep of every other generic reference to x86 by name, each fixed or listed with its reason.
     2. `refactor`: the arch-neutral declarations move to generic headers in `kernel/include/ours`, as Zircon's `kernel/include/arch/user_copy.h` and `kernel/include/arch/mp.h` hold them: the four `arch_copy_*` functions with `UserCopyFault` and `UserCopyCaptureResult`, the `arch_mp_*` and CPU on/off calls generic code makes, and `time::global_clock_event()`. x86 keeps only the bodies and its own `x86_*` declarations.
     3. `refactor(logz4)`: the bootstrap console's byte output and the rate of `arch::Tick` become two functions of the platform seam, after Zircon's `platform_dputs_*` (`kernel/include/platform/debug.h`) and `ticks_per_second()` (`kernel/include/platform/timer.h`); the lock, the accounting and the domains stay shared and compile for every architecture. The panic-mode lock bound becomes a time.
     4. `build`: a `kernel_no_userabi` GN argument (decision A), true by default for riscv. True drops `//kernel/lib/syscall` and `//kernel/lib/userabi` from the kernel, and `KERNEL_NO_USERABI` compiles out the two calls into userabi in `kernel/top`. Checked on x86 with the switch on: it compiles, links and boots to where userboot would start.
     Done when: the above, plus, in a riscv probe build, no compile of generic code carries an `-I` into `kernel/arch/x86`, and `clang -H` of `kernel/top/main.cpp` and two other generic objects opens no file under `kernel/arch/x86`; `ninja -k 0` on riscv vmours reports its failing-compile count before (145) and after, and the missing headers that remain, which are step 1b's list.
   - 1b. The riscv arch headers generic code includes, `kernel/arch/riscv/kernel/start.S`, the riscv console, one named placeholder per symbol the link lacks, and a kernel-only image (fake's shape, vmours as the payload). Done when the riscv boot check passes both riscv boots: `fake.obi` still ends at `[fake-kernel] handoff ok`, and the kernel-only image prints markers C B L H U T, then vmours's own `OURS KERNEL PANIC` naming `init_arch_early` as not ported; the riscv link has no undefined symbols. Its three decisions are in the table above.
2. The kernel's own trap vector (`init_arch_early`): `stvec.S`, an `IrqFrame`, the kernel fault report, CPU 0 published Online. Done when the boot passes marker X and stops at `init_platform_early`.
3. Platform early, the device tree and the PMM: riscv `init_platform_early` and `platform_init_pmm`, a device-tree walk at StartUp, the boot hart ID in the handoff (a decision). Done when log lines carry the cpu0 tag and the boot reaches `init_vmm`.
4. The Sv39 kernel address space: `ArchVmAspace` for the kernel. Decisions: which page-table walker, ASIDs, TLB flushes. Done when `vmalloc: self test passed` and the Vmm level pass.
5. Interrupts and the timer: `cpu_possible_mask` from the device tree, a PLIC `irq::Chip`, a per-CPU one-shot clock-event device and an `rdtime` clock source. Decision: jiffies and the boot tick without a PIT. Done when the Irq and Time levels pass and a sleep returns.
6. Threads, idle and the first switch. Done when the boot passes the Platform level and `reclaim_init_area`, and `labour_routine` returns, with no userabi.
7. The kernel shell and kernel tests on riscv: kshell reads through `platform::debug_console_read`, x86-only commands gated by arch, `kernel_shell_enabled` on for riscv. Done when the arch-neutral `*.test` commands pass under `kshell_run.py`.
8. User copy and page faults. Done when `user_copy_test` passes under kshell.
9. User space: elfload for riscv, a riscv vDSO and syscall entry with an arch-neutral `SyscallFrame`, `arch_enter_uspace`, a user `ArchVmAspace`; `kernel_no_userabi` turns off for riscv. Done when userboot starts the shell on QEMU `virt` and it prints `--- end ---`.
10. SMP: harts through SBI HSM, IPIs through SBI. Decision: TLB shootdown by IPI (Zircon) or by SBI RFENCE (Linux). Done when `-smp 2` shows `CPU[1] is online` and the shell still finishes.
11. Feature and performance items (ASIDs, Sstc against SBI `set_timer`, Zicbom/Zicboz, the per-CPU access cost), each put to the user with a measurement in `docs/perf`.

Steps 2 to 11 come from the research synthesis of 2026-10-29; each is introduced, with its Zircon counterpart, before it starts.

## Files
- Step 1a.1: `kernel/top/BUILD.gn`, `kernel/arch/x86/kernel/BUILD.gn`, `kernel/mm/init_pmm.cpp`, `kernel/mm/init_vmm.cpp`, `kernel/platform/include/ours/platform/interrupt.hpp`, `docs/changes/261029/kernel-top.md`
- Step 1a.2: new `kernel/include/ours/arch_mp.hpp` and `kernel/include/ours/arch_user_copy.hpp`; `kernel/include/ours/time/clock_event_device.hpp`; `kernel/arch/x86/kernel/include/ours/arch/{mp,time}.hpp` (shrunk) and `user_copy.hpp` (deleted); their includers in `kernel/kernel/task`, `kernel/kernel/time`, `kernel/lib/user_copy`, `kernel/lib/lockup_detector`, `kernel/commands`, `kernel/platform/pc` and `kernel/arch/x86/kernel`; `docs/changes/261029/kernel-arch-seam.md`
- Step 1a.3: `kernel/lib/logz4/kernel.cpp`, `kernel/platform/include/ours/platform/{debug_console,timer}.hpp`, `kernel/platform/pc/debug_console.cpp`, new `kernel/platform/pc/timer.cpp`, `kernel/platform/pc/BUILD.gn`, `docs/changes/261029/logz4.md`
- Step 1a.4: `kernel/kernel_params.gni`, `kernel/top/BUILD.gn`, `kernel/products/BUILD.gn`, `kernel/top/main.cpp`, `kernel/top/crash_record.cpp`, and whatever else the switch shows still needs the user ABI, `docs/changes/261029/build.md`
- This plan, updated as each step lands.

## Test plan
- After every commit: `python3 .agents/skills/ours-verify/scripts/boot_check.py` (x86: dbg and rel PASS, host tests PASS), and `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/bringup.riscv` (fake kernel `handoff ok`).
- Step 1a, in a riscv probe build directory under `out/` named with `s1a-e7ee`, removed at the end: `ninja -k 0` on `kernel_riscv.dbg.1/phony/kernel/products/bin/vmours`, failing compiles counted before and after; every generic compile command checked for an `-I` into `kernel/arch/x86`; `clang -H` on `kernel/top/main.cpp` and two other generic objects, no file under `kernel/arch/x86` opened.
- Step 1a.4: x86 configured with `kernel_no_userabi=true` compiles, links and boots to where userboot would start; what it prints is recorded in the change record.
