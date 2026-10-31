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

## Title: header_lists.py holds //kernel/lib/arch, the generic half of the <arch/...> headers, to its list by default, as it holds each CPU's half (2026-10-31)

**Files**
- `scripts/header_lists.py` — `DEFAULT_SCOPE` gains the prefix `//kernel/lib/arch:`; the docstring says what the default scope is and why the two halves share it.
- `scripts/test_header_lists.py` — three tests: the generic half's missing header is found by the default scope, the prefix reaches that directory's target and no neighbour of it, and the summary line names both prefixes (27 tests become 30).
- `.agents/skills/ours-verify/SKILL.md` — the sentence on which targets the check holds, and the `FAIL` example line with the counts it now prints.
- `.agents/skills/ours-verify/scripts/boot_check.py` — the module docstring names the second prefix.
- `docs/repairs/261030/build-headers.md` — item 5 (widen the default to the whole tree) says what the default is today.
- `docs/changes/261031/ours-verify.md` — this entry.

**Symptom**
Latent, cannot be triggered today: `//kernel/lib/arch:headers` lists all 31 headers of its root. The condition is a header added to `kernel/lib/arch/include/arch/` and left out of that list, which `ours-verify` did not report before this change, on either build directory, while the same slip in `kernel/arch/x86/lib/arch/include/arch/` (or riscv's) failed the check.

**Cause**
`<arch/...>` is one include namespace that two directories provide. Each CPU gives the headers it has to write itself under `kernel/arch/<cpu>/lib/arch/include/arch/` (`//kernel/arch/x86:headers` is that root, 33 files), and the generic ones, which every architecture shares, are in `kernel/lib/arch/include/arch/` (31 files, `//kernel/lib/arch:headers`). The default scope was written as "the architectures' targets", `//kernel/arch/`, by directory, so it caught the first half and not the second, although a reader of `#include <arch/tlb.hpp>` cannot tell which half a header comes from and `gn check` treats both the same way: it holds an include to a dependency only for a header some target lists (`gn help check`: "includes matching unknown paths are ignored"), so an unlisted generic header is as invisible to it as an unlisted x86 one. The two halves were never meant to be held to different rules. The gap was an accident of where the directory is, and it is the kind of accident the first entry of `docs/changes/261030/ours-verify.md` exists to remove.

The prefix ends in a colon on purpose. A GN label is `//kernel/lib/arch:headers`, so `//kernel/lib/arch/` (the spelling that works for `//kernel/arch/`) is not a prefix of it and would match nothing, which the fourth entry of that file made an error and not a pass. A colon reaches the targets of that directory alone: `//kernel/lib/arch_foo:x` and `//kernel/lib/archive:x` do not start with `//kernel/lib/arch:`, and neither does `//kernel/lib/arch/sub:x`. It is the narrowest prefix that says "libarch's own header target", and the targets of the rest of `//kernel/lib/` (items 2 to 4 of `docs/repairs/261030/build-headers.md` are about them) stay out until those items are done.

**Change**
Before:

```python
DEFAULT_SCOPE = ("//kernel/arch/",)
```

After:

```python
# The architectures' targets: all of //kernel/arch/, and libarch's generic half,
# the one target of //kernel/lib/arch (a prefix ending in a colon reaches that
# directory's targets and not those of its subdirectories).
DEFAULT_SCOPE = ("//kernel/arch/", "//kernel/lib/arch:")
```

Nothing else in the code changed: `interfaces()` already takes a tuple of prefixes (`label.startswith(scope)`) and `summary()` already joins them, so the line now reads `... headers under //kernel/arch/ and //kernel/lib/arch:, every one listed`.

Rejected:
- `//kernel/lib/arch/` as the second prefix: matches no target, so it would fail the check at once (a scope that matches nothing is an error).
- `//kernel/lib/` for the whole of lib: it holds the debt of items 2 to 4 of the repair file (the 46 headers of mm are not here, but `//kernel/phys`, the small kernel targets and the system libraries are), and widening before they are fixed would fail every `ours-verify`.
- A second constant for "the generic half" and a flag to choose between them: one default that is the architectures' namespace is simpler to say, and `--scope` already picks anything else.

**Reference**
None, this project's own tooling. The two-directory layout of `<arch/...>` is the one `layering.md` ("Architecture, platform and drivers") describes for libarch: each architecture provides its headers under their public names from `kernel/arch/<cpu>/lib/arch/include`, and the generic part is `kernel/lib/arch`.

**Verification**
- `python3 scripts/test_header_lists.py`: 30 tests pass. The three new ones fail against the old constant (the same files copied to a scratch directory with `DEFAULT_SCOPE = ("//kernel/arch/",)`: the libarch test errors with `no cpp_interface target has a label under //kernel/arch/`, the other two fail; a fourth failure there, the test that the root is the checkout holding the script, is only the copy being outside a checkout).
- The scope alone, `header_lists.py out/bringup.x86 --scope //kernel/lib/arch:` and the same on `out/bringup.riscv`: `1 targets, 1 include roots, 31 headers under //kernel/lib/arch:, every one listed`, before the change as after.
- Failing with the right name, on both build directories, by deleting `"include/arch/tlb.hpp"` from `//kernel/lib/arch:headers` and putting the line back (`git status` of `kernel/` is empty afterwards): x86 `headers FAIL 1 missing, 0 stray among 8 targets, 8 include roots, 106 headers under //kernel/arch/ and //kernel/lib/arch: · 1.9s` and riscv `... 81 headers ... · 1.4s`, both with `missing //kernel/lib/arch/include/arch/tlb.hpp: under the include root of //kernel/lib/arch:headers, which does not list it`, exit 1. The same edit with `--scope //kernel/arch/` (the old default) on x86: `headers PASS 7 targets, 7 include roots, 75 headers`, exit 0: the gap this closes.
- The `FAIL` example in SKILL.md is the real output of removing `hpet.hpp` from `//kernel/arch/x86/kernel:headers` and adding `gone.hpp`, run on the x86 directory (file restored).
- x86 `boot_check.py`: build ok; dbg PASS shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 8 include roots, 106 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed · 1.8s (before: 7 targets, 75 headers); exit code 0.
- riscv `boot_check.py --out out/bringup.riscv`: riscv PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi; headers PASS 8 targets, 8 include roots, 81 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed · 1.4s (before: 7 targets, 50 headers); exit code 0.
- `lint_docs.py --changed` on SKILL.md, the repair file and this record: no violations.

**Deliberately not done**
- The rest of `//kernel/lib/` is not in the default scope: its gaps are items 1 to 4 of `docs/repairs/261030/build-headers.md`, and item 5 there (the whole tree as the default) is still open, with its wording updated.
- Generic header-only targets that include a header of the per-arch set stay item 6 of the same file: that is about includes `gn check` cannot resolve, a different gap from a list that does not name its files.
- No change to `cpp_interface.gni`, to `//kernel/lib/arch:headers` or to any header: the list was already complete.

**How it was found**
From the brief, which noticed that the riscv-mode work of the previous entry left the default scope naming a directory and not a namespace. `--scope //kernel/lib/arch:` on each build directory showed the target already passing (1 target, 31 headers), so widening the default costs no fix.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `<arch/tlb.hpp>` can come from `kernel/lib/arch` or from `kernel/arch/<cpu>/lib/arch`. Why does `gn check` treat an unlisted header of either the same way, and what did the old default scope therefore leave unguarded?
- Why does the new prefix end in `:` where the old one ends in `/`, and what would `//kernel/lib/arch/` have done when given to the check?
- The riscv directory reports 81 headers and the x86 one 106 under the same default scope. Which targets does each hold that the other does not (x86's `kernel`, `mm` and `phys` hold 29 headers, riscv's 4), and why is the generic half's 31 the same in both?
