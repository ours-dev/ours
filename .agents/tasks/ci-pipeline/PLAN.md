# ci-pipeline: implementation plan for the CI and release workflows

**Date**: 2026-11-17
**Status**: Accepted

## Request
The user (2026-11-17): the project has no CI/CD; build it. Documents first, as a task; work starts only after the user has reviewed them. The user-facing document (`DESIGN.md`) stays short; this plan carries the implementation detail.

In scope: a pinned, scripted toolchain; one local entry point for CI; a CI workflow on GitHub Actions; a release workflow publishing boot images; the documentation that tells agents CI exists.

Out of scope: everything listed under non-goals in `DESIGN.md` (branch protection, other architectures, NUMA/SMP variants, `ktest all`, coverage, benchmarks, Linux hosts).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1 runner | (a) GitHub-hosted macOS arm64; (b) self-hosted on the user's Mac; (c) hosted Linux | (a) | None | (a), 2026-11-17 |
| D2 triggers | (a) baseline, main, PRs into them, manual; (b) every branch push | (a) | None | (a), 2026-11-17 |
| D3 toolchain source | (a) pinned upstream downloads; (b) snapshot of local `prebuilt/` | (a) | None | (a), 2026-11-17 |
| D4 CD scope | (a) artifacts only; (b) plus Release on `v*` tags; (c) plus nightly | (b) | None | (b), 2026-11-17 |
| D5 submodule access | (a) fine-grained PAT secret `SUBMODULE_TOKEN`; (b) deploy keys | (a) | None | (a), 2026-11-17 |

Mirrors `DESIGN.md`, where the reasons are. The steps below assume the recommendations; where a different answer changes a step, the step says how.

## Steps
Each step is one commit, built and booted with `verify` (for steps that touch only scripts and YAML, `verify` still runs, because the scripts are what CI will trust). The user approves each step before it starts.

### Step 1: pin the toolchain in a manifest and fetch it with one script
What:
- Add `build/prebuilt.json`, one entry per tool: `name`, `version`, `url`, `sha256`, `strip_components`, `dest` (under `prebuilt/`), `keep` (glob list of what to keep after unpacking, to drop unneeded payload), and `kind` (`tar`, `zip`, `cipd`, `bottle`).
- Add `scripts/fetch_prebuilt.py` with `--dest <dir>` (default `prebuilt/`), `--only <name>...`, `--check` (report what is missing or stale, download nothing), `--force`. Shape copied from `scripts/fetch_ccache.py`: download to a temp file inside the destination, verify sha256, unpack into a sibling temp directory, rename into place, write an `.archive` stamp last. Absorb `fetch_ccache.py` as one entry; keep `fetch_ccache.py` as a two-line wrapper so the `verify` skill's instructions still work.
- Pins, resolved from what `prebuilt/` holds today:
  - clang: `llvm-23.1.0-rc3`, from `github.com/llvm/llvm-project/releases` (`LLVM-23.1.0-rc3-macOS-ARM64.tar.xz`). Keep `bin/{clang,clang++,clang-format,clang-tidy,ld.lld,lld,llvm-ar,llvm-cov,llvm-ifs,llvm-nm,llvm-objcopy,llvm-objdump,llvm-profdata,llvm-ranlib,llvm-readelf,llvm-size,llvm-strings,llvm-strip}`, `lib/clang/`, `include/`. Drop `clangd` and the rest (saves about 1 GB in the cache). Write `.versions/clang.cipd_version` as today, since `build/config/compiler/clang.gni` reads it. If rc3 is gone upstream, stop and ask the user whether to move to the final 23.1.0 (it changes every object, so it is a toolchain change, not a CI detail).
  - gn: CIPD `gn/gn/mac-arm64/+/git_revision:<full hash of e0a6ab04a113>` (version 2530). Resolve the full hash from the CIPD tag listing.
  - ninja: `1.13.2`, `github.com/ninja-build/ninja/releases` (`ninja-mac.zip`).
  - ccache: `4.14.1`, as pinned in `fetch_ccache.py`.
  - rust: the version behind today's `~/.cargo/bin/rustc` symlink (read `rustc --version`), from `static.rust-lang.org/dist/` as the combined `rust-<ver>-aarch64-apple-darwin.tar.xz`, installed with its own `install.sh --prefix=<dest>/rust/stable --components=rustc,cargo,rust-std-aarch64-apple-darwin`. Only the host target: the default build uses Rust only for `//build/rust:rust-toolchain-smoke-test($host_toolchain)`. This also replaces the symlinks into the user's home locally.
  - qemu and grub: Homebrew bottles from `ghcr.io/v2/homebrew/core/<formula>/blobs/sha256:<digest>`, anonymous token from `ghcr.io/token?scope=repository:homebrew/core/<formula>:pull`. The formula list is `prebuilt/qemu/.brew/*.rb` and `prebuilt/grub/.brew/*.rb`; pin each bottle by the digest for `arm64_<macOS codename>` that matches what is installed today (read each `.brew/*.rb` for its version). Unpack each bottle's `<formula>/<version>/` and merge `bin/`, `lib/`, `share/`, `libexec/` into `prebuilt/qemu/` (or `prebuilt/grub/`), which is the flat layout `scripts/qemu_boot.py` expects (`DYLD_LIBRARY_PATH=prebuilt/qemu/lib:prebuilt/grub/lib`). For qemu keep only `bin/qemu-system-x86_64`, `bin/qemu-img` and `share/qemu/` (firmware), drop the other system emulators (most of the 900 MB). Bottles carry `@@HOMEBREW_PREFIX@@` placeholders only in text files and pkg-config; the dylibs resolve through `DYLD_LIBRARY_PATH`, which is how the current tree works. If a binary refuses to run, re-sign ad hoc with `codesign -s - -f` (stated in the script's docstring as the reason).
  - gcc: not fetched. `cxx_compiler` defaults to clang and `prebuilt/gcc/mac-arm64` already says `unavailable-on-macos-host`.
