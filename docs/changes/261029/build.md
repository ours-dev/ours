# build changes

## Title: A kernel_no_userabi GN argument builds the kernel without user space, on by default for riscv (2026-10-29)

**Files**
- `kernel/kernel_params.gni` — new argument `kernel_no_userabi`, beside `kernel_shell_enabled`, defaulting to `ours_cpu == "riscv"`; the file now imports `//build/ours/cpu.gni` for `ours_cpu`
- `kernel/products/BUILD.gn` — `kernel.config` defines `KERNEL_NO_USERABI` when the argument is true, as it defines `CONFIG_CPU_HOTPLUG` for `cpu_hotplug`
- `kernel/top/BUILD.gn` — `//kernel/lib/syscall` and `//kernel/lib/userabi` only when the argument is false, each kept at its old place in the list
- `kernel/top/main.cpp` — under `KERNEL_NO_USERABI`, `labour_routine()` logs that userboot does not start instead of calling `userabi::userboot_init()`, and `<userabi/userboot.hpp>` is not included
- `kernel/top/crash_record.cpp` — under `KERNEL_NO_USERABI`, `recover_record()` still reports the previous boot's record but does not call `userabi::stash_crashlog()`
- `kernel/arch/x86/kernel/syscall.S` — under `KERNEL_NO_USERABI`, `x86_syscall` calls `x86_syscall_without_userabi` where it called `syscall_dispatch`
- `kernel/arch/x86/kernel/faults.cpp` — defines `x86_syscall_without_userabi()` under `KERNEL_NO_USERABI`: a panic naming the switch

**Symptom**
None on x86 with the default, which builds and links exactly as before. Without the switch, porting vmours to riscv meant porting the user ABI layer along with everything else before the first boot: in the riscv probe build, 20 of the failing compiles were in `//kernel/lib/syscall` (16), `//kernel/lib/userabi` (2), `//kernel/lib/kelfload` (1) and `//system/lib/elfload` (1, its `#error "elfload has no ELF machine for this target"`), and behind them wait the x86-64 vDSO (`vdso.S`), the generated shims' `frame->ax`, and userboot's `%rdi`/`%rsi` conventions.

**Cause**
The objects:
- The user ABI layer: the system calls (`//kernel/lib/syscall`, with `syscall_dispatch()` and the dispatch table), and the kernel's half of starting userboot (`//kernel/lib/userabi`, with the ELF loading it needs, `//kernel/lib/kelfload` and `//system/lib/elfload`). Everything in it exists to serve a user program.
- The architecture's half of it: a system-call entry, a vDSO, the ELF machine and relocations, a way into ring 3 (or U-mode). riscv has none of these yet; they are step 9 of `.agents/tasks/riscv-kernel/PLAN.md`.
- A GN `declare_args()` argument: a build-time switch with a default, overridable from `args.gn`. Its default is evaluated per toolchain, so it may depend on the toolchain's CPU.

The kernel needs none of the user ABI layer to boot its own half: the scheduler, memory, interrupts and timers come up before `labour_routine()` reaches `userboot_init()`, the first and only place the kernel turns to user space. Without a way to leave the layer out, a new architecture has two bad options: write the whole user side before its kernel can boot at all, or write a placeholder for every symbol the layer needs, which is the user-side work stubbed out piece by piece and kept until step 9. The user chose the switch (decision A, 2026-10-29) over that second option.

What the switch has to leave behind is decided by what still refers to the layer once it is gone:
- `kernel/top`: `main.cpp` calls `userabi::userboot_init()`, and `crash_record.cpp` hands the recovered crash record to `userabi::stash_crashlog()` for userboot to read. Both are compiled out; the record is still reported in the log, which was always its first reader.
- x86's system-call entry, `x86_syscall` in `syscall.S`, calls `syscall_dispatch()`. Linked with the switch on, that was the only undefined symbol left (`ld.lld: error: undefined symbol: syscall_dispatch`, referenced from `kernel.syscall.S.o:(x86_syscall)`). A kernel with no user space starts no process, so no `SYSCALL` can reach the entry; the call becomes a named function that panics, which is the one kind of stand-in `ours-kernel-design` rule 3 allows: the link refers to it.
- Nothing else. `//kernel/object` and `//kernel/lib/user_copy` stay: they do not depend on the layer, and they are what the system calls are later built on.

**Change**
`kernel/kernel_params.gni`:

```text
  kernel_no_userabi = ours_cpu == "riscv"
```

`kernel/products/BUILD.gn`:

```text
  if (kernel_no_userabi) {
    defines += [ "KERNEL_NO_USERABI" ]
  }
```

`kernel/top/BUILD.gn`, where the list had `"//kernel/lib/syscall"`, `"//kernel/lib/unwind"`, `"//kernel/lib/userabi"` in a row:

```text
  if (!kernel_no_userabi) {
    deps += [ "//kernel/lib/syscall" ]
  }
  deps += [ "//kernel/lib/unwind" ]
  if (!kernel_no_userabi) {
    deps += [ "//kernel/lib/userabi" ]
  }
```

Why three pieces and not one `if` at the end. GN puts a target's objects on the link line in the order of its deps, depth first, and the link order decides more than layout: with `--icf=all`, identical functions fold into one, and which one's name survives depends on which came first. The first version of this change moved both deps into one block after the list; with the switch off, the x86 dbg vmours then lost two symbol names, `ChannelDispatcher::on_zero_handles_locked()` and `on_peer_zero_handles_locked()`, which `--icf=all` had folded into identical functions met earlier on the new link line. The code was the same, but the image was not. Kept in place, the default links exactly as before.

