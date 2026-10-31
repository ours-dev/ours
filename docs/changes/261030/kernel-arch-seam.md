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

## Title: x86's `headers` target lists the 25 headers of its include root instead of one, so GN knows which target owns them (2026-10-30)

**Files**
- `kernel/arch/x86/kernel/BUILD.gn` — the `public` list of `cpp_interface("headers")` goes from `hpet_regs.hpp` alone to every header under `include/ours/arch/` (six) and `include/ours/arch/x86/` (nineteen), in three groups: the five headers of the per-arch set, `vm_aspace.hpp` (a stub, not part of the set) alone, then x86's own, each group alphabetical. A comment above the target says what the list is for, and one above each group says what the group is.

**Symptom**
Latent, cannot be triggered today: nothing fails at build or run time. `gn desc` of the target listed one header while the include root held twenty-five (`find kernel/arch/x86/kernel/include -type f` finds 25), so twenty-four headers belonged to no target. It would show the day someone runs `gn check --check-system`, or an IDE indexes the target: an include of any of the twenty-four was invisible to the checker, and the indexer found one file.

**Cause**
`cpp_interface()` (`build/cpp/cpp_interface.gni`) makes a `source_set` with no sources, so there is nothing to compile; what it forwards is the `public` list, and it publishes `include/` as an include directory to dependents. The ninja file of the target says nothing about `public`: `kernel_x86.dbg.1/obj/kernel/arch/x86/kernel/headers.ninja` holds an `include_dirs` line and two `phony` edges, and no header name. So the list is metadata for GN, not an input of the build, and a header missing from it cannot make a compile fail. That is why it stayed at one entry while the directory grew: the target was made in `feec7b72` (2026-08-24, the Bazel-to-GN checkpoint) with one entry, the hyphenated `hpet-regs.hpp`, and `25c50029` (2026-08-30) only repointed that entry at the renamed `hpet_regs.hpp`. Nothing in either commit says why that header and not the others, and no compile ever objected.

What the list is for is `gn check`, and two of its rules decide whether a gap matters (`gn help check`):
- It only checks an include that names a file some target lists ("includes matching unknown paths are ignored"). A header owned by no target can be included from anywhere with no dependency at all, and nothing says so.
- It reads `"quoted"` includes always and `<angle>` includes only with `--check-system`. The kernel includes its architecture headers with angle brackets everywhere: 71 files have `#include <ours/arch/...>` and none has the quoted form. So a plain `gn check` never looked at these includes even when the header was listed, which is why it passed before and passes after.

The listing is also not what makes the headers findable. The toolchain puts `-I kernel/arch/<cpu>/kernel/include` on every kernel compile (`build/switch/kernel_switchset.gni`, `build/toolchain/ours/BUILD.gn`: `headers.config` in the prefix configs), so `<ours/arch/thread.hpp>` resolves in a target that never depends on `:headers`. Ownership and findability are two separate facts; this target had the second for every header and the first for one.

riscv's target was already complete in the same sense: its one entry, `aspace_layout.hpp`, is every header riscv's include root has. x86's was the one that fell behind.

**Change**
Before:

```gn
cpp_interface("headers") {
  public = [ "include/ours/arch/x86/hpet_regs.hpp" ]
```

After (the paths are the real ones, relative to the BUILD.gn file, which is the `public` spelling the target already used):

```gn
cpp_interface("headers") {
  public = [
    # ours/arch/ is the per-arch set: ...
    "include/ours/arch/aspace_layout.hpp",
    "include/ours/arch/cpu_local.hpp",
    "include/ours/arch/entry.hpp",
    "include/ours/arch/idle_thread.hpp",
    "include/ours/arch/thread.hpp",

    # Not part of that set: a migration stub ...
    "include/ours/arch/vm_aspace.hpp",

    # ours/arch/x86/ is private to x86: ...
    "include/ours/arch/x86/apic.hpp",
    "include/ours/arch/x86/asm.hpp",
    "include/ours/arch/x86/bootstrap.hpp",
    "include/ours/arch/x86/cpu.hpp",
    "include/ours/arch/x86/descriptor.hpp",
    "include/ours/arch/x86/entry.hpp",
    "include/ours/arch/x86/faults.hpp",
    "include/ours/arch/x86/feature.hpp",
    "include/ours/arch/x86/hpet.hpp",
    "include/ours/arch/x86/hpet_regs.hpp",
    "include/ours/arch/x86/idt.hpp",
    "include/ours/arch/x86/init.hpp",
    "include/ours/arch/x86/io_bitmap.hpp",
    "include/ours/arch/x86/mem_cfg.hpp",
    "include/ours/arch/x86/mp.hpp",
    "include/ours/arch/x86/numa.hpp",
    "include/ours/arch/x86/page_table.hpp",
    "include/ours/arch/x86/percpu.hpp",
    "include/ours/arch/x86/time.hpp",
  ]
```

The groups mirror the two promises the previous entry separated, so the list itself shows which headers every architecture provides and which are x86's. `vm_aspace.hpp`, the migration stub, has a group of its own: it sits in `ours/arch/` but is not part of the set (nothing includes it, and the x86 virtual-address-space interface will replace it), and a list that put it beside `thread.hpp` would say otherwise. The first comment is also worded as what the set is, not as a promise riscv already keeps: riscv's own `headers` target lists only `aspace_layout.hpp`, because that is all riscv has written so far. `page_table.hpp` was already named by the target's `public_deps` comment ("whoever includes it needs that library's headers"), which only made sense for a header the target owned; now it does.

Rejected: listing only the five contract headers, which would leave x86's own includers (the `//kernel/arch/x86/apic` target includes `ours/arch/x86/apic.hpp` and `feature.hpp`) as unchecked as before; and `headers = [ "ours/arch/..." ]`, which gives the same files with shorter lines but changes the spelling the target used without gaining anything.

**Reference**
None, this project's own choice. Zircon's `zircon/kernel/arch/x86/BUILD.gn` declares `library_headers("headers")` with `headers = []` and lets the include path carry everything, so it does not list its headers at all. Here the listing is kept for the reason `docs/changes/261028/riscv-paging.md` gives for riscv's: `gn check` and an IDE can only work with headers a target owns.

