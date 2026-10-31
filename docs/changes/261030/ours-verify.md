# ours-verify changes

## Title: boot_check.py checks that each architecture's cpp_interface lists every header of its include root, and its first run found one real gap, x86 libarch's pic.hpp (2026-10-30)

**Files**
- `scripts/header_lists.py` — new: asks GN for every `cpp_interface()` target (and the include root each publishes), compares its `public` list with the files under the root, and names each `missing` header (under the root, in no list) and each `stray` entry (in a list, not a file under the root). Standalone (`header_lists.py [OUT] [--scope PREFIX]... [--all]`) and as `report()` for `boot_check.py`.
- `scripts/test_header_lists.py` — new: 21 tests with a fake `gn` and a temporary tree.
- `.agents/skills/ours-verify/scripts/boot_check.py` — x86 mode runs `header_lists.report()` after the boots and the host tests, adds its result to the exit code, and `--no-tests` skips it with the host tests; the docstring says so.
- `.agents/skills/ours-verify/scripts/riscv_boot.py` — one more closing line: the header lists do not run in the riscv mode, and how to run them on a riscv directory.
- `.agents/skills/ours-verify/SKILL.md` — what the `headers` line checks and how to read it, `--no-tests`, the riscv mode, and a row in the failure table.
- `kernel/arch/x86/BUILD.gn` — `arch/x86/pic.hpp` joins the `headers` list of `//kernel/arch/x86:headers`, the one gap the check found in its own scope.
- `docs/repairs/261030/build-headers.md` — new: the same gap in 13 include roots outside `//kernel/arch/` (87 headers), in five claimable items.
- `docs/changes/261030/ours-verify.md` — this entry.

**Symptom**
Latent, cannot be triggered today: nothing fails at build or run time, which is the point. The previous entries of `kernel-arch-seam.md` found by hand that x86's headers target listed one header of 25, a gap that had been there since the target was made on 2026-08-24 (`feec7b72`). Nothing would have said so about the next target to fall behind. Run on HEAD before this change's own fix, the new check reports `missing //kernel/arch/x86/lib/arch/include/arch/x86/pic.hpp` for `//kernel/arch/x86:headers`, on the x86 build directory and on the riscv one (the host toolchain builds that target in both): the target lists 32 of its root's 33 files.

**Cause**
A `cpp_interface()` (`build/cpp/cpp_interface.gni`) makes a `source_set` with no sources, `configs = []`, a config named `<label>.config` whose one include directory is the root, and `public_configs` that publish it. The headers it lists in `public` are metadata for GN: ninja never reads them (the previous entry shows no header name in the target's `.ninja`), so a missing entry cannot fail a compile and a stale one cannot fail a link. What reads the list is `gn check`, and only for an include that names a file some target lists (`gn help check`: "includes matching unknown paths are ignored"). So a header no list names is a header every includer may include with no dependency and no warning; and a name left in a list after a rename is a stale claim nothing reads (`25c50029` repointed the one entry of x86's list at the renamed `hpet_regs.hpp`; had it missed, nothing would have said). Both kinds of mistake are silent, so the only protection is a check that compares the lists with the directories, and the lists are many (about 60 include roots on one build directory), so the check has to find them by asking GN rather than by being told.

What identifies a `cpp_interface` to GN is the shape the template gives it, and each part of the rule is there for a case the survey met:
- `type == "source_set"` and a config named `<its own label>.config` among its `public_configs`: the template's own config.
- No `sources`: `//vendor/lz4:lz4` and `//vendor/cxxopt:cxxopt` also publish a config named for themselves, and compile.
- `public` is a list: a source set that never set it has `public = "*"` in `gn desc`'s JSON, a string, which says "every file is public" and holds no list to compare.
- The root is the config's include directories (`gn desc <config>(<toolchain>)`, one call per target): there must be exactly one, and the check stops with an error when there are not, rather than guessing.

