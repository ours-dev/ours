# kernel-top changes

## Title: //kernel/top stops depending on x86, so a header riscv lacks no longer resolves to x86's copy (2026-10-29)

**Files**
- `kernel/top/BUILD.gn` — drops `//kernel/arch/x86/kernel:headers` and `//kernel/driver` from `top`'s deps
- `kernel/arch/x86/kernel/BUILD.gn` — depends on `//kernel/driver` (the i8253) instead of `//kernel/driver:headers`, with a comment saying why it is the PC's arch code that links it
- `kernel/mm/init_pmm.cpp`, `kernel/mm/init_vmm.cpp` — drop `#include <arch/x86/cache.hpp>`, from which neither uses anything
- `kernel/platform/include/ours/platform/interrupt.hpp` — includes the dispatcher `<arch/interrupt.hpp>` instead of `<arch/x86/interrupt.hpp>`

**Symptom**
Latent on x86, which builds and boots the same. In a riscv build of vmours (`tools/set.py bringup.riscv`, `ninja -k 0 kernel_riscv.dbg.1/phony/kernel/products/bin/vmours`), the five compiles of `kernel/top` carried `-I../../kernel/arch/x86/kernel/include`, `-I../../kernel/arch/x86/lib/arch/include` and `-I../../kernel/arch/x86/lib/page_tables/include`, and `clang -H` of `main.cpp` showed `ours/arch/cpu_local.hpp` and `ours/arch/thread.hpp` opened from `kernel/arch/x86/kernel/include`. The x86 `X86Thread` was being compiled into a riscv object without a word from the compiler, and `main.cpp` failed only later, at `vm_aspace.hpp:7`. The i8253 driver (`kernel/driver/clock/i8253.cpp`, the PC's interval timer chip) was compiled for riscv too, and its object was on the riscv link line.

**Cause**
The objects:
- A GN `cpp_interface` target such as `//kernel/arch/x86/kernel:headers` publishes an include directory. Every target that depends on it, directly or through `public_deps`, gets that directory as an `-I` on each of its compiles. The edge says nothing about which CPU is being built: `kernel/top/BUILD.gn:15` named x86 by path, so the riscv toolchain followed it like any other.
- The compiler resolves `#include <ours/arch/thread.hpp>` by walking the `-I` list in order and taking the first directory that has the file.
- The per-arch kernel headers are a protocol: generic code includes `<ours/arch/thread.hpp>`, `<ours/arch/cpu_local.hpp>` and so on, and each architecture's `kernel/arch/<cpu>/kernel/include` provides them. The riscv toolchain puts riscv's directories on every kernel compile (`build/toolchain/ours/BUILD.gn`, the `kernel-riscv` prefix configs). riscv has written almost none of the headers yet.

The events, in a riscv compile of `kernel/top/main.cpp`:
1. `main.cpp` includes `<ours/task/thread.hpp>`, which checks `__has_include(<ours/arch/thread.hpp>)` and raises `#error "... required by [protocol.module.task]"` when it is absent.
2. The compiler looks in riscv's `kernel/arch/riscv/kernel/include`: no `ours/arch/thread.hpp`.
3. It goes on down the list, reaches `kernel/arch/x86/kernel/include` from the stray edge, and finds x86's. The guard passes.
4. x86's `ArchThread`, its `X86_PCPU`-backed `ArchCpuLocal` and whatever they include are compiled as if they were riscv's.

So the guard written to stop exactly this was defeated, and every header riscv will write in step 1b would have had a silent fallback: forget one and the build uses x86's instead of failing. That is why this had to go before any riscv header is written. A dependency on an architecture's headers is the architecture's to declare: `//kernel/arch/$ours_cpu/kernel`, which `top` already depends on, is the edge that follows the CPU being built.

`//kernel/driver` was a second instance of the same mistake, in the link rather than the include path. It holds one driver, the i8253, which only x86's `kernel/arch/x86/kernel/time/time.cpp` uses (it includes `<ours/driver/i8253.hpp>` and calls `i8253_init()` in `setup_pit()`). Linked from `kernel/top`, every architecture's kernel carried a driver for a chip only a PC has, compiled against `<arch/x86/io.hpp>`. A source set's objects travel up its dependency chain to the executable that links them, so the x86 arch target depending on it is enough for vmours on x86 to keep linking it.

The two `<arch/x86/cache.hpp>` includes in `kernel/mm` are leftovers: neither file names `arch::` at all. An x86 header included by generic code by name is the third form of the same leak; on riscv it would be "file not found" or, worse, a silent success through a stray `-I`.

`ours/platform/interrupt.hpp` is a generic header (the platform seam's interrupt calls) that included `<arch/x86/interrupt.hpp>` for `arch::IrqFrame`. `<arch/interrupt.hpp>` is the dispatcher that `kernel/lib/arch` keeps for this purpose: it includes the current architecture's `interrupt.hpp` under `ARCH_X86`, `ARCH_ARM` or `ARCH_RISCV`.

**Change**
Before, `kernel/top/BUILD.gn`:

```text
  deps = [
    "//kernel/arch/$ours_cpu/kernel",
    "//kernel/arch/x86/kernel:headers",
    "//kernel/driver",
    "//kernel/kernel",
```

After:

```text
  deps = [
    "//kernel/arch/$ours_cpu/kernel",
    "//kernel/kernel",
```

and `kernel/arch/x86/kernel/BUILD.gn` takes `"//kernel/driver"` where it had `"//kernel/driver:headers"`. `init_pmm.cpp` and `init_vmm.cpp` lose `#include <arch/x86/cache.hpp>`; `ours/platform/interrupt.hpp` goes from `#include <arch/x86/interrupt.hpp>` to `#include <arch/interrupt.hpp>`.

The sweep of everything else outside `kernel/arch/x86` and `kernel/platform/pc` (the PC platform, x86 only) that names x86, with what was left and why:
- `kernel/lib/logz4/kernel.cpp`: `<arch/x86/io.hpp>` and its `#if defined(ARCH_X86)`. The next change in this series (step 1a.3 of `.agents/tasks/riscv-kernel/PLAN.md`) splits it.
- `kernel/lib/kshell/kshell/shell.cpp:12`: polls COM1 through `<arch/x86/io.hpp>`. A real x86 dependency of kshell; on riscv kshell stays out of the build through `kernel_shell_enabled` until step 7, which reads through `platform::debug_console_read`.
- `kernel/commands/interrupt_line_test.cpp:88` (`<arch/x86/interrupt.hpp>`) and `kernel/commands/ioport_test.cpp:34-35` (`<ours/arch/x86/io_bitmap.hpp>`, `percpu.hpp`): x86-only test commands, which go with kshell and are gated by arch in step 7.
- `kernel/driver/clock/i8253.cpp:7`: `<arch/x86/io.hpp>`. The driver is a PC device, and after this change only x86's arch target links it.
- `kernel/lib/arch/include/arch/*.hpp`: the dispatcher headers, whose job is to pick `arch/x86/...` under `ARCH_X86`.
- `kernel/lib/arch/test/x86_*_smoke.cpp`, `kernel/lib/arch/BUILD.gn:52,72,92`: host tests of x86's own `lib/arch` headers.
- Root `BUILD.gn:130,134`: the host tests of x86's idle states and page-table walks, listed in the tree's test group.
- `benches/kernel/lib/arch/BUILD.gn`: the host benchmark of x86's page-table walks.
- `kernel/mm/tests/BUILD.gn:22,40`: host tests that build generic mm sources in the host toolchain against a fixed host-side arch include set (the file's comment says so); no kernel compiles them.
- `build/switch/kernel_switchset.gni:117`: x86's page-table config, inside `if (cpu == "x86")`.
- `build/toolchain/ours/BUILD.gn:83-85,153-154`: the x86 kernel and phys toolchains' own include paths.
- `kernel/phys/BUILD.gn:222`: inside `if (ours_cpu == "x86")`.
- `build/config/x86/BUILD.gn:20`, `system/BUILD.gn:30,125`: comments.

**Reference**
Zircon keeps the same rule by construction: generic kernel code reaches arch headers only through `//zircon/kernel/arch/$zircon_cpu` (`zircon/kernel/BUILD.gn`, where `f8c925da99a` added `"//zircon/kernel/lib/arch/$kernel_cpu:headers.config"` to the kernel toolchain), never by naming x86. Its PIT code lives in the PC platform (`zircon/kernel/platform/pc/timer.cc`), so no other architecture links it.

**Verification**
- x86 `ours-verify`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (152 passed). vmours still defines `ours::i8253_init()` and `I8253_CLKEVT` (`llvm-nm`).
- riscv phys check (`boot_check.py --out out/bringup.riscv`): PASS, `handoff ok at 0.1s`.
- `gn check out/bringup.x86` on `//kernel/top/*`, `//kernel/arch/x86/kernel/*`, `//kernel/mm/*`, `//kernel/platform/*`: OK.
- riscv probe (`out/probe-s1a-e7ee-riscv`): of the 186 compiles vmours needs, none outside `kernel/arch/x86` and `kernel/platform/pc` carries an `-I` into `kernel/arch/x86` (5 did, all `kernel/top`). `clang -H` opens no file under `kernel/arch/x86` for `kernel/top/main.cpp` (1205 headers opened; it now stops at `'ours/arch/cpu_local.hpp' file not found`, the honest error), `kernel/top/crash_record.cpp`, `kernel/mm/init_pmm.cpp`, `kernel/mm/init_vmm.cpp` and `kernel/lib/logz4/kernel.cpp`; `-M -MG` agrees. `ninja -k 0`: 144 failing compiles, from 145; the one gone is `kernel/driver/clock/i8253.cpp`, which riscv no longer builds.
- clang-tidy on `init_pmm.cpp` and `init_vmm.cpp`: no diagnostic in either file (only pre-existing ones in headers they include).

**Deliberately not done**
- kshell's COM1 polling and the x86-only commands: step 7, through `kernel_shell_enabled`, as listed above.
- `//kernel/arch/x86/kernel:headers` itself is not renamed or split: x86's own targets and `kernel/platform/pc` depend on it legitimately.
- No `#error` was added for a generic target that names an x86 path: the sweep found no other one, and a check in GN for that belongs with a decision on how the tree polices it.

**How it was found**
The research for step 1 (2026-10-29) built riscv vmours with `ninja -k 0` and read the include graph with `clang -H`, which showed `main.cpp` getting past `ours/task/thread.hpp`'s guard on x86's header. `gn path` from `//kernel/top` to `//kernel/arch/x86/kernel:headers` showed the one edge.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did `ours/task/thread.hpp`'s `__has_include` guard pass on riscv, though riscv has no `ours/arch/thread.hpp`?
- Why is it enough for x86's arch target to depend on `//kernel/driver` for the i8253 to stay in x86's vmours?
- What would have happened in step 1b if a riscv `ours/arch/idle_thread.hpp` had been forgotten with the old edge still in place?

## Title: Corrections to the //kernel/top record: kshell is still in the riscv build, and x86's vmours layout did move (2026-10-29)

**Files**
- `docs/changes/261029/kernel-top.md` — this entry, correcting two statements of the one above

**Symptom**
Two statements in the entry above are wrong:
- Its sweep says of `kernel/lib/kshell/kshell/shell.cpp:12` that "on riscv kshell stays out of the build through `kernel_shell_enabled` until step 7". Nothing turned it off: `kernel_shell_enabled` is `true` for every CPU (`kernel/kernel_params.gni`), and the riscv build's `compile_commands.json` holds 74 compiles of `kernel/commands` and 2 of `kernel/lib/kshell` in the riscv kernel toolchain. `build.md`'s "step 1b says what riscv needs from it" was not true either when it was written: the plan's step 1b did not mention it.
- Its Symptom says x86 "builds and boots the same", and its Verification checks only that the i8253's symbols are still defined. That is true of behaviour and of the symbol set, but the image changed: moving `//kernel/driver` from `kernel/top`'s deps into `//kernel/arch/x86/kernel`'s moved the i8253 object on the link line, and with it the layout of `.text`, `.rodata`, `.init.text` and `.rela.dyn`.

**Cause**
The first is a plan written as a fact. The research for step 1 proposed defaulting `kernel_shell_enabled` off for riscv; the brief for step 1a moved that to step 1b and said not to change the default in 1a; the record then described the planned state as the present one. On riscv today kshell and the x86-only commands still compile, and fail at x86's headers (`arch/x86/io.hpp`, `ours/arch/apic.hpp`, `ours/arch/x86/io_bitmap.hpp`), which is why they appear in step 1b's header list.

The second is a check that looked at the wrong level. A GN source set's objects go on the executable's link line in the order the dependency walk reaches them, and lld lays sections out in input order. `//kernel/driver` was a dep of `kernel/top` listed after `//kernel/arch/$ours_cpu/kernel`, so its object followed the whole x86 arch subtree; as a dep of `//kernel/arch/x86/kernel` it sits inside that subtree. Same objects, same symbols, a different order, so different addresses and different bytes. `build.md` takes care over link order because `--icf=all` keeps the first of identical functions by input order; this change could have moved such a choice and the record did not look.

**Change**
None to code. Read the entry above with these replacements:
- For "on riscv kshell stays out of the build through `kernel_shell_enabled` until step 7": "on riscv kshell is still in the build; how it leaves the riscv link until step 7 is an open decision of step 1b (`.agents/tasks/riscv-kernel/PLAN.md`), the recommended one being `kernel_shell_enabled` defaulting to `ours_cpu != "riscv"`".
- Add to its Verification: "The default x86 image's layout shifted: the i8253 object is now the 43rd input of the dbg vmours link (`vmours.binary.ninja`), between `//kernel/arch/x86/lib/idle_states` and `//kernel/lib/acpi`, inside the x86 arch target's block, where it used to follow that block. The review compared the dbg vmours of `dbc96a47` and `c0753c85`: the same symbol names and sizes (`llvm-nm -C -S`), different `.text`, `.rodata`, `.init.text` and `.rela.dyn` bytes; in `nm -n` order the four `I8253ClockDevice` vtable thunks moved from about entry 642 to 576, and `ours::i8253_init()` now comes before `lockup_detector::(anon)::arm()` in `.init.text`."

**Reference**
None, a correction of this project's own record.

**Verification**
- `kernel/kernel_params.gni`: `kernel_shell_enabled = true`, no condition on the CPU.
- `out/bringup.riscv/compile_commands.json`: 332 compiles in the riscv kernel toolchain, 74 of them under `kernel/commands` and 2 under `kernel/lib/kshell`.
- `out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours.binary.ninja`: `obj/kernel/driver/clock/driver.i8253.cpp.o` is input 43, after `idle_states.idle_states.cpp.o` and before `acpi.debug.cpp.o`.
- The byte and symbol comparison is the review's; it was not repeated here.

**Deliberately not done**
- Rewriting the entry above. Change records are append-only; this entry is the correction.
- Turning `kernel_shell_enabled` off for riscv: a decision for step 1b, put to the user with it.

**How it was found**
Review of step 1a: the reviewer counted the kshell compiles in the riscv build and compared the two vmours images byte by byte and symbol by symbol.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does moving a GN dependency from one target to another change the bytes of the linked kernel when no source changed?
- When does a changed link order change what the kernel does, rather than only where its code sits?
- Why do kshell's x86 headers show up in step 1b's list of headers riscv lacks?