- Why now: every later step and the CI runner need a toolchain that is not the user's hand-built `prebuilt/`.
- Verified: `python3 scripts/fetch_prebuilt.py --dest .worktrees/ci-prebuilt-probe/prebuilt` fills a scratch tree; a worktree whose `prebuilt` points at it builds `out/bringup.x86` and `boot_check.py` passes; `diff` of `clang --version`, `gn --version`, `ninja --version`, `rustc --version`, `qemu-system-x86_64 --version` against today's tree shows nothing. Add `scripts/test_fetch_prebuilt.py` (stamp handling, sha mismatch, `--check`) in the style of the existing `scripts/test_*.py`, using a local archive, no network.
- Commit: `build(prebuilt): pin the toolchain in a manifest and fetch it`.

### Step 2: one entry point for CI, proven in a clean clone
What:
- Add `scripts/ci.py`, subcommands:
  - `check --base <rev>`: the pre-commit checks over `git diff --name-only <base>...HEAD` instead of the index: `format_cpp.py --check`, `format_gn.py --check`, `lint_docs.py --changed`, on the changed paths. Reuse the path classification of `.githooks/pre-commit` by moving its suffix tables into `scripts/formatting.py` if they are not already shared there (check first; do not copy them). Also runs the `scripts/test_*.py` suite with `python3 -m unittest discover -s scripts -p 'test_*.py'`, which needs no build.
  - `verify [--out out/ci.x86]`: `tools/set.py bringup.x86 --out <out>` then `.agents/skills/verify/scripts/boot_check.py --out <out> --timeout <N>`, with `N` raised for a slower runner (start at 3x the local default; tune in step 3 from measured boot times). Exit code passes through.
  - `artifacts --out <out> --dest <dir>`: read `<out>/product_info.json` (GN metadata, so the image list is not kept by hand), copy each `kernel_images[].output_name` as `kernel.<output_name>.obi` plus its `.debug.json`, the unstripped ELFs named in the `verify` skill, and the boot log directory `boot_check.py` printed; write `SHA256SUMS`.
  - `all`: `check`, `verify`, `artifacts` in order, stopping at the first failure except that `artifacts` always runs to save logs.
- Make `tools/set.py` honour a ccache directory override from the environment (`CCACHE_DIR`), if it does not already: CI keeps the cache inside the workspace so the Actions cache can save it. Check `build/ccache.conf` first; the change is only needed if the directory is hard-wired there.
- Dry run in a clean clone: `git clone --recurse-submodules <origin> .worktrees/ci-dryrun` on `baseline/20260824`, `fetch_prebuilt.py` into it, `python3 scripts/ci.py all --base origin/main`. Not `scripts/new_worktree.py`, which links `prebuilt/` to the project root's and would hide exactly what this proves.
- Expected findings, each recorded in `docs/repairs/261117/ci.md` and fixed in its own commit before step 3, not inside this one: files the build reads that are not committed; submodule pins that differ from the working directories (memory notes `system/lib/*` hold unversioned work); absolute paths into the user's home; the stale `bits` entry in `.gitmodules` that has no gitlink.
- Verified: `ci.py all` exits 0 in the clean clone; `ci.py check --base HEAD~1` with a deliberately misformatted line exits non-zero and names the file.
- Commit: `build(ci): one entry point for the CI checks`.

### Step 3: the CI workflow
What: replace `.github/workflows/ci.yaml`:

```yaml
name: ci
on:
  push:
    branches: [main, "baseline/**"]
  pull_request:
    branches: [main, "baseline/**"]
  workflow_dispatch:
concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true
permissions:
  contents: read
jobs:
  verify:
    runs-on: macos-15          # arm64; D1 (b) changes this to [self-hosted, macOS, ARM64]
    timeout-minutes: 45
    env:
      CCACHE_DIR: ${{ github.workspace }}/.ccache
    steps:
      - uses: actions/checkout@<pinned sha>  # v4
        with:
          fetch-depth: 0                     # check needs the merge base
          submodules: recursive
          token: ${{ secrets.SUBMODULE_TOKEN }}
      - uses: actions/cache@<pinned sha>      # v4
        with:
          path: prebuilt
          key: prebuilt-${{ runner.os }}-${{ runner.arch }}-${{ hashFiles('build/prebuilt.json') }}
      - run: python3 scripts/fetch_prebuilt.py
      - uses: actions/cache@<pinned sha>
        with:
          path: .ccache
          key: ccache-${{ github.ref_name }}-${{ github.sha }}
          restore-keys: |
            ccache-${{ github.ref_name }}-
            ccache-baseline-
      - run: python3 scripts/ci.py all --base ${{ github.event.pull_request.base.sha || github.event.before }}
      - if: always()
        uses: actions/upload-artifact@<pinned sha>   # v4
        with:
          name: ours-${{ github.sha }}
          path: out/ci-artifacts
          retention-days: 14
```

