# ours-verify changes

## Title: boot_check.py boots the riscv phys chain when given a riscv build directory, and reports the furthest stage it reached (2026-10-28)

**Files**
- `.agents/skills/ours-verify/scripts/riscv_boot.py` — new: builds the riscv shim and, when it builds, `fake.obi`; boots them on QEMU `virt` under OpenSBI; reads the serial console for each stage's line; passes only at `[fake-kernel] handoff ok`
- `.agents/skills/ours-verify/scripts/boot_check.py` — `target_cpu()` reads the build directory's `args.gn`, and `main()` hands a riscv one to `riscv_boot.run()`; `--timeout` defaults per mode (60 s x86, 10 s riscv)
- `.agents/skills/ours-verify/SKILL.md` — a "riscv" section: the command, what it builds and boots, the stage table, and what the mode does not do

**Symptom**
Nothing checked the riscv boot chain. Each piece of the riscv port (the device-tree shim, Sv39 paging, the phys arch layer, the fake kernel) had to be booted by hand with a QEMU command line and a timeout, and the result read off the raw console. The plan's integration step needs one command that says how far the chain got.

**Cause**
`boot_check.py` was written for x86 only: it boots through GRUB and `scripts/qemu_boot.py`, judges the dev images by the shell's `--- end ---`, and runs the host tests. None of that exists on riscv, where OpenSBI starts the shim directly and the chain ends at the fake kernel.

**Change**
The same command, a different check, chosen by what the build directory targets:

```python
if target_cpu(out, args.product) == "riscv":
    ...
    return riscv_boot.run(root, out, args.product, args.no_build, args.timeout)
```

`target_cpu()` reads `target_cpu = "..."` from `args.gn`, or takes the CPU half of `--product` for a directory not configured yet, so the default invocation (`out/bringup.x86`, `bringup.x86`) takes the x86 path exactly as before, and its output is unchanged.

`riscv_boot.py`:
- builds `kernel.phys_riscv.phys.dbg_0/obj/kernel/arch/riscv/phys/bootshim/bootshim.bin`, which must build, then `fake.obi`, which may not yet. A failed `fake.obi` is reported in one line (its first failed target and first error) and the shim boots without an initrd; an older `fake.obi` left on disk is never used, since it would report on code that is not this tree's.
- runs `qemu-system-riscv64 -machine virt -m 256M -smp 1 -nographic -bios default -kernel bootshim.bin [-initrd fake.obi]` with `DYLD_FALLBACK_LIBRARY_PATH` at `prebuilt/qemu/lib`, as `scripts/qemu_boot.py` does, and kills it once the verdict is in or the timeout runs out: nothing on riscv ends the machine.
- reports the furthest of six stages, each matched by the line its source prints: `OpenSBI v` (the firmware's banner), `[riscv-bootshim]` (`bootshim_main()`), `physload: loaded by '` or `Loaded phys module '` (physload's first line, printed only when the shim names itself in `bootloader_name`, and its last), `Kernel image at 0x` or `handoff: jumping to kernel at ` (physboot's first and last), `[fake-kernel] entered`, and `[fake-kernel] handoff ok`, the only pass.
- fails at once on `[fake-kernel] FAIL:`, a phys `[error]` line (panic), or OpenSBI's `sbi_trap_error: hart<N>`, and keeps reading 3 s for what follows.
- logs to `$TMPDIR/ours-verify-<dir>-<sha1 of the directory's path, 10 hex digits>/riscv.log`. The x86 mode's `ours-verify-<dir>` collides between worktrees that use the same directory name, which is why every lane passes its own `--out`; the riscv name cannot collide, since every worktree has an `out/bringup.riscv`.

Rejected: a separate `--arch` flag, which could disagree with the directory it names; and folding the riscv boot into `boot_check.py`'s `Boot` class, whose GRUB, monitor socket, NMI and required-line machinery has no riscv counterpart and would grow a branch at every step.

**Reference**
None, this project's own tooling. The stages follow the chain the plan names (`.agents/tasks/riscv-boot-chain/PLAN.md`), the QEMU command the riscv shim was brought up with, and the console strings in the sources listed above.

**Verification**
In `.worktrees/feat-riscv-fake`, `boot_check.py --out out/bringup.riscv`, exit 1, no traceback (the absolute build path shortened to `...`):

```text
build      ok (.../out/bringup.riscv: bootshim.bin, 0.1s)
fake.obi   not built, so the shim boots without an initrd (0.3s): FAILED: [code=1] user-riscv/obj/system/lib/launch/src/launch.bootfs.cpp.o / clang++: error: argument unused during compilation: '-fno-plt' [-Werror,-Wunused-command-line-argument]
riscv      FAIL  no "[fake-kernel] handoff ok" within 10s · reached: shim · no initrd
  next stage: physload, shown by /physload: loaded by '|Loaded phys module '/
```

- `--out out/riscv-fake --product bringup.riscv` on an unconfigured directory configures it, builds the shim and reaches `shim`.
- `--out out/bringup.riscv --only dbg` stops with `error: --only picks an x86 image; a riscv build directory boots one chain`, exit 2.
- `Boot` driven with the runtime device tree as a stand-in initrd boots with `-initrd` and reaches `shim`; fed the six stage lines it reaches `handoff ok` and passes; fed `[error] panic` after the shim's line it fails with `1 failure line(s)`.
- `target_cpu()`: `out/bringup.x86` and `out/x86-fake` give `x86`, `out/bringup.riscv` gives `riscv`.
- x86, `--out out/x86-fake`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 150 host tests pass, the same lines as before the change.
- The stages past `shim` have not been seen on riscv: the shim does not load physload yet, and `fake.obi` does not build.

**Deliberately not done**
- Host tests in the riscv mode: they are host binaries, the same for either target, and the x86 check runs them.
- An NMI-style lockup report for a silent riscv boot: there is no kernel on riscv to answer one yet.
- Moving the x86 mode's log directory to the collision-free name: its path is in every x86 summary, and that output is to stay as it is.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the check refuse an older `fake.obi` when the current one fails to build, rather than boot whatever is on disk?
- Why does the physload stage accept two different lines, and when would only the second one appear?
- Why must the log directory's name include the build directory's whole path on riscv, when the x86 mode gets by with its last component?
