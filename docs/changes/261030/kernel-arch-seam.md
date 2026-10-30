# kernel-arch-seam changes

## Title: x86's headers stop declaring three functions nothing uses (2026-10-30)

**Files**
- `kernel/arch/x86/kernel/include/ours/arch/x86/time.hpp` — the declarations of `ours::time::hpet_init()` and `ours::time::pit_init()` are gone, and so is the `<ours/status.hpp>` include only they needed
- `kernel/arch/x86/kernel/include/ours/arch/x86/mp.hpp` — the declaration of `arch_mp_reschedule(CpuNum)` is gone
- `kernel/arch/x86/kernel/mp.cpp` — the definition of `arch_mp_reschedule(CpuNum)` is gone; its comment now heads `arch_mp_reschedule_mask()`, which does the same per CPU
- `kernel/arch/x86/kernel/include/ours/arch/idle_thread.hpp` — `ArchIdleThread::wake()`'s comment names `arch_mp_reschedule_mask()` as its caller
- `.agents/skills/ours-kernel-design/references/task.md` — the idle-wake rule names `arch_mp_reschedule_mask()`
- `docs/repairs/261029/kernel-arch-seam.md` — deleted: its only item is this one

**Symptom**
Nothing at run time. A reader of x86's headers found three functions that looked like part of the timer and IPI set-up but were not. `ours::time::hpet_init()` and `ours::time::pit_init()` were declared and defined nowhere. `arch_mp_reschedule(CpuNum)` was defined and called from nowhere, and its `arch_` prefix is the generic seam's (`ours/arch_mp.hpp`), so a riscv porter reading x86's header could take it for part of the seam and write a riscv body nothing calls.

**Cause**
All three are leftovers. The two `ours::time` declarations have existed since the oldest commit that has the file (`1387a1a3`, 2026-04-16, then `kernel/arch/x86/main/include/ours/arch/time.hpp`) and no version of the tree ever defined them: the HPET is set up by `ours::hpet_init(PhysAddr)` (`ours/arch/hpet.hpp`, `time/hpet.cpp`), a different function in another namespace that the platform's init hook `platform_hpet_init()` (`kernel/platform/pc/hpet.cpp`) calls with the address it reads from ACPI's HPET table, and the PIT by `setup_pit()` (`time/time.cpp`). C++ overloads by namespace and parameters, so a declaration with the same name and no definition links fine as long as nobody calls it, and the compiler never complains. `arch_mp_reschedule(CpuNum)` lost its last caller in `85a8fb69` (2026-09-10), when `IpiService::send_resched(CpuNum)` began delegating to the mask form, which runs the same monitor-word check per CPU. The arch-seam move (`1cc87400`) carried all three over unchanged instead of deleting them, because `ours-kernel-design` asks before an uncalled function is removed: it may be scaffolding for planned work. The user decided on 2026-10-30 that none of them is.

**Change**
The three are deleted, with the comments that named `arch_mp_reschedule()`. Ruled out: renaming `arch_mp_reschedule()` to `x86_mp_reschedule()` and keeping it, which keeps a function nothing calls; keeping the two `ours::time` declarations for a planned split of the timer set-up, which no plan contains.

**Reference**
None, this project's own cleanup. Zircon's generic seam has only the mask form, `arch_mp_reschedule(cpu_mask_t)` (`zircon/kernel/include/arch/mp.h`), with no single-CPU one.

