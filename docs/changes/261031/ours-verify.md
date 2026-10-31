# ours-verify changes

## Title: The riscv mode of ours-verify runs the header-list check on its own build directory, and its exit code counts it (2026-10-31)

**Files**
- `.agents/skills/ours-verify/scripts/riscv_boot.py` — `run()` calls `header_lists.report(root, out)` after the boot and exits non-zero if either the boot or the header lists failed; the line that said the check was not run is gone. `run()` takes `no_tests`, so `--no-tests` skips the check as it does in the x86 mode.
- `.agents/skills/ours-verify/scripts/boot_check.py` — passes `args.no_tests` to the riscv mode; the module docstring says the riscv mode ends with the header lists.
- `scripts/header_lists.py` — the docstring's "Used by" paragraph names both callers; no code changed.
- `.agents/skills/ours-verify/SKILL.md` — the riscv section and the flags paragraph describe the new line, and the sentence about the x86 check's blind spot now says where it is covered.

**Symptom**
A riscv header could be added to `//kernel/arch/riscv/kernel`, `mm` or `phys` without listing it in the target's `public`, and `ours-verify` passed on both build directories: the x86 directory does not contain those targets, and the riscv mode printed "not run" and exited 0 on the boot alone. `docs/changes/261030/ours-verify.md` left this open twice: its first entry listed running the check in the riscv mode as "the user's call", and its fifth entry, which found that the riscv mode's closing line read as "x86 covers it", changed only the wording and put the choice to the user. The user chose to run it on 2026-10-31.

**Cause**
`gn desc` answers for one build directory, and each directory's graph holds the targets of its own `target_cpu`. `//kernel/arch/riscv:headers` is in both graphs because the host toolchain builds it for the host tests, but `//kernel/arch/riscv/{kernel,mm,phys}:headers` are defined only inside riscv's kernel and phys toolchains, which the x86 directory never instantiates. So a check that "runs the check" can still see none of them. The gap was a property of where the check ran, not of the check: `header_lists.py` already accepts any generated directory. Only a run that is pointed at the riscv directory can read riscv's lists, and the riscv mode is the one run that is pointed there.

Why it matters that nothing fails otherwise: ninja never reads a `public` list, and `gn check` holds an include to a dependency only for a header some target lists ("includes matching unknown paths are ignored"). A riscv header that no list names is therefore never checked for who may include it, and nothing says so.

**Change**
`run()` collects one result per check and ands them, as the x86 `main()` already does:

```python
# before
ok = boot.report()
print(f"{'headers':<10} not run in the riscv mode, and the x86 check does not see "
      f"riscv's own header targets: run scripts/header_lists.py on this build directory")
return 0 if ok else 1

# after
results = [boot.report()]
if not no_tests:
    results.append(header_lists.report(root, out))
return 0 if all(results) else 1
```

`report()` prints the same `headers` line the x86 mode prints, so one line format serves both. Rejected: running the check before the boot. The boot is the mode's reason to exist and its verdict is the line a reader looks for first; a list failure is cheap to report after it. Rejected: a riscv-specific scope. The default scope, `//kernel/arch/`, already picks the riscv targets in the riscv graph: its 7 targets are riscv's `kernel`, `mm`, `phys` and top-level `headers` plus three x86 targets the host toolchain builds, 50 headers, where the x86 graph's 7 hold riscv's top-level `headers` alone and 75 headers.

**Reference**
None, this project's own tooling. The rule the check enforces is the one `gn help check` states for header ownership, and the targets it reads are the ones `build/cpp/cpp_interface.gni` makes; no Zircon function is a counterpart of running it per build directory.

**Verification**
- riscv `boot_check.py --out out/bringup.riscv`: `riscv PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi`, then `host-tests not run in the riscv mode; the x86 check runs them`, then `headers PASS 7 targets, 7 include roots, 50 headers under //kernel/arch/, every one listed · 1.3s`.
- The failure path, by misspelling the one entry of `//kernel/arch/riscv/kernel:headers` (`aspace_layout.hpp` to `aspace_layut.hpp`) and restoring it, with `--no-build`: `headers FAIL 1 missing, 1 stray among 7 targets, 7 include roots, 50 headers under //kernel/arch/ · 1.3s` with `missing .../aspace_layout.hpp` and `stray .../aspace_layut.hpp` named, exit code 1, although the boot line above it still said PASS. The same directory with `--no-tests`: no `headers` line, exit code 0. `git status` of `kernel/` is empty afterwards: the file is back to its committed text.
- x86 `boot_check.py`: build ok; dbg PASS shell finished at 5.8s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.7s; exit code 0.
- `lint_docs.py --changed` on this record and SKILL.md: no violations.

**Deliberately not done**
- The `host-tests` line still says the riscv mode does not run them: the tests are host-toolchain executables of the one default build and run once, on x86. Running them again from the riscv directory would repeat the same binaries.
- `--no-tests` now has something to skip in the riscv mode and does. That is the flag's stated meaning ("skip the host tests and the header lists"), and the loop-of-boots use is the same there.
- The first header-lists entry is not edited: it is history, and this one answers the question it ended on.

**How it was found**
Reported by an independent review (see the previous entries of `docs/changes/261030/ours-verify.md`); the decision to close it was the user's on 2026-10-31.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `//kernel/arch/riscv:headers` is in the x86 graph and `//kernel/arch/riscv/kernel:headers` is not. What makes a target appear in a build directory's graph, and which run can therefore read riscv's lists?
- The riscv boot passed and the headers line failed in the same run. Why is the exit code the conjunction of the two and not the boot's alone, and what did the old "not run" line leave a reader to assume?
- Why does a missing entry in a `public` list fail nothing in ninja, and what is the only thing in the build that reads the list?
