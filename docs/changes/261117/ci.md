# ci changes

## Pin the toolchain in build/prebuilt.json and fetch it with one script (2026-11-17)

**Files**
- `build/prebuilt.json` — new: every tool under `prebuilt/` pinned to an upstream archive and its SHA-256, with what to keep of it.
- `scripts/fetch_prebuilt.py` — new: fills `prebuilt/` (or `--dest`) from the manifest; `--check`, `--only`, `--force`, `--downloads`.
- `scripts/test_fetch_prebuilt.py` — new: 14 offline tests.
- `scripts/fetch_ccache.py` — now `fetch_prebuilt.py --only ccache`; the pin moved into the manifest.

**Symptom**
Latent, cannot be triggered today: nothing but the user's Mac can build the tree. `prebuilt/` is gitignored and was assembled by hand: clang from an LLVM release with files added later, gn and ninja from CIPD, QEMU and GRUB extracted from Homebrew bottles, and Rust as symlinks into `~/.cargo/bin`, i.e. whatever rustup's default toolchain is that day. A CI runner, or a second machine, has no way to get the same tree.

**Cause**
A toolchain that is not described anywhere cannot be reproduced; it can only be copied. Three things in the hand-built tree show how that drifts:
- `prebuilt/rust/stable/.versions/rust.cipd_version` says `rust-1.95.0`, but the symlinks run rustup's stable, which is 1.96.0. GN reads the stamp to key rebuilds on the toolchain version, so the key was wrong, and a `rustup update` would have changed the compiler again without touching it.
- The x86 compiler runtime (`lib/clang/23/lib/x86_64-unknown-none-elf/libclang_rt.builtins.a`) is not in the macOS LLVM release at all. It was taken from the Linux x86-64 release and renamed, which only its README recorded.
- GN reads `prebuilt/gcc/mac-arm64/.versions/gcc.cipd_version` unconditionally (`build/config/compiler/gcc.gni`), even with clang as the compiler, so `gn gen` fails on a tree without that one stamp file.

**Change**
The manifest describes each directory as sources plus files:

```json
"clang": {
  "dest": "clang/llvm-23",
  "sources": [
    {"url": ".../LLVM-23.1.0-rc3-macOS-ARM64.tar.xz", "sha256": "d23dc0ba...", "strip": 1,
     "keep": ["bin/clang", "bin/clang-23", ..., "include/c++", "lib/clang/23/include", ...]},
    {"url": ".../LLVM-23.1.0-rc3-Linux-X64.tar.xz", "sha256": "76ec17df...", "strip": 1,
     "keep": ["lib/clang/23/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a"],
     "rename": {"lib/clang/23/lib/x86_64-unknown-linux-gnu": "lib/clang/23/lib/x86_64-unknown-none-elf"}}],
  "files": {".versions/clang.cipd_version": "..."},
  "check": {"command": ["bin/clang", "--version"], "expect": "clang version 23.1.0-rc3"}}
```

The script's rules, each for a reason:
- A finished directory carries `.prebuilt.json`, the canonical JSON of its entry. Equal: left alone. Different: rebuilt. Absent: the directory was made by hand and is never replaced without `--force`, so running the script on the user's machine changes nothing there.
- A directory is built beside its destination and renamed into place only after every hash matched, no symlink dangles, and the tool reported the pinned version. An interrupted run leaves the old tree.
- Every `keep` prefix must match something, so an upstream layout change fails in the fetch, not in the build.
- Downloads compare the bytes received with `Content-Length` and continue a short one with a `Range` request. The user's HTTP proxy ends long transfers early without any error: curl and urllib both reported success on 23 MB of a 200 MB archive.
- Bottles come from ghcr.io with an anonymous token, which the script drops on the redirect to signed storage (urllib would forward it). They are not relocated: QEMU finds its libraries through `DYLD_LIBRARY_PATH`, which dyld searches by file name whatever directory a load command names (`@@HOMEBREW_PREFIX@@/opt/glib/lib/...`), which is how the hand-built tree already worked.