**Verification**
- `git grep -n "hpet_init()\|pit_init()\|arch_mp_reschedule("` over `kernel/` finds only comments naming the real `hpet_init(PhysAddr)`.
- `llvm-nm -C` of the x86 dbg vmours has no `arch_mp_reschedule(` symbol.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.8s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_cpp.py --check` clean on the four C++ files; `clang-tidy` reports nothing in `mp.cpp`.

**Deliberately not done**
None.

**How it was found**
The review of riscv-kernel step 1a, which saw the arch-seam move carry the three over, filed them as `docs/repairs/261029/kernel-arch-seam.md` item 1 for the user.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a declaration with no definition link without an error, and when would it stop linking?
- Why were `ours::time::hpet_init()` and `ours::hpet_init(PhysAddr)` never in conflict?
- What does the mask form of the reschedule do for each CPU in the mask before it sends any IPI?

## Title: x86's seven private headers leave `ours/arch/` for `ours/arch/x86/`, so the directory holds only what every architecture provides (2026-10-30)

**Files**
- `kernel/arch/x86/kernel/include/ours/arch/{apic,asm,cpu,hpet,hpet_regs,mem_cfg,numa}.hpp` — moved with `git mv` to `ours/arch/x86/`, beside the twelve headers already there; their include guards follow the path (`OURS_ARCH_APIC_HPP` becomes `OURS_ARCH_X86_APIC_HPP`, and so on), as every header in `ours/arch/x86/` already does
- `kernel/arch/x86/apic/{io_apic,ipi,local_apic}.cpp`, `kernel/arch/x86/kernel/{faults,init,mp}.cpp`, `kernel/arch/x86/kernel/time/{hpet,time}.cpp`, `kernel/platform/pc/{hpet,interrupt,memory}.cpp`, `kernel/commands/{interrupt_line_test,irq_test,timer_share_test}.cpp` — the includes name the new paths, and re-sorted into place where the new spelling sorts differently (`init.cpp`, `mp.cpp`, `memory.cpp`)
- `kernel/arch/x86/kernel/{bootstrap16,context,exception,gdt,idt,start,syscall,user_copy,uspace}.S`, `kernel/arch/x86/phys/{gdt,legacy_multiboot_start,start64}.S` — the twelve assembly files that include `asm.hpp`
- `kernel/arch/x86/kernel/BUILD.gn` — the `headers` target's one `public` entry names `ours/arch/x86/hpet_regs.hpp`
- `.clang-format-ignore` — `asm.hpp` is assembler wearing a `.hpp` extension, and the ignore list names it by path
- `kernel/arch/x86/kernel/include/ours/arch/x86/time.hpp`, `kernel/lib/acpi/include/acpi/structures/hpet.hpp` — comments that name `hpet.hpp` and `hpet_regs.hpp`
- `docs/repairs/260903/x86-clock.md`, `docs/repairs/261030/x86-apic.md`, `.agents/tasks/irq-genirq/PLAN.md`, `.agents/tasks/riscv-kernel/PLAN.md` — live documents that name the old paths

**Symptom**
Nothing at run time. A reader of `kernel/arch/x86/kernel/include/ours/arch/` saw thirteen headers and could not tell which are the architecture's contract and which are x86's own. Five are the contract (`aspace_layout.hpp`, `cpu_local.hpp`, `entry.hpp`, `idle_thread.hpp`, `thread.hpp`): generic code includes them by that name and expects every architecture to have one. The other seven (and `vm_aspace.hpp`, a migration stub nothing includes) exist only on x86. A riscv porter who copied the directory's list would write thirteen headers where five are needed. The riscv probe showed the other face of it: three generic-looking files in `kernel/commands` include `<ours/arch/apic.hpp>`, and on riscv that failed with `file not found` for a name that looked like the contract's.

**Cause**
`<ours/arch/thread.hpp>` is not a path to one file. Every compile of the kernel gets `-I kernel/arch/<cpu>/kernel/include` from the toolchain (`build/switch/kernel_switchset.gni`, `prefix_configs`, which lists `//kernel/arch/$cpu/kernel:headers.config`; `build/config/BUILDCONFIG.gn` puts those configs ahead of every target's own). So the directory `ours/arch/` is one *name* that each architecture fills with its own files: x86's `include/` holds one `ours/arch/thread.hpp`, riscv's would hold another, and the include line in generic code means "the running architecture's". That is a contract, and the contract is the set of names: if an architecture lacks one of them, the generic include fails with `file not found` on that architecture, which is the right way for a missing piece to announce itself.

A header only x86 has, placed in the same directory, has the same address shape as a contract member and none of the meaning. Nothing in the path says "this one is x86's". `apic.hpp` was included from generic `kernel/commands`, and on x86 it resolved; on riscv it failed with the same error a missing contract header gives, so a reader could not tell "riscv has not written this yet" from "this never belonged to the contract". The mistake is that one directory carried two different promises, "every architecture provides this" and "this is private to the architecture", and the second was only a convention of the author's memory. `ours/arch/x86/` already existed for the second promise: a path with `x86/` in it can only resolve while x86's include root is on the command line, so the name carries the fact, and a generic file that includes it fails on another architecture with a name that says what is wrong.

