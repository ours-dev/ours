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