Pins: LLVM 23.1.0-rc3 and the Linux x86-64 release for the runtime; gn 2530 from CIPD by `git_revision`; ninja 1.13.2 and ccache 4.14.1 from their GitHub releases; Rust 1.96.0 from static.rust-lang.org (host target only, the only one `//:default` uses); QEMU 11.1.1 and GRUB 2.12 with their dependencies as `arm64_sequoia` bottles, the oldest macOS among the bottle platforms, so they run on a macOS 15 runner as well as on this macOS 27 host. QEMU keeps `qemu-system-x86_64` and `share/qemu` (425 MB instead of 900 MB); clang drops `clangd` (669 MB in all).

Rejected: a tarball of the hand-built `prebuilt/` as a release asset (opaque, and its Rust is symlinks into one user's home); `brew install` on the runner (installs into system directories, and resolves versions on the day); keeping the pins in each fetch script (`fetch_ccache.py`'s way), since a CI cache key then has to hash several files instead of one.

**Reference**
None, this project's own tooling. Fuchsia pins its prebuilts the same way in `integration/` jiri manifests resolved through CIPD; Chromium's `DEPS` with `gclient sync` is the same idea. The Homebrew bottle lookup follows Homebrew's own `GitHubPackages` download strategy (`Library/Homebrew/download_strategy.rb`, `CurlGitHubPackagesDownloadStrategy`).

