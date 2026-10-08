# system changes

## Title: the console shell moves from examples/ to system/app/ (2026-10-07)

**Files**
- `examples/shell/` → `system/app/shell/` — the whole directory, renamed: `BUILD.gn`, `shell.cpp`, `shell_launcher.hpp`, `shell_tests.cpp`, `shell_tests.hpp`
- `system/app/shell/BUILD.gn` — the toolchain assert names the new path; the program's visibility is `:*` and the images, in place of `//examples/*`
- `system/app/shell/shell_launcher.hpp`, `system/app/shell/shell_tests.hpp` — header guards `SHELL_SHELL_LAUNCHER_HPP` and `SHELL_SHELL_TESTS_HPP`, as the other programs under `system/` name theirs (`INIT_*`, `PC_PS2_*`), in place of `OURS_EXAMPLES_SHELL_*`
- `system/README.md` — `system/app` gets its first entry, the shell, and says what makes a program an app rather than a service: it serves no protocol
- `examples/BUILD.gn` — the `examples` group no longer lists the shell; its comment says the system's own programs live under `//system`
- `kernel/products/image/dev/BUILD.gn` — root.cm's child `shell` names `//system/app/shell:shell.cm`
- `kernel/products/image/dev.dbg_1/BUILD.gn` — the BootFS takes `//system/app/shell`; "the example shell" is now "the console's shell"
- `kernel/products/image/BUILD.gn`, `system/BUILD.gn`, `kernel/lib/syscall/src/debug.cpp` — comments that told the shell's history by its old label now say "the shell"
- `system/component_manifest.gni` — the usage example and the toolchain note name the new label
- `kernel/lib/syscall/test/fault_probe/BUILD.gn`, `kernel/commands/vma_seal_test.cpp`, `system/lib/loop/test/fake_kernel.hpp`, `system/lib/loop/README.md` — comments that point at the shell's checks name the new path (fault_probe's also names `shell_tests.cpp`, where `run_probes()` has lived since the checks moved out of `shell.cpp`)
- `.agents/skills/ours-verify/scripts/boot_check.py`, `scripts/kshell_run.py` — the comment on `END_MARKER` names the new path
- `docs/design/261004/component-framework.md` — the manifest example and the toolchain note name the new path
- `docs/repairs/261004/mm.md`, `docs/repairs/261004/object.md`, `docs/repairs/260910/x86.md` — open items point at the new path

**Symptom**
None at run time. The tree said the wrong thing about the shell: it sat in `examples/`, beside a demo workload and a demo component, although it is the system's console, started on every dev boot by `meta/root.cm` and by userboot on dbg_1, and it holds the system's on-target checks.

**Cause**
`examples/` was the only home a ring-3 program had when the shell was first written (2026-09-10; init, the first program under `system/service`, came on 2026-09-23), and it began as a program that listed the kernel's command registry from ring 3, which an example could be. It then became the serial console's shell and the driver of most on-target checks, and stayed where it started. A directory is a claim about what its contents are for: a reader who skips `examples/` as optional skips the console, and anyone deciding what may depend on what (`examples/*` visibility, "nothing in the system needs an example") gets the wrong answer.

`system/README.md` already had a place for it. `system/service` is for programs that serve protocols (init, the root, serves `ours.process.Launcher` and the `ours.resource` protocols); `system/driver` for device drivers; `system/app` for applications. The shell serves no protocol and drives no device: it uses `/boot`, the Launcher and the resources, so it is an app, and the first one.

What stays in `examples/`: `echo`, a component that serves `ours.examples.echo.Echo` so routing_checks can test a route through a sibling's expose, and `profile`, a workload for the sampler. Both exist to show something.

**Change**
`git mv examples/shell system/app/shell`, then every live label and path:

```gn
# kernel/products/image/dev/BUILD.gn, before
      manifest = "//examples/shell:shell.cm"
# after
      manifest = "//system/app/shell:shell.cm"
```

```gn
# kernel/products/image/dev.dbg_1/BUILD.gn, before
    "//examples/shell(//build/toolchain/ours:user-${target_cpu})",
# after
    "//system/app/shell(//build/toolchain/ours:user-${target_cpu})",
```

The program's visibility was `//examples/*` so the `examples` group could list it; nothing under `//system` lists programs in a group (`system/service` and `system/driver` have no BUILD.gn of their own), so it is now `:*` (its own `shell.cm`) and the images. No `system/app/BUILD.gn` group was added, for the same reason.

The names did not change. The program's BootFS name comes from its target name through `user_executable()` (`bin/shell`), and the child's name and URL in root.cm from the child entry and the manifest's target name (`shell`, `fuchsia-boot:///#meta/shell.cm`), so `userboot.next=bin/shell`, the generated `programs.hpp` headers and the shell's commands are as they were.

**Reference**
Fuchsia keeps the serial console's shell with the system's bring-up programs, not in `//examples`: `//src/bringup/bin/console-launcher` (`ConsoleLauncher::LaunchShell()`) starts `/boot/bin/sh` on the console, and `//examples` holds only demonstrations.

**Verification**
`boot_check.py` in the worktree, on `baseline/20260824` (2867a8eb) and on this change rebased onto it, from a warm ccache:

| Image | Before the move | After the move |
| --- | --- | --- |
| dbg | PASS, 35 passed, 3 skipped, 0 failed, 0 error lines | PASS, 35 passed, 3 skipped, 0 failed, 0 error lines |
| rel | PASS, 33 passed, 9 skipped, 0 failed, 6/6 required, 0 error lines | PASS, 33 passed, 9 skipped, 0 failed, 6/6 required, 0 error lines |
| host tests | 143 passed, 0 failed | 143 passed, 0 failed |

The shell's `passed`/`skipped` lines are the same set on both sides (37 on dbg, 42 on rel). rel logs `pc-ps2: key a` and `pc-ps2: key A`. The dbg shell finished at 16.0 s before and 50.6 s after, then 21.0 s on a second boot of the same image: host load from other sessions, not the change, which changes no code. Both images' BootFS manifests (`bootfs.fini`) take the shell from `user-x86/obj/system/app/shell`. `kshell_run.py irq.test` on the dbg image: ok, 72 checks. `git grep -n examples/shell` lists only change records, task records, dated design tables and the history lines of repair items.

**Deliberately not done**
- `examples/profile` and `examples/echo` stay: each is a demonstration, and the shell's checks name profile only by its install name, so it need not move with the shell.
- Task documents under `.agents/tasks/` and the dated tables in `docs/design/260923` and `260924` keep `examples/shell`: they record what was changed where at the time.
- Item 4 of `docs/repairs/261005/pc-ps2.md` still names `examples/shell/shell.cpp`: the item appears fixed (the shared `ours_resource::get()` client exists) and should be deleted with a change entry by whoever owns it, not edited here.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What tells an app under `system/app` apart from a service under `system/service`, and which is init?
- Why did moving the shell not change `bin/shell`, `meta/shell.cm` or any `programs.hpp` constant, and what change to the BUILD.gn would have?
- Why does root.cm in an image's BUILD.gn not load `system/app/shell/BUILD.gn` in the kernel toolchain, where its assert would fail gen?