- `fetch_prebuilt.py` always runs; on a cache hit it only checks stamps (seconds). On a new branch's first push `github.event.before` is all zeros; `ci.py check` treats that as "compare with `origin/main`".
- Actions pinned by commit sha, not tag (a tag can be moved).
- The ssh URLs in `.gitmodules` (`git@github.com:ours-dev/...`) do not work with a token; the checkout action rewrites github.com URLs to https with the token, which covers them. If it does not, add a step `git config --global url."https://github.com/".insteadOf git@github.com:` before checkout's submodule update (checkout with `submodules: false`, then `git submodule update --init`).
- User action needed before this step can pass: create the `SUBMODULE_TOKEN` secret (fine-grained PAT, repository access `ours-dev/lockdep` and `ours-dev/concurrent`, permission Contents: read-only).
- Verified: push a branch `ci/probe` with the workflow's `push.branches` temporarily including it, see green; push a deliberately broken commit (a failing host test assertion) and see red with the failing test in the log and the logs in the artifact; drop the probe branch. Record cold and warm run times and the billed minutes from the run's usage page in `RESULT.md`.
- Commit: `ci: build, test and boot on every push to the baseline`.

### Step 4: the release workflow
What: `.github/workflows/release.yaml`, on `push: tags: ["v*"]`, `permissions: contents: write`. Same steps as `ci.yaml` (shared through a reusable workflow, `ci.yaml` gaining `on: workflow_call`, so the two cannot drift), then `gh release create ${{ github.ref_name }} out/ci-artifacts/* --generate-notes --verify-tag`. With D4 (a) this step is skipped; with D4 (c) add a `schedule:` trigger that tags `nightly-<date>` and prunes all but the last seven nightly releases.
- Verified: tag `v0.0.0-ci-test` on a probe commit, see the release with images and `SHA256SUMS`, download one image and boot it locally with `scripts/qemu_boot.py`; delete the tag and release afterwards (ask the user before deleting, as it is outward-facing).
- Commit: `ci: publish boot images on a version tag`.

### Step 5: tell agents CI exists
What:
- `verify` skill: a short section saying CI runs `scripts/ci.py all`, which is `boot_check.py` plus the pre-commit checks, and how to reproduce a red run locally (`python3 scripts/ci.py all --base <rev>`), and that `fetch_prebuilt.py` is how a new machine gets `prebuilt/`.
- `CLAUDE.md` and `AGENTS.md` (kept in sync): one line under Repo conventions, "Toolchain versions are pinned in `build/prebuilt.json`; change a tool by changing its pin."
- Change record `docs/changes/261117/ci.md` written with each step, not at the end.
- Verified: `lint_docs.py --changed` clean.
- Commit: `docs(ci): point agents at the CI entry point`.

## Files
- `build/prebuilt.json`: new, the pinned toolchain.
- `scripts/fetch_prebuilt.py`: new, fills `prebuilt/` from the manifest.
- `scripts/test_fetch_prebuilt.py`: new, offline tests of the fetcher.
- `scripts/fetch_ccache.py`: changed, a wrapper over `fetch_prebuilt.py --only ccache`.
- `scripts/ci.py`: new, `check` / `verify` / `artifacts` / `all`.
- `scripts/formatting.py`: changed only if the pre-commit suffix tables need sharing.
- `tools/set.py` or `build/ccache.conf`: changed only if the cache directory is hard-wired.
- `.github/workflows/ci.yaml`: replaced.
- `.github/workflows/release.yaml`: new.
- `.agents/skills/verify/SKILL.md`, `CLAUDE.md`, `AGENTS.md`: a few lines each.
- `docs/changes/261117/ci.md`: new change record.
- `docs/repairs/261117/ci.md`: new, only if the clean-clone dry run finds problems.

## Test plan
- Step 1: scratch-tree build and boot, expected `boot_check.py` output: `dbg PASS`, `rel PASS`, `host-tests PASS`; tool versions identical to today's.
- Step 2: `ci.py all` in `.worktrees/ci-dryrun` exits 0; a misformatted line makes `check` exit 1 naming the file; `out/ci-artifacts/` holds `kernel.dev.obi`, `kernel.dev.dbg_1.obi`, `SHA256SUMS` and the logs.
- Step 3: one green and one deliberately red run on GitHub, both with artifacts; cold and warm durations recorded.
- Step 4: a probe tag produces a release whose image boots locally.
- Step 5: `lint_docs.py --changed` clean.
