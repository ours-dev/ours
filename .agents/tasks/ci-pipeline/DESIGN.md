# ci-pipeline: run verify on every push to the baseline and publish the boot images

**Date**: 2026-11-17
**Status**: Accepted

## Problem
- There is no working CI. `.github/workflows/ci.yaml` is a 9-line stub cut off mid-line (`actions/checkout@v2`, no steps after it).
- "Done" is proven only by `verify` on the user's Mac. Nothing checks that a clean clone of `baseline/20260824` builds: `prebuilt/` is gitignored and hand-assembled (Rust is a symlink into `~/.cargo`, QEMU and GRUB are extracted Homebrew bottles), and the private submodules may differ from their working directories.
- The repo is private on GitHub, so hosted macOS minutes are billed at 10x.

## Goals and non-goals
Goals:
- Every push to `baseline/20260824` or `main`, and every PR into them, runs the same check as `verify`: build `bringup.x86`, host tests, boot both images.
- A fast check of what the push changed: C/C++ format, GN layout, document layout.
- The toolchain in CI comes from pinned upstream releases, by one Python script that also works locally.
- CD: the boot images and logs of every run are downloadable; a `v*` tag publishes a GitHub Release.

Non-goals:
- Branch protection or required checks (agents push the baseline directly).
- arm64 / riscv builds, NUMA and SMP variants, `ktest all`, coverage, benchmarks.
- Linux as a build host.

## Design
Objects:
- `build/prebuilt.json`: the pinned toolchain manifest (tool, version, URL, sha256).
- `scripts/fetch_prebuilt.py`: fills `prebuilt/` from the manifest; skips what already matches.
- `scripts/ci.py`: the one entry point CI calls, `check` / `verify` / `artifacts`; runnable locally.
- `.github/workflows/ci.yaml`: thin YAML that only checks out, restores caches and calls `scripts/ci.py`.
- `.github/workflows/release.yaml`: on a `v*` tag, the same run plus a GitHub Release.

Flow of one run:
1. Checkout with submodules, using a read-only token for the private `lockdep` and `concurrent` repos.
2. Restore `prebuilt/` from the Actions cache keyed by the manifest's hash; on a miss, `fetch_prebuilt.py`.
3. Restore the ccache directory (keyed by commit, falling back to the branch's latest).
4. `ci.py check` on the pushed range, then `ci.py verify` (wraps `boot_check.py`).
5. `ci.py artifacts`: images named by GN's `product_info.json`, plus boot logs; uploaded even on failure.

Estimated cost (hosted macOS arm64, unmeasured): about 10 minutes a run, billed as 100.

## Alternatives considered
- Linux runner: 10x cheaper, but the host toolchain, scripts and QEMU setup have only ever run on macOS; porting is a project of its own.
- Tarball of the user's `prebuilt/` as a release asset: fast to set up, but opaque, not reproducible, and its Rust symlinks point into the user's home.
- Docker dev container: the repo forbids it.
- YAML holding the logic: cannot be run locally, and the repo's tooling rule is Python.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1 runner | (a) GitHub-hosted macOS arm64; (b) self-hosted runner on the user's Mac; (c) hosted Linux | (a): a clean machine each run is the point of CI; (b) is free but heats the Mac and reuses the very `prebuilt/` CI should prove | None | (a), 2026-11-17 |
| D2 triggers | (a) push to `baseline/20260824` and `main`, PRs into them, manual; (b) also every branch push | (a): keeps minutes bounded | None | (a), 2026-11-17 |
| D3 toolchain source | (a) pinned upstream downloads via `fetch_prebuilt.py`; (b) snapshot of the local `prebuilt/` | (a) | None | (a), 2026-11-17 |
| D4 CD scope | (a) workflow artifacts only; (b) (a) plus a GitHub Release on `v*` tags; (c) (b) plus a nightly release | (b) | None | (b), 2026-11-17 |
| D5 submodule access | (a) fine-grained PAT, read-only on `lockdep` and `concurrent`, as secret `SUBMODULE_TOKEN`; (b) one deploy key per submodule | (a): one secret; the user creates it | None | (a), 2026-11-17 |

## Testing and verification
- `fetch_prebuilt.py` into a scratch `--dest`, then a build and boot against it, proves the pins equal today's toolchain.
- `ci.py` run end to end in a fresh clone under `.worktrees/ci-dryrun` before any YAML exists.
- A pushed branch with a deliberate break (format error, failing host test) must turn the run red; reverted after.
- Cannot be verified locally: runner speed, cache hit rates, the 10x billing.

## Open questions
- Whether LLVM 23.1.0-rc3 for macOS arm64 is still downloadable upstream, or the pin must move to 23.1.0 final.
- Whether the committed baseline builds from a clean clone at all; the dry run in step 2 answers it.
- The user's GitHub plan, which sets the monthly macOS budget (Free: 2000 minutes, about 20 runs).