**Verification**
- `python3 -m unittest scripts/test_fetch_prebuilt.py`: 14 passed, on Python 3.9.6 (the host's `python3`) and 3.13.
- Fetched every tool into `.worktrees/ci-prebuilt-probe/prebuilt`; `--check` then reports all nine current.
- Every file of the fetched `clang/llvm-23` is byte-identical to the hand-built one (SHA-256 per file), the x86 runtime included; only `bin/clangd` and the runtime's README are absent, by design.
- Versions printed by both trees are identical: clang 23.1.0-rc3 (7196f931), gn 2530 (e0a6ab04a113), ninja 1.13.2, ccache 4.14.1, rustc 1.96.0, QEMU 11.1.1, GRUB 2.12, mtools 4.0.49.
- A worktree at the baseline with `prebuilt` linked to the fetched tree, built cold with `compiler_launcher=""`: build ok in 48.0 s; dbg PASS (46 passed, 5 skipped), rel PASS (44 passed, 8 skipped, 7/7 required), host-tests PASS (185 passed).
- On the hand-built tree, `fetch_prebuilt.py --check` and `fetch_ccache.py` leave every directory alone ("was not made by this script").
- Download times through the user's proxy are not representative: GitHub release assets came at about 80 KB/s, about 1 MB/s without it. Not measured on a CI runner yet (step 3).

**Deliberately not done**
- The riscv64 compiler runtime: upstream publishes no riscv64 build, the hand-built one was compiled locally from compiler-rt, and CI builds x86 only. Fetching it means building it, which needs cmake in `prebuilt/`.
- `scripts/fetch_llvm_coverage.py` is left as it is: it finds `llvm-cov` and `llvm-profdata` already in place now, since the manifest keeps them.
- No replacement of the user's hand-built `prebuilt/`; the user may run `fetch_prebuilt.py --force` to switch, which also turns Rust from symlinks into a pinned copy.
- No automatic re-pinning: the docstring says how to look up a bottle's digest by hand.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does a missing `.prebuilt.json` stamp make the script refuse to replace a directory, instead of treating it as stale?
- How can a download through a proxy report success yet be wrong, and what in an HTTP response lets the client notice?
- Why do unrelocated Homebrew bottles still load their libraries here, and what would break if `scripts/qemu_boot.py` stopped setting `DYLD_LIBRARY_PATH`?

## One CI entry point, scripts/ci.py, proven in a clean clone (2026-11-17)

**Files**
- `scripts/ci.py` — new: `check --base`, `verify`, `artifacts`, `all`; what the CI workflow will call.
- `scripts/test_ci.py` — new: 7 tests on throwaway repositories and build directories.
- `scripts/gitdiff.py` — the revision "what changed" is measured from is `OURS_DIFF_BASE` when set, HEAD otherwise.
- `scripts/lint_docs.py` — lists changed documents against that same revision.
- `docs/repairs/261117/ci.md` — new: the stale `bits` entry in `.gitmodules`.

**Symptom**
Latent, cannot be triggered today: the pre-commit checks could not run in CI at all. `format_cpp.py` and `lint_docs.py` judge "the lines you changed", measured as `git diff HEAD`. On a CI runner the checkout is the pushed commit with a clean working tree, so the diff is empty and every file reads as unchanged. Shown in the dry run on a commit with a misformatted line:

```text
$ python3 scripts/format_cpp.py --check kernel/lib/ktest/src/runner.cpp
kernel/lib/ktest/src/runner.cpp: clean: no changed lines
```

**Cause**
The checkers answer "what did I change" with one hard-wired reference point, HEAD, which is right for an edit (the working tree against the last commit) and for a commit (the index against its parent), and wrong for a push, where the change is a range of commits that already ends at HEAD. Only checking whole files would avoid the question, and that is not an option here: about a sixth of the C++ is not clang-format clean, so a whole-file check fails every push for code nobody touched.

**Change**
The reference point became a variable with HEAD as its default:

```python
# before (gitdiff.py)
["git", "diff", "-U0", "-M", "--no-color", "HEAD"]
# after
["git", "diff", "-U0", "-M", "--no-color", base_rev()]   # OURS_DIFF_BASE or HEAD
```

`ci.py check --base REV` sets it to the merge base of REV and HEAD, so a branch is judged on its own commits and not on what its base gained meanwhile, and passes the changed files to the same three checkers the pre-commit hook runs, then runs the scripts' unit tests. A REV of forty zeros (GitHub's "before" for a branch's first push) means `origin/main`. Two differences from the hook, on purpose: a checker that cannot run fails the check (on a runner that is a broken setup, not a laptop missing a tool), and `all` runs every stage even after one fails, so one run reports a format slip and a failing boot together.

`ci.py artifacts` takes the image list from the build's `product_info.json` and the unstripped ELFs from each image's `.debug.json`, both GN metadata, so a new image needs no edit to the script. Each `.debug.json` carries one record for the image itself (`obi`, `image`, `label`) besides the ELF records; the first dry run failed with `KeyError: 'debug'` on it, which the test fixture now includes.

Rejected: `git reset --soft <base>` before checking (works on a runner, but makes the command destructive to run locally, and the point is that it runs the same on both); an argument on every checker (three interfaces and the hook to change, for what is one setting of the whole run).

**Reference**
None, this project's own tooling. `git clang-format <commit>` takes the same reference point as an argument for the same reason.

**Verification**
- `python3 -m unittest scripts/test_ci.py`: 7 passed; the checker suites (`test_lint_docs`, `test_format_cpp`, `test_gitdiff`, `test_precommit`): 96 passed.
- Clean clone of `baseline/20260824` at 7e98c2e6 with submodules into `.worktrees/ci-dryrun`, `prebuilt` linked to the step-1 fetched tree, these scripts added as one commit, `python3 scripts/ci.py all --base HEAD~1`: check PASS (4 files changed, no C++/GN/docs among them; script tests PASS), build ok in 69.7 s (cold cache), dbg PASS (46 passed, 5 skipped), rel PASS (44 passed, 8 skipped, 7/7 required), host-tests PASS (185 passed), artifacts PASS (32 files, 140 MB, 1:43 end to end).
- All six submodules resolved at their pinned commits from their own remotes, the private `lockdep` and `concurrent` over SSH included.
- Negative: a commit adding a misformatted function to `runner.cpp` and a skipped heading level to this file makes `check --base HEAD~1` exit 1, naming only the new lines (`runner.cpp: formatted: would change`; `ci.md:78: heading-level`).
- Not verified: anything on a GitHub runner (step 3).

**Deliberately not done**
- The `bits` entry in `.gitmodules` is left; it is recorded in `docs/repairs/261117/ci.md`, since removing a dependency declaration is the user's call.
- The full-history clone of `vendor/nlohmann_json` failed twice through the local HTTP proxy (`curl 18`, early EOF); a shallow `git submodule update --depth 1` without the proxy took 7 s. That is this machine's network, not the repository, so nothing changed for it; GitHub's checkout action fetches submodules shallow anyway.
- No fix to the one host test that fails in the shared baseline checkout: it comes from another session's uncommitted `kernel/lib/ktest` edits, and the committed tree passes all 185.

**How it was found**
The clean clone was the experiment: everything the build needs had to come from git and the step-1 manifest. The expected failures (uncommitted files the build reads, submodule pins that differ from the working directories, absolute paths into a home directory) did not appear. The diff-base gap was found by reading `gitdiff.repo_diff` while designing `check`, and confirmed by the negative test above.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does a checker that compares against HEAD see nothing on a CI runner, and why is the merge base, not REV itself, the right thing to compare against?
- Why must the changed-lines check stay changed-lines in CI instead of checking whole files?
- What would a test of `artifacts` have to contain to catch the `KeyError` the first dry run hit?

## The CI workflow runs on GitHub, green on the baseline (2026-11-20)

**Files**
- `.github/workflows/ci.yaml` — replaced the 9-line stub: checkout with submodules, a venv with `scripts/requirements.txt`, the prebuilt and ccache caches, `python3 scripts/ci.py all`, artifacts.
- `.agents/tasks/ci-pipeline/DESIGN.md`, `PLAN.md` — decision D6 (artifact upload).

**Symptom**
Latent until the first run: nothing had ever built this tree on a machine other than the user's Mac.

**Cause**
Each of the five red runs before the first green one found something a clean macOS machine does not share with the user's:
- A secret meant for the private `lockdep` submodule reached the job empty, twice: the repository had no Actions repository secret. Resolved by the user making every `ours-dev` repository public (2026-11-18), after a scan of their full histories for credentials found none; the token step was deleted.
- The runner's Python had no PyYAML, so `scripts/test_activate.py` could not import `activate.py`. The job now makes a venv from `scripts/requirements.txt` (the runner's Homebrew Python refuses a plain `pip install`).
- On macos-15 the host tool `obit` failed to link: `std::__1::__hash_memory` is called by LLVM 23's libc++ headers but exported only by the libc++ of macOS 26 and later. The runner moved to `macos-26`; the real fix is item 1 of `docs/repairs/261117/ci.md`.
- On macos-26 both boots failed: the loaded 3-core runner dropped periodic ticks and APIC calibration refused the timer. Fixed in the kernel, the Linux way (`docs/changes/261118/x86-clock.md`, commit 6e58e3d3).
- The first baseline push (87 commits) failed `lint-docs` on an older task document whose Status the linter does not accept, fixed in 0253cf5d: a push is checked over its whole range, so the first push of a long-unpushed branch also checks documents committed long ago.