A root can be published by several targets: sampler (`:core.headers`, `:headers`, `:request`) and phys (`:headers`, `:handoff`, `:boot-obi`) each split one directory between targets so a dependant takes only the part it needs, and each target lists a part. Comparing each list with the whole root flagged every part of such a split, headers that another target of the split does list included (the first draft named `:request` for nine sampler headers that `:core.headers` lists); the rule that fits the design is that every header must be listed by at least one target publishing the root, so the lists are taken together, per toolchain (a list may differ by `if (toolchain_environment == ...)`). A `stray` entry is judged against the one target that lists it, since each entry has to be a file under its own root.

"Header" for `missing` is a file named like one (`.h .hh .hpp .hxx .inc .inl .ipp .tcc`; `ktl/xarray.tcc` is a template's out-of-line half that its header includes), because `//system/public/` is a root that also holds `BUILD.gn` and two `.gni` files, and they are not headers. `stray` tests against every file under the root, with the dotfiles left out, so an entry naming a file in the root with an unlisted suffix (a `.def`) is not wrongly called stray. The asymmetry is deliberate: "what must be listed" is narrower than "what may be listed".

The survey decided the scope. On the x86 build directory the first pass found 64 include roots and 18 of them with a file no target lists; the final tool over the x86 and riscv build directories finds 87 unlisted headers in 13 roots outside `//kernel/arch/` (`//kernel/mm:headers` lists none of 46, `//kernel/phys`'s three targets leave 15, and so on), with no stray entry anywhere. That is broad, so by the brief the check holds only the architecture targets, `//kernel/arch/` (seven labels on x86: `:headers`, `kernel:headers`, `mm:headers`, `phys:headers`, `lib/idle_states:headers`, `lib/page_tables:page_tables`, and riscv's `:headers` through the host toolchain; riscv's own `kernel`, `mm` and `phys` on the riscv directory), and the rest is `docs/repairs/261030/build-headers.md`. A label prefix, not a list of targets, so an architecture added later is held to the rule the day its `BUILD.gn` exists.

The one finding in scope is real. `//kernel/arch/x86:headers` (include root `kernel/arch/x86/lib/arch/include`) does not list `arch/x86/pic.hpp`. Nothing includes it (`git grep` finds no `#include` of it), and `arch-platform-seams/DESIGN.md` (I10) says `arch::Pic` may leave libarch for `platform/pc`. Neither argument is a reason to leave it out: listing states what is on disk, deletes nothing, and if the header moves the entry goes with it.

**Change**
The check, in the shape `boot_check.py` prints (`report()`):

```text
headers    PASS  7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.5s
```

and on a mismatch the same line says FAIL with the counts, and one line under it per problem:

```text
headers    FAIL  1 missing, 0 stray among 7 targets, 7 include roots, 75 headers under //kernel/arch/ · 1.5s
  missing //kernel/arch/x86/lib/arch/include/arch/x86/pic.hpp: under the include root of //kernel/arch/x86:headers, which does not list it
```

`boot_check.py`, x86 mode: after `tests.report()` it appends `header_lists.report(root, out)` to the results, so a mismatch makes the exit code 1 as a failing host test does. Before:

```python
    results = [b.report() for b in boots]
    if tests is not None:
        results.append(tests.report())
```

After:

```python
    results = [b.report() for b in boots]
    if tests is not None:
        results.append(tests.report())
        results.append(header_lists.report(root, out))
```

It runs once the boots are over rather than beside them: it takes about 1.5 seconds (`gn desc` calls eight at a time), so a thread would save nothing visible. It is under `--no-tests` because that flag is already "the loop of boots, never the check before a commit". The module is in `scripts/` beside `host_tests.py` and not in the skill's directory beside `riscv_boot.py`, because it is testable on its own with a fake `gn` like `test_host_tests.py` and runs on any build directory.

In `kernel/arch/x86/BUILD.gn` the list gains `"arch/x86/pic.hpp"` between `page_table.hpp` and `pmu.hpp`.

Rejected:
- A fixed list of targets to check: the structure (splits, defaults, libraries with sources) is in GN, and a hand-kept list is the kind of thing that fell behind in R8.
- Checking only each target's own list against the root (no union): wrong for sampler and phys, as above.
- Reading the `BUILD.gn` files with a regular expression: `headers = [...]` (prefixed with `include_dir`), `public = ours_public_headers` (a variable) and `if` blocks all produce the list, and `gn desc` has already evaluated them.
- Making the check a `gn` action in the build: the build would fail for a metadata mistake, and a build that cannot say which of 700 targets is wrong is a worse reporter than one line per file.

**Reference**
None, this project's own tooling. The rule it enforces is the one `gn help check` states for header ownership, and the template it reads is `build/cpp/cpp_interface.gni`; Zircon's `library_headers()` lists no headers at all, so it has no list to drift.

**Verification**
- `python3 scripts/test_header_lists.py`: 21 tests pass (a complete list, a missing file, a stray name, a real file outside the root, a root shared by two targets and judged by the union, the same problem in two toolchains reported once, a problem in only one toolchain found, dotfiles, header suffixes, a custom `include_dir`, scope, a target with sources, `public = "*"`, a target without its own config, a config with two include directories, and the printed lines).
- Failing with the right names, on `out/bringup.x86`, by editing `kernel/arch/x86/kernel/BUILD.gn` and the include root and putting everything back after each run (`git diff` of the file is empty afterwards and the directory has its 19 headers again):
  - one entry removed (`hpet.hpp`): `1 missing, 0 stray`, `missing //kernel/arch/x86/kernel/include/ours/arch/x86/hpet.hpp: under the include root of //kernel/arch/x86/kernel:headers, which does not list it`.
  - a new unlisted header in the root (`stray_probe.hpp`): `1 missing, 0 stray`, naming `.../ours/arch/x86/stray_probe.hpp`.
  - an entry naming no file (`not_there.hpp`): `0 missing, 1 stray`, `stray   .../ours/arch/x86/not_there.hpp: listed by //kernel/arch/x86/kernel:headers, and not a file under its include root //kernel/arch/x86/kernel/include/`.
  - an entry naming a real file outside the root (`BUILD.gn` beside it): `0 missing, 1 stray`, naming `//kernel/arch/x86/kernel/BUILD.gn`.
  - all four together: `2 missing, 2 stray`, the four names.
  - the same removal under `boot_check.py --only dbg --no-build`: dbg PASS, host-tests PASS, `headers    FAIL  1 missing, 0 stray among 7 targets, 7 include roots, 75 headers under //kernel/arch/ · 1.5s`, and the script's exit code 1.
- At HEAD before the `pic.hpp` entry: FAIL with `pic.hpp` named, on the x86 build directory (75 headers) and on the riscv one (50). After it, `header_lists.py out/bringup.x86`: `headers    PASS  7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.5s`; `header_lists.py out/bringup.riscv`: `headers    PASS  7 targets, 7 include roots, 50 headers under //kernel/arch/, every one listed · 1.2s`.
- `header_lists.py --all`, x86 build directory: 68 targets, 60 include roots, 549 headers, 102 findings in 13.4 s; riscv: 64, 56, 520, 103 in 12.7 s; 87 distinct headers in 13 roots between them, none stray. That is the repair file's whole content (46 + 17 + 12 + 12).
- The build did not change: after `gn gen`, all 711 x86 and all 707 riscv `.ninja` files are byte-identical to the copies taken before, and `compile_commands.json` has the same 752 and 638 entries. `gn check` passes on both and `--check-system` keeps its 37 (x86) and 22 (riscv) distinct pairs: listing `pic.hpp` adds no finding because nothing includes it.
- x86 `ours-verify` with the check in it: build ok; dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.5s; exit code 0. riscv check: `riscv PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi`, with the new closing line `headers    not run in the riscv mode; the x86 check runs it, or run scripts/header_lists.py on this build directory`.
- `lint_docs.py --changed` on the three documents: no violations.

**Deliberately not done**
- The other 13 roots (87 headers) are not fixed or checked by default: `docs/repairs/261030/build-headers.md` has them as five items, and the last widens the default scope once they pass.
- The riscv mode does not run the check; it prints how to. The brief said x86 mode, like the host tests; running it there too is one call in `riscv_boot.run()` and would hold riscv's hand-kept lists (the porter adds a header, the check says whether the list followed) at the cost of one more line in a mode whose check was deliberately kept small. It is the user's call.
- arm's `//kernel/arch/arm/kernel:headers` (`public = []`) is not in either build graph, so it is not checked; the day arm builds, its target falls under the same prefix.
- No check that the lists are sorted, that `include_dir` is on the toolchain's `-I`, or that a listed header is included by anything: this is only "the list is the root".
- `pic.hpp` is not moved, deleted or edited: the placement of board-chip headers in libarch is I10 of `arch-platform-seams`.
- The header-suffix set is not exhaustive (no `.def`, no extensionless C++ library headers): none occurs under a root today, and a missing suffix makes the check quieter, not wrong; `stray` does not depend on it.

**How it was found**
From the brief, which asked for it after R8 found the one-of-25 gap by hand. `gn ls` has no `config` type to enumerate and `gn desc` takes one label, so the first design was one `gn desc //* --format=json` (0.2 s, 1034 targets, 2.4 MB) for the targets and one `gn desc` of each target's `<label>.config` for its root, in parallel. The first survey ran on both build directories as a throwaway script; its false positives taught three of the four rules (union for split roots, `public = "*"` as a string, sources) and the suffix filter came from `//system/public/`'s `BUILD.gn` in the second.

**The user's prediction**
None. The brief expected that broad existing debt would force a scope of the architecture targets only, and that was so (13 roots, 87 headers outside it); it did not expect the architecture scope itself to fail at HEAD, and it did, on `pic.hpp`.

**Three questions you should be able to answer**
- A header is missing from a `cpp_interface`'s list. Why does nothing fail to build or check, and what exactly makes this check able to see it when `gn check` cannot?
- sampler and phys publish one include root through three targets each, and each target lists only part of it. What would comparing every list with the whole root have reported, and why is the union of the lists, taken per toolchain, the rule that matches their design?
- `missing` counts only files named like headers while `stray` tests against every file under the root. What goes wrong if both use the same set, in each direction?

## Title: header_lists.py finds the repository from where the script is, not from the working directory's git, because the second is another checkout's root or no root at all (2026-10-30)

**Files**
- `scripts/header_lists.py` — `repo_root()` returns the directory above `scripts/`; `main()` uses it where it ran `git rev-parse --show-toplevel`; the docstring says the build directory is the one of the checkout the script is in.
- `scripts/test_header_lists.py` — one new test (`Main`): from a working directory that is in no repository, `main()` hands `report()` the script's checkout and the resolved build directory.
- `docs/changes/261030/ours-verify.md` — this entry.

**Symptom**
Two ways to get it wrong, both from the standalone command and neither from `boot_check.py`. From a directory outside any repository, `python3 <abs>/scripts/header_lists.py <abs>/out/bringup.x86` ended in a traceback, `subprocess.CalledProcessError: Command '['git', 'rev-parse', '--show-toplevel']' returned non-zero exit status 128`, where every other failure of this check is one `headers    FAIL` line. From the directory of a different worktree (`.worktrees/feat-pmm-layering`, which still has the old one-of-25 lists), the same command with this worktree's build directory printed `headers    FAIL  26 missing, 0 stray among 6 targets, 6 include roots, 66 headers under //kernel/arch/`: a false failure, about the wrong tree's `BUILD.gn` files, with nothing in the line to say so.

**Cause**
The check compares two things that have to come from one checkout: the lists GN evaluated into the build directory (`gn desc <out>` reads the `BUILD.gn` files at the time of `gn gen`) and the files on disk under each include root (`files_under(root, ...)` walks `root / "kernel/..."`). `root` was `git rev-parse --show-toplevel` of the working directory, so it was the checkout the shell happened to be in, and nothing tied it to the build directory or to the script. When the shell is in the same checkout the two agree and nobody notices. When it is in another worktree the script reads that worktree's files against this worktree's lists (26 phantom `missing`), and `gn` itself is started from that worktree's `prebuilt/gn/gn` with that worktree as its source root. Outside a repository `git` has no answer and `check=True` turns that into an exception nobody caught.

`boot_check.py` was not affected for a reason that is easy to miss: it passes `root` in, from its own `repo_root()`, which tries git and falls back to the script's location (`parents[4]`). The standalone command had only the first half of that. The script already knows where it is: `scripts/header_lists.py` is one level below the root, and unlike the working directory that cannot differ from run to run. A script that is part of a checkout should name its checkout by where it is, and let a caller who wants another one run that checkout's copy.

**Change**
Before:

```python
    root = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True,
                               text=True, check=True).stdout.strip())
```

After:

```python
def repo_root() -> Path:
    # scripts/header_lists.py -> repo root
    return Path(__file__).resolve().parents[1]
...
    root = repo_root()
```

A relative `OUT` is still taken from the root (`root / args.out`), as before; an absolute one stays itself.

Rejected:
- Copying `boot_check.repo_root()` (git, then the script's location): for a script that lives in the checkout the git answer can only be right when it equals the location, and wrong otherwise, so the fallback would be the only part that ever mattered.
- Making `boot_check.repo_root()` use the location too: it has the same cwd-dependence (run from another worktree it builds that worktree's tree with this one's scripts), but it is the entry point of every `ours-verify` run and not part of this finding, so it is left (see Deliberately not done).

**Reference**
None, this project's own tooling. `scripts/host_tests.py` and the other helpers that take a `root` are called with the one `boot_check.py` found; this is the one helper with its own command line.

**Verification**
- `python3 scripts/test_header_lists.py`: 22 tests pass. The new one fails against the previous file with the same `CalledProcessError` (copied to a scratch directory and run there).
- From a directory that is in no repository: `headers    PASS  7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.5s`, exit 0 (before: the traceback).
- From `.worktrees/feat-pmm-layering`, with this worktree's absolute build directory: the same PASS line (before: `26 missing, 0 stray among 6 targets, 6 include roots, 66 headers`).
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 7 targets, 7 include roots, 75 headers under //kernel/arch/, every one listed · 1.5s.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.

**Deliberately not done**
- `boot_check.repo_root()` still asks git for the working directory's root. Run from another worktree it uses that worktree's root for the build, the images and `header_lists.report()`, while importing its helpers from the checkout the script file is in. It is the same kind of slip and a larger blast radius than this one, and a different file with its own callers, so it is a separate change.
- No `headers FAIL` line for a missing `git`: there is no `git` call left to fail.

**How it was found**
Reported by an independent review of the commit that added the check. It ran the command from a temporary directory and got the traceback, then from the detached worktree at an older commit and got the false failure. Reproduced here with the previous file copied to a scratch directory, so the same commands could be run against both versions.

**The user's prediction**
None.

**Three questions you should be able to answer**
- The check mixes a build directory and a checkout. Which fact in each came from `gn gen` time and which from the moment of the run, and what made 26 files "missing" when the script was run from another worktree?
- `boot_check.py` never showed this. What did it do that the standalone command did not, and what does it still do wrong?
- Why is "where this file is" a better answer than "where the shell is" for a script that lives inside the repository it works on, and when would the opposite be right?