**Verification**
- Before the change, at `a63e4664`: the x86 `ours-verify` (dbg 44 passed, 4 skipped; rel 41 passed, 8 skipped; host-tests 153 passed) and the riscv check (handoff ok) gave the pass counts the previous entry records, and the x86 dbg `vmours` and `vmours.with-tests` hashed the same (`llvm-objcopy --dump-section`, sha256 prefix): `.text` `8c224710bf1e37dd` and `a4537ef1107cff4a`, `.rodata` `339ce0defeee43f2` and `beafba3cbeccc3a6`, `.data` `6b949fe14e37ba92` and `68d80ba4bacc5723`. The previous commit's claims reproduce.
- `gn desc out/bringup.x86 '//kernel/arch/x86/kernel:headers(//build/switch/set/dbg.1:kernel_x86.dbg.1)' public` lists the 25 files. (The bare label matches no target: the kernel is built in variant toolchains.)
- `gn check out/bringup.x86` and `gn check out/bringup.riscv`: `Header dependency check OK`, before and after.
- `gn check --check-system --error-limit=1000000`, which already fails before the change on both architectures. riscv: 85 error lines before and after (its graph does not read x86's BUILD.gn, and its `build.ninja` was not even regenerated). x86: 91 error lines before and 143 after. GN prints an error once for each toolchain variant that builds the file (`kernel_x86.dbg.0` and `kernel_x86.dbg.1`), so counted as distinct (file, line, include) pairs that is 44 before and 70 after, and the 52 new lines are 26 new pairs, none gone. 25 of them are an `Include not allowed` whose owner is `//kernel/arch/x86/kernel:headers`, and one is a different error, `Can't include this header from here` (`//kernel/lib/userabi/src/userboot.cpp:19`, `<ours/arch/aspace_layout.hpp>`): there `//kernel/lib/userabi:userabi` does reach `:headers`, but through a dependency that is not `public`, so GN says to depend on the target directly or have the intermediate one forward it. By kind the pairs go from 39 to 64 `Include not allowed` and from 5 to 6 `Can't include this header from here`. By header: `ours/arch/aspace_layout.hpp` 16, `ours/arch/x86/apic.hpp` 6, and one each of `ours/arch/entry.hpp`, `ours/arch/x86/feature.hpp`, `ours/arch/x86/io_bitmap.hpp`, `ours/arch/x86/percpu.hpp`. By includer: fifteen in `//kernel/commands:commands`, four in `//kernel/arch/x86/apic:apic`, three in `//kernel/object:object`, and one each in `//benches/kernel/mm:large-page-bench`, `//kernel/kernel/irq:irq`, `//kernel/lib/user_copy:user_copy` and `//kernel/lib/userabi:userabi`.
- The build did not change. After `gn` regenerated `out/bringup.x86`, all 711 `.ninja` files are byte-identical to the copies taken before, and `compile_commands.json` has the same 752 entries as a set (the file's order is not stable). riscv: 707 `.ninja` files identical, 638 entries.
- The same six section hashes as above after the change, so the x86 dbg `vmours` and `vmours.with-tests` are unchanged in `.text`, `.rodata` and `.data`.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.6s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_gn.py` reports the file already formatted; the list is in the order written.

**Deliberately not done**
- No `deps = [ "//kernel/arch/x86/kernel:headers" ]` was added to the 26 includers above, and `--check-system` was not made a gate. It fails on 44 pairs before this change, so it was never one. The 26 are three kinds, and only the first two are the user's call:
  - Seventeen are in generic targets (`//kernel/commands`, `//kernel/object`, `//kernel/lib/user_copy`, `//kernel/kernel/irq`, `//benches/kernel/mm:large-page-bench`, `//kernel/lib/userabi`) including a header of the per-arch set (`ours/arch/aspace_layout.hpp` sixteen times, `ours/arch/entry.hpp` once). They cannot name x86's label without becoming x86-only, and the other way to spell it, `//kernel/arch/$ours_cpu/kernel:headers`, is a design choice about how generic code declares that it needs the architecture's headers. The `userabi` one is the odd one: that target already reaches `:headers` through a dependency that is not `public`, so it needs the same answer as the other sixteen, not a different one.
  - Five are in four `kernel/commands` tests that include x86's private headers (`apic.hpp` three times, `io_bitmap.hpp` and `percpu.hpp` once, the last two in `ioport_test.cpp`): the leak the previous entry already names, which a dependency would paper over. Where such a test belongs is the user's call.
  - Four are x86's own `//kernel/arch/x86/apic:apic` including `apic.hpp` and `feature.hpp`. Neither argument above applies to them: an x86 target can name x86's label. They are not a design question, and the next entry adds the dependency.
- riscv's `headers` target is unchanged: its list is already its whole include root.
- No header was edited, moved or deleted; `vm_aspace.hpp` stays the stub it is.
- The other `cpp_interface` targets of x86 (`//kernel/arch/x86:headers`, `//kernel/arch/x86/mm:headers`, `//kernel/arch/x86/lib/page_tables`) were not audited for the same gap.

**How it was found**
The previous entry promised it as "the next commit". `find kernel/arch/x86/kernel/include -type f` gave 25 files and `gn desc` of the target gave one. `gn help check` gave the two rules above, and running `gn check --check-system` on both build directories before touching anything gave the baseline (91 and 85 error lines) that the 52 new lines on x86 (26 pairs times two toolchain variants) are measured against. A grep for `#include "ours/arch` (no hits) against `#include <ours/arch/` (71 files) is what showed that a plain `gn check` never reads these includes.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Listing 24 more headers changed not one byte of any `.ninja` file or of the kernel's `.text`. Why not, and what did it change?
- Why does a plain `gn check` read none of the `#include <ours/arch/...>` lines, and what does `--check-system` do with them that it did not do before this change?
- `//kernel/object` includes `<ours/arch/aspace_layout.hpp>` and compiles without depending on x86's `:headers`. What puts the directory on its command line, and why would a `deps` line naming x86's label in that target be the wrong fix?

## Title: x86's `apic` target depends on x86's `headers` target, because two of the headers it includes are owned there (2026-10-30)

**Files**
- `kernel/arch/x86/apic/BUILD.gn` — `//kernel/arch/x86/kernel:headers` joins the `deps` of `source_set("apic")`, with a comment naming the two headers that need it.

**Symptom**
Latent, cannot be triggered today: nothing fails at build or run time. `gn check --check-system` on x86 reported four `Include not allowed` errors for this target, which the previous entry's listing of the 25 headers had made visible: `io_apic.cpp:1`, `ipi.cpp:1` and `local_apic.cpp:1` include `<ours/arch/x86/apic.hpp>`, and `local_apic.cpp:2` includes `<ours/arch/x86/feature.hpp>`. Each says "It is not in any dependency of `//kernel/arch/x86/apic:apic`" and names `//kernel/arch/x86/kernel:headers` as the owner.

**Cause**
The previous entry separated two facts: a header is *findable* (the toolchain puts x86's include root on every kernel compile's `-I`, so `apic.cpp` compiles whatever `deps` says) and a header is *owned* (a target lists it in `public`, and `gn check` then wants an edge from every includer to that target). Before the listing, `apic.hpp` and `feature.hpp` had no owner, so the check had nothing to hold the `apic` target to, and its `deps` list, written without the check in view, never named the owner. After the listing the missing edge is a finding.

The reason the previous entry gave for leaving the includers alone does not reach this one. That reason is about *generic* code: `//kernel/object` or `//kernel/commands` cannot write `//kernel/arch/x86/kernel:headers` without becoming x86-only, so how generic code asks for "the running architecture's headers" is a design question. `//kernel/arch/x86/apic` is not generic. It sits under `kernel/arch/x86/`, it is only loaded in x86's graph, and its sources include x86's own headers; a dependency that says "x86's headers" states something true about it. There is no question to put to the user, only an edge that was never written.

The edge cannot make a cycle: `:headers` has one `public_deps`, `//kernel/arch/x86/lib/page_tables`, which forwards `//kernel/arch/x86:headers`, libarch's headers and ustl's, and none of them depends on `apic`. `gn gen` would refuse a cycle, and it did not.

Six other errors disappeared with it, and they are worth understanding. Those six were already failing before the previous entry (they are among its 44 baseline pairs): `local_apic.cpp` includes `arch/interrupt.hpp`, `arch/spin_hint.hpp` and `arch/tick.hpp`, owned by `//kernel/lib/arch:headers`, and `arch/x86/apic/xapic.hpp` and `arch/x86/msr.hpp`, owned by `//kernel/arch/x86:headers`; `io_apic.cpp` includes `arch/x86/apic/ioapic.hpp`, owned by the same. `apic` had an edge to neither owner. `public_deps` is GN's forwarding: a target that lists `public_deps = [ X ]` lets its dependents include X's headers as if they had depended on X directly. So `apic` depends on `:headers`, which forwards page_tables, which forwards both of those owners, and the check is satisfied through that chain.

**Change**
Before:

```gn
    deps = [
      "//kernel/lib/mtl:headers",
```

After:

```gn
    deps = [
      # ours/arch/x86/apic.hpp and feature.hpp, which the sources include.
      "//kernel/arch/x86/kernel:headers",
      "//kernel/lib/mtl:headers",
```

The edge goes first in the list, ahead of the other headers targets; the list is otherwise in the order it was written.

**Reference**
None, this project's own graph. Zircon's `zircon/kernel/arch/x86/BUILD.gn` declares `library_headers("headers")` with `headers = []` (line 264) and lists nothing for the check to hold an includer to, so it has no such edge to write; here the listing is kept (previous entry), and with it comes the edge.

**Verification**
- `gn check out/bringup.x86` and `gn check out/bringup.riscv`: `Header dependency check OK`.
- `gn check --check-system --error-limit=100000`, parsed into distinct (file, line, include) pairs as in the previous entry. x86: 143 error lines and 70 pairs before, 123 and 60 after. The ten that went are the four above and the six that the forwarding covers; no pair is new. The expected figure was 135 lines (the four pairs times two toolchain variants), and the extra twelve lines are those six pairs. riscv: 85 lines before and after, because `apic` is not in its graph.
- The build did not change. After `gn gen`, 709 of the 711 x86 `.ninja` files are byte-identical; the two that differ are `apic.ninja` in `kernel_x86.dbg.0` and `kernel_x86.dbg.1`, in one line each: the order-only `linkdeps` phony edge of `apic` now also lists the `linkdeps` of `:headers`, page_tables, x86's headers and libarch's headers. No compile line changed (`compile_commands.json` has the same 752 entries as a set), so nothing recompiled (`ours-verify` build ok in 0.5s).
- `.text`, `.rodata` and `.data` of the dbg `vmours` (`kernel_x86.dbg.1`) and `vmours.with-tests` (`kernel_x86.dbg.0`) hash the same before and after (`llvm-objcopy --dump-section`, sha256 prefix): `.text` `8c224710bf1e37dd` and `a4537ef1107cff4a`, `.rodata` `339ce0defeee43f2` and `beafba3cbeccc3a6`, `.data` `6b949fe14e37ba92` and `68d80ba4bacc5723`.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_gn.py --check` reports the file already formatted.

**Deliberately not done**
- The 22 other new pairs of the previous entry (seventeen in generic targets, five in `kernel/commands` tests including x86's private headers) are untouched: they are the user's call, and this edge says nothing about them.
- `//kernel/lib/arch:headers` and `//kernel/arch/x86:headers` are not named in `apic`'s `deps`. The six includes they own are satisfied through `:headers` forwarding page_tables forwarding both, which holds only while page_tables keeps forwarding them; a direct edge to each would be sturdier. It is a separate change with a separate reason, and forwarding is what GN's `public_deps` is for, so it was left.
- The other x86 targets were not audited for an include they own no edge to. `--check-system` reports no other x86-internal includer, which is the check there is, and it still skips any header no target lists.

**How it was found**
An independent review of the previous entry grouped the 26 new `--check-system` pairs by includer target and noticed that one includer, `//kernel/arch/x86/apic:apic`, is x86's own, so the entry's argument for deferring the generic ones did not cover it. `gn check --check-system` on the tree with this edge confirmed the four went and showed the six more.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `//kernel/object` includes `<ours/arch/aspace_layout.hpp>` and has the same missing edge as `apic`. Why is the one-line fix right for `apic` and not for `object`?
- The edge added one line to each `apic.ninja` and no compile line changed. Which edge is it, and why did ninja recompile nothing?
- Six errors about headers other than x86's own disappeared when `apic` began to depend on x86's `:headers`. What carried them, and what would break the cover if page_tables stopped forwarding its two owners?

## Title: A correction to the `ours/arch/x86/` entry: Zircon is the one that shares the `arch/` name, the guard rule has one exception, the grep was narrower than it said, and the guard question is now taught (2026-10-30)

**Files**
- `docs/changes/261030/kernel-arch-seam.md` — this entry only. The entry "x86's seven private headers leave `ours/arch/` for `ours/arch/x86/`" above is history and is not edited; this one says which of its statements are wrong or incomplete, and what is true.

**Symptom**
None: wrong teaching in a record. Four statements of that entry:
- Its Reference says "This project shares one `ours/arch/` name between the generic kernel and each architecture ..., so the sub-directory is what separates the two kinds." That is the opposite of the project's rule.
- Its Change says "Every header already in `ours/arch/x86/` derives its guard from its path". One does not.
- Its Verification says `git grep` for the old paths finds only `docs/changes/` history and `arch-platform-seams/DESIGN.md`. That is true of one spelling only.
- Its third question asks what the compiler does with a second header that reuses a guard, and no section of the entry teaches it.

**Cause**
*Who shares the name.* `layering.md` ("Architecture, platform and drivers") says `ours/arch/` holds only the headers every architecture provides under the same name, its per-arch set, and that generic seams are declared in `kernel/include/ours/arch_<topic>.hpp`. It is Zircon that lets two directories provide one name: `zircon/kernel/include/arch/` holds the generic half (`ops.h`, `thread.h`, `mp.h`, ...) and each `zircon/kernel/arch/<cpu>/include/arch/` the per-arch half (for x86 `arch_ops.h`, `arch_thread.h`, and the `x86/` sub-directory), and the generic `arch/ops.h` includes `<arch/arch_ops.h>` (line 56), the generic `arch/thread.h` `<arch/arch_thread.h>` (line 15). The user kept the split riscv-kernel step 1a made, which stops two directories from providing one `ours/arch/` name (2026-10-30). What this project shares is the name across *architectures*: x86's include root holds one `ours/arch/thread.hpp`, riscv's would hold another, and only one root is on the command line. The entry's own Cause says exactly that. So here `ours/arch/x86/` does not separate the generic kernel from the architecture; it separates, inside the architecture, the per-arch set from x86's private headers. The error was in the Reference, which described Zircon's shape as if it were ours.

*The guard rule.* `kernel/arch/x86/kernel/include/ours/arch/x86/feature.hpp` opens with `OURS_ARCH_X86_FEATURES_HPP`, plural, where the path rule gives `OURS_ARCH_X86_FEATURE_HPP`. The other eighteen of the nineteen headers in `ours/arch/x86/`, the seven moved ones among them, follow the rule, so "every" was one too many. The mismatch predates R8 and R8 did not touch the file, so it stays: whoever next edits `feature.hpp` should rename the guard and say so in that record. It is not a collision either way: no two headers of x86's include root open with the same guard, and each of the seven new guard names appears in one header only.

*The grep.* The recorded pattern, `ours/arch/\(apic\|asm\|cpu\|hpet\|hpet_regs\|mem_cfg\|numa\)\.hpp`, matches the full spelling of the old paths. Two other spellings remain, both history: the read-only `docs/repairs-20260830.md` (four mentions of `arch/hpet.hpp` without the `ours/`), and `.agents/tasks/acpi-lib-redesign/RESULT.md:26`, which names `ours/arch/hpet-regs.hpp`, the hyphenated spelling the header had until `25c50029` renamed it on 2026-08-30. A result record and an archive say what was true when they were written, so neither was updated. What the grep supports is "no live file names the old path in its full form", not "no file names it".

*The include guard, which the third question asked about.* A header opens with `#ifndef G`, `#define G` and closes with `#endif`. The preprocessor decides by the name `G` alone, not by what the file is or where it lives: once `G` is defined in a translation unit, the body of any later header that opens with the same `G` is skipped, with no warning. If two different headers share a guard, whichever is included second is missing from that translation unit, and the symptom is an undeclared identifier or an incomplete type at some use site, far from the include line and present only in the translation units that include both, in that order. That is why the convention derives the guard from the path: a path is unique inside an include root, so a name built from it is unique with it. `ours/arch/x86/cpu.hpp` gives `OURS_ARCH_X86_CPU_HPP` and the generic `ours/cpu.hpp` gives `OURS_CPU_HPP`, so they cannot shadow each other, whatever their similar names. R8 did not rename the seven guards to cure a collision (there was none: the old `OURS_ARCH_CPU_HPP` and a riscv header of the same path would never share a build), but so that a guard keeps telling the truth about the path, as the old directory did not.

**Change**
The Reference sentence, before:

```text
This project shares one `ours/arch/` name between the generic kernel and each architecture (...), so the sub-directory is what separates the two kinds.
```

What it should have said:

```text
Zircon lets two directories provide the name `arch/`: the generic half in `zircon/kernel/include/arch/` and the per-arch half in `zircon/kernel/arch/x86/include/arch/`. This project kept the split riscv-kernel step 1a made: generic seams are in `ours/arch_<topic>.hpp`, `ours/arch/` is only the per-arch set, and `ours/arch/x86/` is what separates x86's private headers from that set.
```

The guard and grep statements are corrected by the Cause above; no file other than this record changed.

**Reference**
Zircon: `zircon/kernel/include/arch/ops.h` and `thread.h` are the generic half; `zircon/kernel/arch/x86/include/arch/arch_ops.h` and `arch_thread.h` are the per-arch halves they include; `arch/x86/apic.h` and `arch/x86/asm.h`, in the `x86/` sub-directory of that include directory, are the counterparts of the two largest moved headers. This project: `.agents/skills/ours-kernel-design/references/layering.md`, "Architecture, platform and drivers", the bullet on where seams are declared.

**Verification**
- Guard rule, checked over the nineteen headers of `ours/arch/x86/` by deriving `OURS_ARCH_X86_<NAME>_HPP` from each file name and comparing it with the first `#ifndef`: one mismatch, `feature.hpp`. The first `#ifndef` lines of `ours/arch/*.hpp` and `ours/arch/x86/*.hpp` have no duplicate, and `git grep -l` for each of the seven new guard names finds its header and no other file outside `docs/`.
- `git grep -n -E 'ours/arch/(apic|asm|cpu|hpet|hpet_regs|mem_cfg|numa)\.hpp'` finds only `docs/changes/` and `arch-platform-seams/DESIGN.md`. The abbreviated `arch/(apic|hpet|hpet_regs|hpet-regs|mem_cfg|numa)\.hpp` outside `ours/arch/x86/` finds four lines of `docs/repairs-20260830.md`, and `git grep hpet-regs` outside `docs/changes` finds `acpi-lib-redesign/RESULT.md:26`.
- `git log --follow` on `hpet_regs.hpp` shows `25c50029` as the rename from `hpet-regs.hpp`.
- The Zircon paths above exist in the local checkout, and `arch/ops.h:56` and `arch/thread.h:15` hold the two includes named.
- `scripts/lint_docs.py --changed docs/changes/261030/kernel-arch-seam.md`: no violations. Documents only; no source or build file changed, so no boot was run.

**Deliberately not done**
- `feature.hpp`'s guard is not renamed: it is a one-line change in a file R8 did not touch, and a header edit would need its own build and boot for a name nothing else reads. It waits for the next edit of the file.
- The R8 entry, `.agents/tasks/arch-platform-seams/DESIGN.md` (backfilled in one pass), `docs/repairs-20260830.md` and the `acpi-lib-redesign` result are not edited.
- The R8 entry's third question stays where it is; this entry answers it.

**How it was found**
An independent review read the R8 entry against `layering.md`, ran the path rule over the headers of `ours/arch/x86/` and ran `git grep` with broader patterns than the entry's. Each of the four was reproduced from the commands in Verification before it was written down here.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which directories provide the name `arch/` in Zircon and which provide `ours/arch/` here, and what tells generic code from per-arch code in each?
- Two different headers open with the same guard and a `.cpp` includes both. What does the compiler say, and why is the error nowhere near the include line?
- A grep for `ours/arch/hpet.hpp` finds nothing in live files. Which other spellings of a moved header's old path should you also search for, and which of the remaining hits were rightly left alone?

## Title: x86's `time.hpp` comment is re-wrapped to 80 columns, because naming the moved header in it made the line 81 (2026-10-30)

**Files**
- `kernel/arch/x86/kernel/include/ours/arch/x86/time.hpp` — the paragraph above `set_global_clock_event()` breaks one word earlier, so its second line is 73 columns instead of 81 and the third takes `through`. Same number of lines, same words.
- `docs/changes/261030/kernel-arch-seam.md` — this entry.

**Symptom**
Nothing at run time: a comment. The R8 move left one line of 81 columns in a header whose longest line was 77 before it, and no `///` line in the nineteen headers of `ours/arch/x86/` is longer than 80.

**Cause**
The R8 commit changed the path in the comment from `ours/arch/hpet.hpp` to `ours/arch/x86/hpet.hpp`, four characters longer, on a line that had been filled to 77. Nothing objected because nothing here reflows or limits a comment at that width: `.clang-format` has `ColumnLimit: 100` and `ReflowComments: false`, so `clang-format` neither flags a comment of 81 columns nor wraps it, and `format_cpp.py --check` was clean. The 80-column fill is a habit of how these doc comments are written (the plain `//` register notes of `hpet_regs.hpp` already ran to 95 before the move), not a rule a tool holds, so a rename that lengthens a path inside a comment lengthens the line silently. The rename sweep of R8 (`git grep` for every spelling of the old path) is where it should have been caught: a path that grows is a line that may no longer fit.

I scanned every comment line R8 added in `.hpp`, `.cpp` and `.S` files for more than 80 columns: this was the only one.

**Change**
Before:

```cpp
/// Points the legacy timer line, ISA 0, at `device`. hpet_init(PhysAddr)
/// (ours/arch/x86/hpet.hpp) calls it once it has taken IRQ0 from the PIT through
/// the legacy replacement route.
```

After:

```cpp
/// Points the legacy timer line, ISA 0, at `device`. hpet_init(PhysAddr)
/// (ours/arch/x86/hpet.hpp) calls it once it has taken IRQ0 from the PIT
/// through the legacy replacement route.
```

**Reference**
None, this project's own habit: the width the other comments of the directory are filled to.

**Verification**
- `awk 'length>80'` on the file: nothing; its longest line is 75. No `///` line of the nineteen headers in `ours/arch/x86/` is over 80 (checked in Python, line by line).
- `format_cpp.py --check` on the file: clean. `clang-tidy -p out/bringup.x86` on `kernel/arch/x86/kernel/time/time.cpp`, which includes it: no error and no diagnostic in `time.hpp` (the warnings it prints are in other headers and were there before).
- The dbg `vmours` and `vmours.with-tests` hash the same before and after, in `.text`, `.rodata` and `.data` (`llvm-objcopy --dump-section`, sha256 prefix): `.text` `8c224710bf1e37dd` and `a4537ef1107cff4a`. The objects of `hpet.cpp` and `time.cpp` were rebuilt, and the comment has no effect on them.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.6s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.

**Deliberately not done**
- No check was added to hold comments to 80 columns: the width is a habit, and the one place a tool could enforce it (`ColumnLimit`) is 100 for the code's sake.
- The other comments R8 changed are not re-read for wording; only their width was scanned.

**How it was found**
An independent review ran `awk 'length>80'` over the headers R8 touched and compared the one hit with the file before the move.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did `clang-format` and `format_cpp.py --check` both pass an 81-column comment?
- A rename makes a path four characters longer. Which lines of the tree can be affected beyond the ones that fail to compile, and how would you find them?
- The section hashes came out equal for a comment-only edit. What kind of edit to a header would change `.rodata` although no code changed, and why could this one not?

## Title: Seven generic targets depend on `//kernel/arch/$ours_cpu/kernel:headers`, because `gn check --check-system` holds an include of a header that target owns to an edge to it (2026-10-30)

**Files**
- `kernel/commands/BUILD.gn`, `kernel/object/BUILD.gn`, `kernel/lib/user_copy/BUILD.gn`, `kernel/kernel/irq/BUILD.gn`, `kernel/lib/userabi/BUILD.gn`, `kernel/mm/BUILD.gn`, `benches/kernel/mm/BUILD.gn` — `"//kernel/arch/$ours_cpu/kernel:headers"` joins the `deps` of the source set that includes a header of the per-arch set, each with a comment naming the includer and the header. Five of them gain `import("//build/ours/cpu.gni")`, which defines `$ours_cpu` (`user_copy` and `mm` already had it).
- `kernel/mm/BUILD.gn` — also reformatted as a whole: the two imports are in gn's order, `"tests:tests"` is spelled `"tests"` (the same label), and a stray indent and four missing blank lines before comments are fixed. See Cause for why this file could not be committed otherwise.
- `docs/changes/261030/kernel-arch-seam.md` — this entry.

**Symptom**
Latent, cannot be triggered today: nothing fails at build or run time, and a plain `gn check` passes on both architectures before and after. Since the headers target of each architecture lists its include root (the entries above), `gn check --check-system` named every include of one of those headers from a target with no edge to the owner: 22 findings on x86 and 19 on riscv, of which seventeen on x86 and all nineteen on riscv are in generic code (`//kernel/commands`, `//kernel/object`, `//kernel/lib/user_copy`, `//kernel/kernel/irq`, `//benches/kernel/mm:large-page-bench`, `//kernel/lib/userabi` on x86, and `//kernel/mm` on riscv).

**Cause**
The previous entries separated two facts about a header: it is *findable* (the toolchain puts the architecture's include root on every kernel compile's `-I`) and it is *owned* (a target lists it in `public`, and `gn check` then wants an edge from each includer to that target). Findable is why none of these targets ever failed to compile; owned is what the checker reads, and until the headers target listed its files nothing was owned, so nothing was checked. The listing made the missing edges visible, and the edges were left for the user because generic code cannot name x86's label without becoming x86-only. The user approved the answer on 2026-10-30: say "the running architecture's headers" the way the rest of the tree already does.

That spelling is `$ours_cpu` (`build/ours/cpu.gni`): `current_cpu` inside an ours toolchain and `target_cpu` in the default one, so `//kernel/arch/$ours_cpu/kernel:headers` is x86's label in an x86 build, riscv's in a riscv build. The tree already uses it for exactly this kind of edge: `//kernel/phys` depends on the same label for `kernel-boot.hpp`'s layout constants, and `//kernel/lib/ccrt` and `//kernel/lib/user_copy` depend on `//kernel/arch/$ours_cpu:headers`. The toolchain uses the same variable to put the include root on the command line (`build/switch/kernel_switchset.gni`, `//kernel/arch/$cpu/kernel:headers.config` in `prefix_configs`); the new edge is the second half of that fact, the part GN can check.

Three things the numbers taught, none of which the task's brief had:

- **The list is seven targets, not six.** `//kernel/mm` is the seventh, and only riscv reports it. On x86 `//kernel/mm:mm` depends on `//kernel/arch/x86/mm`, which has `//kernel/arch/x86/kernel:headers` in its `public_deps`, so the check passed there; riscv's `//kernel/arch/riscv/mm` has no such edge, so the same four includes (`vmm.cpp`, `init_vmm.cpp`, `vm_aspace.cpp`, `cpu_local.cpp`, all `aspace_layout.hpp`) failed. A generic target that passes on one architecture because that architecture's own target happens to forward the header is passing by accident, which is what the generic edge replaces. Running the check on both build directories and taking the union is what found it.
- **GN checks per target, not per header, so the edge hides the five x86-private includes in `//kernel/commands`.** The brief expected `<ours/arch/x86/apic.hpp>` (three tests), `io_bitmap.hpp` and `percpu.hpp` (`ioport_test.cpp`) to stay flagged until R25 moves those tests. They cannot: `//kernel/commands` now depends on the target that owns all 25 headers, and `gn check` asks only whether the includer's target reaches *a* target that lists the header, so every one of the 25 passes. All 22 findings owned by x86's headers target are gone, the five included. The tests are still where they were and still fail to compile on riscv (the probe below), which is now the only thing that says so. The comment on the new edge in `kernel/commands/BUILD.gn` says this, so a reader of the file does not take the passing check for a clean target.
- **GN does not read header-to-header includes here.** `kernel/lib/heap/include/heap/vmalloc.hpp` includes `<ours/arch/aspace_layout.hpp>` and is owned by `//kernel/lib/heap:headers`, which has no edge to the architecture's headers, and neither architecture reports it. `cpp_interface()` sets `configs = []` (a header target must not inherit the binary configs), so `gn desc` of `//kernel/lib/heap:headers` on riscv lists no `kernel/arch/riscv/kernel/include` among its include directories while the source set `//kernel/lib/heap:heap` lists it; that is consistent with GN resolving an `<angle>` include through the including target's own include directories, so that from a header-only target it finds no file to check (not tested beyond this one header). Only `.cpp` files of ordinary targets are held to an edge. That is why the seven edges go on source sets and not on the `headers` targets next to them.

`//kernel/lib/userabi` needs the edge spelled out even though it reaches the headers: GN's chain for it was `userabi --> //kernel/mm:mm --[private]--> //kernel/arch/x86/mm:mm --> //kernel/arch/x86/kernel:headers`, and a dependency reached through someone's private `deps` is not forwarded. That is the "Can't include this header from here" kind of error, where the other 21 were "Include not allowed". `//kernel/mm` now depends on the headers directly, and still privately, so `userabi` is still flagged without its own edge: with that one line removed, GN's chain became `userabi --> //kernel/mm:mm --[private]--> //kernel/arch/x86/kernel:headers` and the same `Can't include this header from here` error stayed (the line was put back).

One more thing moved. `//kernel/kernel/irq` lost a second finding that is not about this header: `init.cpp:4`, `<arch/interrupt.hpp>`, owned by `//kernel/lib/arch:headers`. x86's headers target forwards `//kernel/arch/x86/lib/page_tables` publicly, which forwards libarch's headers, so the new edge satisfies it by forwarding, as the previous entry's six did. On riscv the headers target forwards nothing and that finding was not there to begin with.

**Change**
Before and after, for one of the seven (the others differ in the comment only):

```gn
# before
    deps = [
      "//kernel/arch/$ours_cpu:headers",
      "//kernel/mm:headers",
    ]
# after
    deps = [
      "//kernel/arch/$ours_cpu:headers",

      # user_copy.cpp includes <ours/arch/aspace_layout.hpp> for the user half
      # of the address space.
      "//kernel/arch/$ours_cpu/kernel:headers",
      "//kernel/mm:headers",
    ]
```

Each list otherwise keeps the order its file wrote; the new entry sits first in `commands`, `object`, `irq`, `userabi` and the benchmark (after `//benches/kernel:bench`), after `//kernel/arch/$ours_cpu/mm` in `mm`, and after `//kernel/arch/$ours_cpu:headers` in `user_copy`, where `//kernel/phys` puts it too.

The includers and what they take, as GN reported them:

| Target | Includers | Header |
|---|---|---|
| `//kernel/commands` | ten tests (`ioport_test`, `large_page_test`, `page_source_test`, `pager_test`, `user_copy_test`, `vma_phys_test`, `vma_seal_test`, `vma_undo_test`, `vma_wx_test`, `vmo_snapshot_test`) | `aspace_layout.hpp` |
| `//kernel/commands` | `interrupt_line_test`, `irq_test`, `timer_share_test`, `ioport_test` | x86-private: `x86/apic.hpp` (three), `x86/io_bitmap.hpp`, `x86/percpu.hpp` (x86 only) |
| `//kernel/object` | `handle.cpp`, `process_dispatcher.cpp` | `aspace_layout.hpp` |
| `//kernel/object` | `thread_dispatcher.cpp` | `entry.hpp` (x86 only: riscv's list has no `entry.hpp`) |
| `//kernel/mm` | `vmm.cpp`, `init_vmm.cpp`, `vm_aspace.cpp`, `cpu_local.cpp` | `aspace_layout.hpp` (riscv only) |
| `//kernel/lib/user_copy`, `//kernel/kernel/irq`, `//kernel/lib/userabi`, `//benches/kernel/mm:large-page-bench` | `user_copy.cpp`, `init.cpp`, `userboot.cpp`, `large_page_bench.cpp` | `aspace_layout.hpp` |

`kernel/mm/BUILD.gn` needed more than one line to be committable. `scripts/format_gn.py --check` called it `unsafe: the result differs from the source by more than layout` at HEAD already, because `gn format` reorders imports and rewrites `"tests:tests"` to `"tests"`, and the tool refuses to format a file where that is not just layout; the pre-commit hook then reports it as not in gn's layout and fails the commit of anything that touches it. I made the two edits by hand (they are the labels' same meaning) and let the formatter do the rest, which is all layout.

Rejected:
- Naming `//kernel/arch/x86/kernel:headers` in a generic target: x86-only, and riscv's graph would not even have the label.
- Having `//kernel/mm:headers` or `//kernel/object:headers` forward the architecture's headers publicly, so that every dependent gets them: it hides which file needs the header and widens every dependent of two widely used targets for the benefit of the checker.
- `// nogncheck` on the includes: it silences the checker, which is the opposite of what the listing was for.
- Splitting each architecture's headers target into the per-arch set and the architecture's private headers, so that a generic edge could not cover private ones: it keeps the five x86-private includes visible and may be the right shape, but it is a design change to a target the user has just approved the shape of, so it is left as a question (see Deliberately not done).

**Reference**
None, this project's own graph. The header listing and the "findable versus owned" split are the previous entries'; Zircon's `zircon/kernel/arch/x86/BUILD.gn` declares `library_headers("headers")` with `headers = []` and so has no edge for the checker to ask for. The in-tree precedent for the spelling is `kernel/phys/BUILD.gn` (`//kernel/arch/$ours_cpu/kernel:headers`, with the comment "kernel-boot.hpp reaches the kernel's address-space layout constants") and `kernel/lib/ccrt/BUILD.gn` (`//kernel/arch/$ours_cpu:headers`).

**Verification**
- `gn check out/bringup.x86` and `gn check out/bringup.riscv`: `Header dependency check OK`, before and after.
- `gn check --check-system --error-limit=1000000`, parsed into distinct (file, line, include) pairs as in the previous entries (GN prints each finding once for each of the two kernel variants, `kernel_x86.dbg.0` and `.dbg.1`). x86: 123 error lines and 60 pairs before, 77 and 37 after; 23 pairs gone, none new, none changed. riscv: 85 lines and 41 pairs before, 47 and 22 after; 19 gone, none new.
- The 23 gone on x86, by includer: `//kernel/commands` 15, `//kernel/object` 3, `//kernel/kernel/irq` 2, one each in `user_copy`, `userabi` and `large-page-bench`. By owner: 22 owned by `//kernel/arch/x86/kernel:headers` (seventeen generic, five x86-private) and the one forwarded libarch finding above. The 19 on riscv: `//kernel/commands` 10, `//kernel/mm` 4, `//kernel/object` 2, one each in `user_copy`, `irq` and `large-page-bench`; `userabi` is not in riscv's graph (`KERNEL_NO_USERABI`).
- What is left, all of it owned by something else: x86 37 pairs (`//system/lib/logz4:headers` 16, `//kernel/lib/arch:headers` 9, `//kernel/platform:headers` 6, `//system/public:public` 5, `//kernel/arch/x86:headers` 1), riscv 22 (logz4 16, `//system/public:public` 5, platform 1). Owned by an architecture's `kernel:headers`: none, on either. `--check-system` is still not a gate.
- riscv has the target: `//kernel/arch/riscv/kernel:headers` lists `aspace_layout.hpp`, its whole include root. arm has the label too, `cpp_interface("headers")` with `public = []` in `kernel/arch/arm/kernel/BUILD.gn`, so `$ours_cpu` resolves for it; arm does not build and was not generated.
- x86 build, from the `.ninja` files after `gn gen` (711 of them, compared with copies taken before): 20 differ, which is ten targets in each of the two variants. In every one the order-only `linkdeps` edge changes: seven targets gain two to four phony dependencies (the headers target and page_tables, with what they forward), and `//kernel/mm`, `//kernel/lib/syscall` and `//kernel/lib/userabi` list the same ones in a different order. Order-only dependencies decide when ninja may start a step, not what the step runs. In two targets, `//kernel/commands` and `//kernel/kernel/irq`, the `include_dirs` line is also a reordering of the same set of flags: `-Igen/system/public/public` moves ahead of one other directory (`kernel/lib/heap/include` and `kernel/lib/sampler/include`), the position the new edge's forwarded configs give it. That is more than "order-only", so it was checked: the two directories share no relative path with the generated one, so no include can resolve differently; 82 of the 752 compile entries differ, all of them those two targets' sources, and in each only the order of the words; and the 82 objects of those two targets (both variants) are byte-identical to the copies taken before the rebuild.
- riscv: all 707 `.ninja` files are byte-identical, and `compile_commands.json` has the same 638 entries as a set. Its headers target forwards nothing, so the edge adds nothing to any command or order-only list.
- The dbg `vmours` (`kernel_x86.dbg.1`) and `vmours.with-tests` (`kernel_x86.dbg.0`) hash the same before and after (`llvm-objcopy --dump-section`, sha256 prefix): `.text` `8c224710bf1e37dd` and `a4537ef1107cff4a`, `.rodata` `339ce0defeee43f2` and `beafba3cbeccc3a6`, `.data` `6b949fe14e37ba92` and `68d80ba4bacc5723`, the values the previous entries record.
- riscv probe, `ninja -k 0 kernel_riscv.dbg.1/obj/kernel/products/bin/vmours` in a build directory configured for riscv: 125 failing compiles before and 125 after, the same 125 object names (sorted lists compared). The four x86-private tests of `//kernel/commands` are among them, as before.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_gn.py --check` reports all seven files already formatted, the lists in the order written.

**Deliberately not done**
- The four tests that include x86's private headers are not moved or edited (item R25), and nothing replaces the visibility the edge took from them. Where they belong is the user's call: `kernel/arch/x86/commands` (CLAUDE.md's rule for a command that only makes sense on one architecture) is the obvious home, and moving them would put the edge back to being only about generic includes. A path-based check in `ours-verify` (nothing outside `kernel/arch/x86` and `kernel/platform/pc` includes `<ours/arch/x86/...>`) would not depend on which target owns what, and would catch the next one at once; it is not written.
- `kernel/kernel/irq/init.cpp` includes `<ours/arch/aspace_layout.hpp>` and uses nothing from it. The edge records the include as written; deleting an include is a source edit and is left alone.
- No edge on any `headers` target next to these source sets, for the reason in Cause: GN does not read their includes.
- `--check-system` is not made a gate: 37 pairs remain on x86 and 22 on riscv, all owned by other targets.
- The other generic includers of the per-arch set are not touched because they need nothing: `//kernel/phys` and the architectures' own `phys` and `mm` targets already depend on the label, and no other `.cpp` outside `kernel/arch/<cpu>/` includes one of `aspace_layout`, `cpu_local`, `entry`, `idle_thread`, `thread` or `vm_aspace` (`git grep` over the tree, then each hit's owning target checked against the list).
- arm was not generated or built.

**How it was found**
`gn check --check-system --error-limit=1000000` on `out/bringup.x86` and `out/bringup.riscv` before any edit, parsed into (file, line, include, includer target, owner) with a small script, then grouped by includer target. The x86 grouping gave the six targets the brief expected; the riscv one gave the seventh, `//kernel/mm`. A `git grep` of every include of the six per-arch headers (23 `.cpp` files outside the architectures' own directories: the 21 in the seven targets above and two in `//kernel/phys`, which already depends on the label) confirmed nothing else needed an edge. The comparison before and after was `gn gen` into the same build directories, with the `.ninja` files and `compile_commands.json` copied out first. The reordered `-I` was found by classifying each differing line of each differing `.ninja` file as `linkdeps`, `include_dirs` or other, rather than by diffing whole files; the objects were compared with `cmp`.

**The user's prediction**
None was written. The brief expected the five x86-private includes in `kernel/commands` to stay flagged, and they did not: `gn check` is per target (see Cause), so they went with the seventeen generic ones, 22 findings in all. The brief also expected six targets and the diff of the `.ninja` files to be order-only deps; the result is seven targets and, in two of them, a one-flag reorder of `-I` that changes no object.

**Three questions you should be able to answer**
- `//kernel/commands` includes `<ours/arch/x86/apic.hpp>` and the check says nothing after the edge. What is `gn check` actually asking, why does that make a private header pass, and what would bring the finding back without moving the test?
- `//kernel/mm` passed the check on x86 with no edge of its own and failed it on riscv. What on x86 supplied the edge, why does `//kernel/lib/userabi` not get it from `//kernel/mm`, and what does it say about a check that passes on only one architecture?
- A `-I` flag moved in two targets' command lines and not one object changed. When would a reordered include path change what a file compiles to, and which two facts here ruled it out?

## Title: kernel/phys/BUILD.gn is put in gn's layout on its own, so the edge the next entry adds is the only line of its diff (2026-10-30)

**Files**
- `kernel/phys/BUILD.gn` — layout only: `physload_module.gni` moves below `phys_executable.gni` in the import block (gn's order), six blank lines are added before a comment that follows a list or assignment and one extra blank line is removed, and two one-element lists (`defines`, `handoff`'s `public`) are written on one line.
- `docs/changes/261030/kernel-arch-seam.md` — this entry.

**Symptom**
Latent, cannot be triggered today: nothing about the build changes. `python3 scripts/format_gn.py --check kernel/phys/BUILD.gn` said `unsafe: the result differs from the source by more than layout` at HEAD, and the pre-commit hook refuses a commit of any GN file that is not in gn's layout, so no edit to this file could be committed as it was.

**Cause**
Two different things reorder tokens when `gn format` runs, and the tool treats them differently. List contents (`sources`, `deps`) are kept in the order the file wrote them (a `# NOSORT` marker is put above each list for the duration of the format), because their order is the link line and the include path. Import statements are not lists, and `gn format` sorts the contiguous block of them. The tool then checks that the formatted result has the same tokens as the source minus comments and commas; a swapped pair of imports fails that check, and the file is reported as unsafe and left alone rather than formatted. `kernel/phys/BUILD.gn` had three relative imports out of order, so it was in that state.

Reordering imports is the one change of order that is safe by construction here: `import()` evaluates a file in a scope of its own and copies its definitions into the importer, so what the importer has afterwards does not depend on the order, unless two files define the same name, which GN reports as an error. The proof is the build, not the argument: `gn gen` of both build directories produces the same 711 and 707 `.ninja` files byte for byte (below).

This is separate from the edge of the next entry for a reason the previous commit taught: its `kernel/mm/BUILD.gn` needed the same reformat and carried it in the same commit as the dependency, and the dependency became harder to find in the diff.

**Change**
The import block, before and after (the rest is blank lines and two lists collapsed to one line, in the diff):

```gn
# before
import("//kernel/kernel_package.gni")
import("physload_module.gni")
import("kernel_elf_interp.gni")
import("phys_executable.gni")

# after
import("//kernel/kernel_package.gni")
import("kernel_elf_interp.gni")
import("phys_executable.gni")
import("physload_module.gni")
```

The tool formatted the rest; I made the import swap by hand, which is the part it will not do. Every list keeps its order.

**Reference**
None, this project's own tooling (`scripts/format_gn.py`).

**Verification**
- `format_gn.py --check kernel/phys/BUILD.gn`: clean, from unsafe.
- `gn gen` of `out/bringup.x86` and `out/bringup.riscv`: all 711 and all 707 `.ninja` files byte-identical to copies taken before, `build.ninja` included; `compile_commands.json` has the same 752 and 638 entries as sets (the files themselves are not byte-identical).
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.5s.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.

**Deliberately not done**
- No dependency change, no comment changed: only whitespace, one import pair and the two one-element lists.
- The other GN files that are not in gn's layout are not touched; the hook judges only the ones a commit changes.

**How it was found**
`format_gn.py --check` on the file the next entry edits, and `gn format --stdin < kernel/phys/BUILD.gn | diff` to see which of the differences were lists (not applied, by the tool) and which were the imports (the tool's reason for calling it unsafe).

**The user's prediction**
None.

**Three questions you should be able to answer**
- `gn format` would sort `deps`, and the tool forbids it. What does each of the two orders (a `sources` list, a `deps` list) decide in the build, and why is an import block not like them?
- What does the tool compare to decide a formatted file is "layout only", and why does a swapped import pair fail that comparison although it is harmless?
- Why is "the `.ninja` files are byte-identical" a better proof that a reformat changed nothing than reading the diff?

## Title: `//kernel/phys:physload.core` depends on `//kernel/arch/$ours_cpu/kernel:headers` itself, because it passed the include check only by what the architecture's phys target forwards (2026-10-30)

**Files**
- `kernel/phys/BUILD.gn` — `"//kernel/arch/$ours_cpu/kernel:headers"` joins `physload.core`'s `deps`, with a comment naming the includer (`aspace.cpp`) and the header (`aspace_layout.hpp`).
- `docs/changes/261030/kernel-arch-seam.md` — this entry, which also corrects a sentence of the entry about the seven generic targets.

**Symptom**
Latent, cannot be triggered today: `gn check` and `gn check --check-system` report nothing about it before or after. The entry on the seven generic targets said, in its list of includers that need nothing, that "`//kernel/phys` ... already depend[s] on the label". That is true of the `physboot` module (`physload_module("physboot")` lists the label, with a comment about `kernel-boot.hpp`) and not of `//kernel/phys:physload.core`, whose `aspace.cpp` includes `<ours/arch/aspace_layout.hpp>` with no edge of its own, on x86 or riscv.

**Cause**
The previous entry's edge exists because a generic target that passes the include check "by accident", since an architecture's own target happens to forward the header, is a target whose correctness depends on a line in someone else's `BUILD.gn`. `physload.core` is the same accident, one level further out and so missed. `gn path` shows it, in the phys toolchain, on both architectures:

```text
//kernel/phys:physload.core --[public]-->
//kernel/arch/x86/phys:x86 --[public]-->
//kernel/arch/x86/kernel:headers
```

(and the same with `riscv` for both names). `//kernel/arch/<cpu>/phys:<cpu>` lists the headers target in its `public_deps`, and `physload.core` lists that target in its own `public_deps`, so GN's check finds a path made only of public edges and is satisfied. It is satisfied correctly by its own rule, and the scan that found nothing was a scan of `gn check --check-system`'s output, which cannot see a pass.

So the list of includers that need an edge was derived from the wrong thing, the checker's complaints, and the right derivation is the sources: for each target with sources, does one include a header of the per-arch set, and does the target list the owner directly. Done that way over both build directories (every target with sources, `#include <ours/arch/NAME.hpp>` for each NAME in `kernel/arch/<cpu>/kernel/include/ours/arch/`), there are nine includers on x86 and eight on riscv, and the one without a direct edge is `physload.core`: the seven of the previous entry, the `physboot` module, and the architectures' own targets (`//kernel/arch/x86/kernel:kernel`, `x86/mm:mm`, `x86/phys:x86`, `riscv/phys:riscv`), all of which list the label. Header-only targets are not in that scan (they have no sources); that is the next entry.

**Change**
Before and after, in `physload.core`'s `deps` (the new line is first, the others keep their order):

```gn
# before
  deps = [
    "//kernel/lib/ccrt",
...
# after
  deps = [
    # aspace.cpp includes <ours/arch/aspace_layout.hpp> for the address-space
    # layout constants. The edge is named here, to the running architecture's
    # headers, rather than relying on //kernel/arch/$ours_cpu/phys to forward
    # them through its public_deps.
    "//kernel/arch/$ours_cpu/kernel:headers",
    "//kernel/lib/ccrt",
```

It is a private edge like `physboot`'s, and `physload.core` already reaches the target publicly through the architecture's phys target, so nothing new is forwarded to its dependents.

Rejected:
- Correcting only the earlier sentence and leaving the build alone: the target would go on passing by someone else's `public_deps`, which is what the previous commit removed from seven others.
- `public_deps`: it would forward the architecture's headers to every dependent of `physload.core`, and they already receive them through the phys target.

**Reference**
None, this project's own graph; the precedent is `physload_module("physboot")` in the same file.

**Verification**
- `gn gen` of both build directories: x86 710 of 711 `.ninja` files byte-identical to copies taken before this entry's edit, the other being `kernel.phys_x86.phys.dbg_0/obj/kernel/phys/physload.core.ninja`, where the one line that differs is the order-only `linkdeps` edge of the target itself: the same 43 words, in a different order (the headers target was already in the list, reached through the phys target). riscv: all 707 identical. `compile_commands.json`: the same 752 and 638 entries as sets.
- `gn check` on both directories: `Header dependency check OK`. `--check-system --error-limit=1000000`, distinct pairs as in the earlier entries: x86 77 lines and 37 pairs, riscv 47 and 22, unchanged.
- Sixteen files hashed with `shasum -a 256` before and after: the twelve `physload.core.*.o` objects of the x86 phys toolchain, and `kernel.dev.dbg_1.obi`, `kernel.dev.obi`, `fake.obi` (x86) and `fake.obi` (riscv): all identical.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.6s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.4s.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.

**Deliberately not done**
- The earlier entry's sentence is not edited, since entries are append-only history; this one says what it got wrong.
- No edge for header-only targets, and no change to the `.cpp` of any target. `kernel/phys/aspace.cpp` stays as written.
- `physload_module("physboot")`'s comment says `kernel-boot.hpp` reaches the layout constants, which is a reason for the edge that `physboot.cpp` includes the header directly as well (`physboot.cpp:12`); the comment is not rewritten.

**How it was found**
Reported by an independent review that did not use the checker: it listed every target's sources, grepped each for the per-arch headers, and compared with each target's direct `deps`. `gn path` on the one it found showed the public chain. Reproduced here with a script of the same shape over the x86 and riscv directories.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `gn check` passed for `physload.core` before and after the edge. What does it ask, why is a chain of public edges enough to satisfy it, and why does that make a clean check a weak argument for "this target declares what it uses"?
- The previous entry's survey of includers came from the checker's complaints. What did that miss, and what is the survey that does not depend on the checker?
- Why is the edge private and not public, when `physload.core` already reaches the headers publicly?

## Title: A correction to the seven-targets entry: GN does read the includes of a header-only target and cannot resolve the per-arch ones there, so five generic headers are held by nothing (2026-10-30)

**Files**
- `docs/repairs/261030/build-headers.md` — new item 6: the five headers, the three ways to hold them, and what each costs.
- `docs/changes/261030/kernel-arch-seam.md` — this entry. No source or build file changes.

**Symptom**
Latent, cannot be triggered today. The entry on the seven generic targets (and its commit message, and its "Deliberately not done") says GN does not read the includes of header-only targets, which is why the edges went on source sets, and it hedged that conclusion once ("consistent with ... not tested beyond this one header"). The statement is wrong in its general form, and the consequence the hedge hid is a gap: `//kernel/lib/heap:headers` is a generic target whose `vmalloc.hpp` includes `<ours/arch/aspace_layout.hpp>`, owned by `//kernel/arch/<cpu>/kernel:headers`, with no edge to it, and `gn check --check-system` has nothing to say about it.

**Cause**
`gn check` finds the target a header belongs to by resolving each `#include` to a file, through the include directories of the including target, and then asks whether that target reaches the owner. The including target can be any target that lists the file, a header-only one included: GN reads `vmalloc.hpp` as part of `//kernel/lib/heap:headers` (it is in its `public`). What fails there is the first step. `cpp_interface()` sets `configs = []` so that a header target does not inherit the consuming toolchain's binary configs, and what is left in its include directories is its own root, the configs it publishes by hand (`//kernel/mm:headers.config`, which is how it gets `PAGE_SIZE`) and the ones its public dependencies publish. The architecture's `kernel/include` is not among them (the toolchain puts it on every compile of the kernel, in a config that a header target does not inherit), so `<ours/arch/aspace_layout.hpp>` resolves to no file, and `gn help check` says "includes matching unknown paths are ignored". A source set (`//kernel/lib/heap:heap`) has the toolchain's configs, resolves the include, and is held to an edge. So the real rule is that an include is checked from the target where GN can find the file, and for the per-arch headers that is every ordinary target and no header-only one.

The probe that shows it, on a copy of `kernel/lib/heap/BUILD.gn` edited and restored, on both build directories (`gn gen` into a scratch directory with the same `args.gn`, then `gn check --check-system --error-limit=1000000`, distinct pairs):

| Edit to `//kernel/lib/heap:headers` | x86 | riscv | what the check says |
|---|---|---|---|
| none | 37 | 22 | nothing about `vmalloc.hpp` |
| the architecture's `kernel:headers.config` added to `public_configs` | 38 | 23 | `vmalloc.hpp:6: Include not allowed. It is not in any dependency of //kernel/lib/heap:headers. The include file is in the target(s): //kernel/arch/<cpu>/kernel:headers` |
| a private `deps` edge to `//kernel/arch/<cpu>/kernel:headers` | 37 | 22 | nothing: the edge is there, and the include still does not resolve |
| that config and that edge | 37 | 22 | nothing: the include resolves and is held, and the edge satisfies it |
| a `public_deps` edge instead | 28 on x86 | 22 | nothing about heap, and nine other pairs gone: every dependent now receives the headers target, so includers without an edge pass |

Two things follow that the earlier entry did not have. The second row is the disproof: GN does read the include in a header-only target once it can resolve it. And the third is a correction to the obvious fix: a bare edge on `heap:headers` changes no check result, so it would document a dependency and hold nothing; what makes the include held is the include directory, with the edge to satisfy it. The fifth row is why `public_deps` stays rejected, for the reason the previous entry gave (it hides which includer needs the header).

Which headers this concerns. A scan of every `.hpp` and `.h` outside the architectures' directories for `#include <ours/arch/NAME.hpp>`, NAME being a header of a per-arch set, finds five:
- `kernel/lib/heap/include/heap/vmalloc.hpp` (`aspace_layout.hpp`), in `//kernel/lib/heap:headers`;
- `kernel/mm/include/ours/mm/cpu_local.hpp` (`cpu_local.hpp`) and `physmap.hpp` (`aspace_layout.hpp`), in `//kernel/mm:headers`, whose list is empty (repair item 1);
- `kernel/include/ours/task/thread.hpp` (`thread.hpp`) and `idle_thread.hpp` (`idle_thread.hpp`): `kernel/include` is not any `cpp_interface`'s root but a directory the kernel products put on the include path (`kernel/products/BUILD.gn`, `//kernel/include`), so these headers have no owning target at all, and listing mm's headers will not surface them.

The three in targets that could list them surface, when their targets are made to, only if the include also resolves; the two without a target are a larger question, because "which target owns `ours/task/thread.hpp`" is not a gap in a list.

**Change**
None to the build. The correction is this entry, and the open work is item 6 of `docs/repairs/261030/build-headers.md`, with the three options (an include-path config with an edge on `//kernel/lib/heap:headers`, the same on `//kernel/mm:headers` once item 1 lists it, and an owner for `kernel/include`).

I did not add the edge to `heap:headers`, though the probe says it builds and passes. It is the only one of the five that a single line could hold, but it is a layering decision: heap is a low-level library whose header target says in a comment that it takes mm's headers as an include-path config on purpose, to avoid a cycle, and the same shape for the architecture's headers (config and edge) would make heap's header target one more place that knows the running architecture. Whether it should is the user's.

**Reference**
None, this project's own graph. `gn help check` states the rule that unresolvable includes are ignored.

**Verification**
- The probe table above: every row is a run on both build directories, `gn gen` ok and `gn check --check-system` counted, with `kernel/lib/heap/BUILD.gn` restored after each (`git diff` of it is empty afterwards) and the scratch build directories removed.
- The scan of headers for per-arch includes (five hits) and of `//kernel/include` as an include root of any `cpp_interface` (none, on x86 and riscv).
- `lint_docs.py --changed` on the two documents: no violations.
- No source or build file changed, so the x86 and riscv `ours-verify` lines of the previous commit stand.

**Deliberately not done**
- No edge, config or list change for any of the five headers (see Change).
- The earlier entry is not edited: it is history, and this one corrects it.
- The commit message of the seven-targets commit says "GN does not read includes in header-only targets"; commits are not rewritten.

**How it was found**
Reported by an independent review, which added the architecture's config to `heap:headers` in a scratch copy, saw the finding appear, and scanned the headers for the other four. Reproduced here in place with a restoring script, and extended by the rows with a bare edge and with the config and the edge together, which show what each part contributes. The check of `kernel/include` as an include root came from asking why the reviewer's four did not all belong to targets that item 1 would list.

**The user's prediction**
None.

**Three questions you should be able to answer**
- GN read `vmalloc.hpp` and found nothing to check. Which step of `gn check` failed, why does a source set pass that step and a `cpp_interface` not, and what does a bare edge on the header target change?
- Adding the architecture's config to `heap:headers` makes a finding appear, and adding a `public_deps` edge makes nine disappear. What is each doing to the include resolution and the reachability that the check is made of?
- Two of the five headers have no owning target. Why is that a different kind of gap from a header missing from a list, and why can `header_lists.py` not see it?

## Title: Two comments on the new edges say why the edge is declared, and no longer what the include checker did before it (2026-10-30)

**Files**
- `kernel/mm/BUILD.gn` — the comment on `//kernel/arch/$ours_cpu/kernel:headers` names the includers and says the edge is declared here and not left to `//kernel/arch/$ours_cpu/mm` to forward; it no longer says that the check "passed" on x86 and not on riscv.
- `kernel/commands/BUILD.gn` — the comment on the same edge keeps what GN checks (per target, not per header) and which four tests include x86's own headers, and drops "with this edge the include check no longer reports them".
- `docs/changes/261030/kernel-arch-seam.md` — this entry.

**Symptom**
Latent, cannot be triggered today: comments only, no generated build file changes. The two comments recorded a fact about the checker at one point in time, and each becomes false by a change somebody will make for another reason: `//kernel/mm`'s when `//kernel/arch/riscv/mm` gains the edge (the sentence "riscv's does not" is then wrong), `//kernel/commands`'s when the four tests move to `kernel/arch/x86/commands` and the check could report them again.

**Cause**
A comment on a dependency edge answers "why is this here", and that answer has to hold as long as the edge does. "The include check passed there, and does not on riscv" is how the edge was found, not why it exists: it describes the checker's behaviour on the day, and it is already in the change record, where history belongs. The reason that lasts is a structural one: these sources include this header, and the edge is to the running architecture's headers, declared by the includer rather than inherited from whichever target happens to forward them. `kernel/commands` keeps one more sentence because it is a lasting property of the tool, not of the day: GN's include check is per target, so while the four x86-private tests are in a generic target a passing check says nothing about them, and a reader of the file should not take it for a clean target.

**Change**
`kernel/mm/BUILD.gn`, before and after:

```gn
# before
      # vmm.cpp, init_vmm.cpp, vm_aspace.cpp and cpu_local.cpp include
      # <ours/arch/aspace_layout.hpp>. On x86 //kernel/arch/x86/mm already
      # depends on these headers, which is why the include check passed there;
      # riscv's does not.
# after
      # vmm.cpp, init_vmm.cpp, vm_aspace.cpp and cpu_local.cpp include
      # <ours/arch/aspace_layout.hpp>. The edge is to the running
      # architecture's headers, declared here and not left to
      # //kernel/arch/$ours_cpu/mm to forward them.
```

`kernel/commands/BUILD.gn`: "so it also covers the four tests" becomes "so it also satisfies the four tests", and the clause ", and with this edge the include check no longer reports them" is deleted, the sentence ending at "rather than in a generic target".

The comments on the other five edges (object, user_copy, irq, userabi, the benchmark) were read for the same fault and say only what is included and, for userabi, why the edge is named outright; they are left.

**Reference**
None, this project's own graph.

**Verification**
- `gn gen` of both build directories: all 711 x86 and 707 riscv `.ninja` files byte-identical to copies taken before; `compile_commands.json` the same 752 and 638 entries as sets.
- `format_gn.py --check` on both files: clean.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.4s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.4s.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.

**Deliberately not done**
- No dependency changes. The earlier entry's account of how the edge was found stays as it is.
- The "belong in kernel/arch/x86/commands" sentence stays: it is the plan (R25), and it is a plan the comment should lose when the tests move.

**How it was found**
Reported by an independent review of the commit. Checked here by reading all seven comments against one question, whether each would still be true if the other architecture's mm target forwarded the header or the four tests moved.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A comment says "the include check passed there; riscv's does not". Which change elsewhere makes it false, and what does the comment that replaced it say that stays true?
- Why does `kernel/commands` keep "GN checks per target, not per header" when it drops "no longer reports them"?
- Comments and change records both record history. What goes in which, and how do you tell a lasting reason from the story of how a line was found?