**Change**
The workflow only checks out, restores caches and calls `scripts/ci.py`, so every failure above was read from `ci.py`'s summary lines and reproduced locally with the same command. Actions are pinned by commit. The prebuilt cache is saved right after the fetch, not at the end of the job, so a red run does not download 3.5 GB again; ccache is saved on every run. Images, logs and `SHA256SUMS` are uploaded every run (2.3 MB); the unstripped ELFs (51 MB zipped) only when a run fails (D6). `ci/**` branches also trigger the workflow, because `workflow_dispatch` appears only once the file is on the default branch and a change to the workflow needs somewhere to be tried.

**Reference**
None, this project's own tooling.

**Verification**
- `ci/probe` at 6e58e3d3, run 44332760950: green, 2 min 43 s with warm caches (build 32.4 s from ccache; dbg and rel PASS; 185 host tests).
- `baseline/20260824` at 0253cf5d, run 44727304051: green, 7 min 50 s (45 files checked; build 322.9 s with a cold ccache for that branch; dbg PASS 46 passed, rel PASS 44 passed, 7/7 required; 185 host tests; 32 artifact files).
- Red runs produced what a red run should: run 44356406563 named the failing check and uploaded both the logs artifact (2.3 MB) and the ELF artifact (51 MB); the ELF artifact is absent from the green runs.
- Cold toolchain fetch on the runner: 6 min 15 s, then 13 s to restore from cache.
- The deliberate red probe of the plan was not run: the real red runs above showed the same thing.

**Deliberately not done**
- The libc++ fix that would let the runner go back to macos-15 (repair item, a build-system change of its own).
- The intermittent `shell: sampler read 0 records` seen once in run 44356406563 is in its own session, not here.
- No branch protection (non-goal).

**How it was found**
Five successive GitHub runs, each read through `ci.py`'s summary lines and, for the boots, the logs artifact downloaded with `gh run download`.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did a host tool link on the user's Mac but not on a macOS 15 runner, though both used the same pinned compiler?
- Why is the prebuilt cache saved immediately after the fetch instead of at the end of the job?
- Why can the first push of a branch fail a document check that every later push would pass?