`kernel/top/main.cpp`:

```cpp
#ifndef KERNEL_NO_USERABI
        userabi::userboot_init();
#else
        log::info("kernel_no_userabi: the kernel is up; no user space is built in, so userboot "
                  "does not start");
#endif // #ifndef KERNEL_NO_USERABI
```

The log line is the boot's last word when the switch is on, so a bring-up sees the kernel's half end, and later riscv steps have a line to stop at.

`kernel/arch/x86/kernel/syscall.S`:

```text
#ifndef KERNEL_NO_USERABI
    call syscall_dispatch
#else
    call x86_syscall_without_userabi
#endif // #ifndef KERNEL_NO_USERABI
```

Rejected for x86's entry: not installing it at all when the switch is on (no `IA32_LSTAR`, so a `SYSCALL` raises `#UD`). That needs `init.cpp`'s MSR setup, `syscall.S` and `arch::stack_roots()`'s two labels in `syscall.S` gated as well, for a path that cannot run; the stand-in is two lines and names the switch if it ever does run.

The define is spelled `KERNEL_NO_USERABI`, a name that is either defined or not, like `CONFIG_CPU_HOTPLUG`, the tree's other switch that compiles code out with `#ifdef`; the `NAME=value` defines (`MM_NUMA`, `SCHED_*`) are read with `#if`.

`kernel_shell_enabled` keeps its default; it is not changed in this step.

**Reference**
Zircon's `kernel_no_userabi` (`zircon/kernel/params.gni:107-108`, "Build a kernel with no user-space support, for development only"), added in `f8c925da99a` with the default `current_cpu == "riscv64"` and later set false again in `8b2b0f27d2c` once riscv64 had user space. When it is true, `zircon/kernel/lib/syscalls/BUILD.gn:28` and `zircon/kernel/lib/userabi/BUILD.gn:15` leave their implementations out, and `zircon/kernel/switch/BUILD.gn:412-413` defines `KERNEL_NO_USERABI`. Zircon's riscv64 syscall entry does what x86's does here: under `KERNEL_NO_USERABI` it defines `unknown_syscall`, which panics with "no syscall hooks to call!", so the entry still links (`zircon/kernel/arch/riscv64/exceptions.S:14-26`, `:118-122`).

**Verification**
- x86 `ours-verify` with the default: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (152 passed). The x86 dbg vmours defines the same symbols as after the previous commit (0 lines of difference); against the build of `47b0960d`, before step 1a, the only difference is the two functions step 1a.3 added.
- x86 with `kernel_no_userabi=true` (`tools/set.py bringup.x86 --out out/probe-s1a-e7ee-x86-nouserabi --args kernel_no_userabi=true`): compiles and links, with 21 fewer objects on the vmours link line (230 to 209: 16 of `lib/syscall`, 2 of `lib/userabi`, 1 of `lib/kelfload`, 2 of `system/lib/elfload`) and `.text` from 869948 to 755716 bytes. `gn desc ... deps --all` reaches none of those libraries. Both images boot all four CPUs and end with:

```text
[3331039000 cpu0 info] crashlog: no record from the previous boot
[3331161000 cpu0 info] (kshell) not started (kernel.shell is off)
[3331641000 cpu0 info] kernel_no_userabi: the kernel is up; no user space is built in, so userboot does not start
```

  (rel; dbg starts its kernel shell, `kshell> ... (kshell) console on COM1`, prints the same line at 3.522 s, and the laborer thread exits). Then nothing, as nothing is left to log; `boot_check.py` reports `FAIL`, since no user shell prints `--- end ---`, and after its 15 s of silence sends `nmi`, whose lockup report shows every CPU in its idle thread.
- riscv probe (`kernel_no_userabi = true` by default, `gn args --list`): 125 failing compiles, from 145; the 20 gone are those of `lib/syscall`, `lib/userabi`, `lib/kelfload` and `system/lib/elfload`, including elfload's `#error`.
- riscv phys check: PASS, `handoff ok at 0.1s`.
- `gn check` on `//kernel/top/*`, `//kernel/products/*` and `//kernel/arch/x86/kernel/*`, in both x86 configurations: OK. clang-tidy on `main.cpp`, `crash_record.cpp` and `faults.cpp` in both configurations: nothing on a changed line.

**Deliberately not done**
- `kernel_shell_enabled` stays true by default on riscv; the brief for this step leaves it, and step 1b says what riscv needs from it.
- The images do not drop userboot and the rest of user space with the switch on: which components an image carries is the image's own GN, and a kernel that never reads them loses nothing by their being there.
- The x86 kernel shell on a `kernel_no_userabi` image was not driven: `scripts/kshell_run.py` waits for the user shell's `--- end ---` before it types, and that never comes here.

**How it was found**
The x86 link with the switch on reported the one undefined symbol, `syscall_dispatch`, from `x86_syscall`. The link-order effect showed in the `llvm-nm` comparison of the default build before and after the change, which is run after every commit of this series.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does the kernel need nothing from `//kernel/lib/syscall` to boot, yet still failed to link without it on x86?
- Why can moving two entries of a `deps` list change which function names an image's symbol table keeps, without changing what the code does?
- What does a boot with `kernel_no_userabi=true` print last, and why does `boot_check.py` still call it a failure?