**Change**
The seven files moved with `git mv` (git records them as renames); every include line, in `.cpp` and `.S` alike, takes the new spelling. Before and after, for the one that matters most to a porter:

```cpp
// before
#include <ours/arch/apic.hpp>
// after
#include <ours/arch/x86/apic.hpp>
```

The include root is the same `-I` in both, so nothing about the build line changes; only the path under it does. Left in `ours/arch/`: the five contract headers, and `vm_aspace.hpp`, a migration stub that is scaffolding and is neither moved nor deleted.

The guards were renamed with the files. Every header already in `ours/arch/x86/` derives its guard from its path (`OURS_ARCH_X86_PERCPU_HPP`), and a guard that keeps the old path misleads the way the old directory did. No other file named any of the old guards.

Two files that look related were left as they are. `hpet_regs.hpp` still opens `namespace ours::arch`, which R6 of `arch-platform-seams` renames. `kernel/lib/acpi/include/acpi/structures/hpet.hpp` is a generic firmware-table header that only mentions `hpet_regs.hpp` in a comment, and its comment now names the new path; whether the ACPI library should know about an x86 register header is the P-fw question.

**Reference**
Zircon keeps its x86-private headers under `arch/x86/` and the cross-architecture ones in `arch/`: `zircon/kernel/arch/x86/include/arch/x86/apic.h` and `zircon/kernel/arch/x86/include/arch/x86/asm.h` are the counterparts of the two largest moved headers, while `zircon/kernel/include/arch/ops.h` and `arch/thread.h` are the generic side. This project shares one `ours/arch/` name between the generic kernel and each architecture (`ours-kernel-design`, `layering.md`: "Architecture, platform and drivers"), so the sub-directory is what separates the two kinds.

**Verification**
- `git grep -n "ours/arch/\(apic\|asm\|cpu\|hpet\|hpet_regs\|mem_cfg\|numa\)\.hpp"` finds only `docs/changes/` history and `.agents/tasks/arch-platform-seams/DESIGN.md`.
- `git diff -M --stat` shows the seven as renames.
- The x86 dbg `vmours` and `vmours.with-tests` have the same `.text` before and after (`llvm-objcopy --dump-section .text`, sha256 prefix `8c224710bf1e37dd` and `a4537ef1107cff4a`), and the same `.rodata` and `.data`: no moved header embeds `__FILE__`.
- riscv probe, `ninja -k 0` on `kernel_riscv.dbg.1/obj/kernel/products/bin/vmours`: 125 failing compiles before and after. The same three compiles (`interrupt_line_test`, `irq_test`, `timer_share_test`) fail on `apic.hpp`, and their message now names `ours/arch/x86/apic.hpp`.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.8s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_cpp.py` clean on the touched C++ files; `clang-tidy -p out/bringup.x86` on the fourteen touched `.cpp` files reports no error and nothing on a changed line.

**Deliberately not done**
- `vm_aspace.hpp` stays in `ours/arch/`: it is scaffolding for a planned home of `ArchVmAspace`, never to be deleted or moved without asking.
- `entry.hpp` stays too, though it is reworked by a later item.
- `hpet_regs.hpp`'s `ours::arch` namespace (R6) and the ACPI library's comment about an x86 header (P-fw) are other items.
- The `.S` files' include order is not sorted; they never were, and an assembly include order can matter.
- `GN` lists every one of these headers in `//kernel/arch/x86/kernel:headers`: the next commit.
- Old change records and `arch-platform-seams/DESIGN.md` still name the old paths; they are history and a plan backfilled in one pass.

**How it was found**
`arch-platform-seams` I6 counted the directory (five contract headers, seven private ones, one stub) and R8 was approved on 2026-10-30. The list of includers came from `git grep` over every file type, not only `.cpp`/`.hpp`: the assembly files, `BUILD.gn`, `.clang-format-ignore` and four documents carried the old paths too.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `#include <ours/arch/thread.hpp>` compile on x86 and fail on riscv without anyone editing the include line, and what in the build decides which file it means?
- A generic file includes `<ours/arch/x86/apic.hpp>`. What does a riscv build say, and why is that better than what `<ours/arch/apic.hpp>` said?
- `ours/arch/x86/cpu.hpp` and the generic `ours/cpu.hpp` have similar names. What keeps their include guards apart, and what does the compiler do with a second header that happens to reuse a guard already defined?
