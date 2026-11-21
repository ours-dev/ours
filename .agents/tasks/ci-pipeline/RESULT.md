# ci-pipeline: every push builds, tests and boots on GitHub, and a version tag publishes a bootable release

**Date**: 2026-11-21

## Delivered
- Step 1, pinned toolchain: `build/prebuilt.json` and `scripts/fetch_prebuilt.py` (7e98c2e6).
- Step 2, one entry point: `scripts/ci.py` with `check`, `verify`, `artifacts`, `all`, `release`; `OURS_DIFF_BASE` lets the checkers diff against a merge base (110d8721).
- Step 3, the CI workflow: `.github/workflows/ci.yaml` on pushes and pull requests to `main` and `baseline/**` (a1c87544, 1c0ad539, 5e757d2e, 7c520f17, c6e36d3d).
- Step 4, the release workflow: `.github/workflows/release.yaml` on `v*` tags, calling `ci.yaml` in release mode; a release carries `bootshim` (52e53a42, 9da3678a, cfbd07d8).
- Step 5, agent-facing docs: a CI section in the `verify` skill and a pin line in `CLAUDE.md` and `AGENTS.md` (this commit).
- Made necessary along the way, in another session: APIC calibration trusts the clock source when ticks are lost (6e58e3d3).

## Verification
- Script tests: `scripts/test_fetch_prebuilt.py` (14) and `scripts/test_ci.py` (7) pass, and run in every CI `check`.
- Clean clone: `ci.py all` green on a fresh clone with a freshly fetched `prebuilt/` (step 2).
- CI, `baseline/20260824` at 0253cf5d, run 44727304051: green in 7 min 50 s with a cold ccache for the branch (build 322.9 s); dbg PASS, rel PASS 7/7 required; 185 host tests.
- CI, `ci/probe` at 6e58e3d3, run 44332760950: green in 2 min 43 s with warm caches (build 32.4 s).
- Cold toolchain fetch on the runner: 6 min 15 s; restore from cache: 13 s.
- Red runs (for example 44356406563) named the failing check and uploaded both the logs artifact (2.3 MB) and the ELF artifact (51 MB).
- Release, `v0.0.0-ci-test.2`, run 44761653590: green in about 16.5 min; 6 assets, `shasum -a 256 -c SHA256SUMS` OK; booted from the downloaded files alone: dbg PASS, rel PASS. Both probe tags and releases were deleted afterwards.
- Billed minutes: none; macOS runners are free for public repositories, and every `ours-dev` repository is public since 2026-11-18.

## Deviations from the design
- D5 (a submodule token) became moot: the user made every `ours-dev` repository public, so checkout fetches submodules anonymously.
- The runner is `macos-26`, not `macos-15`, because of the libc++ mismatch below.
- The deliberate red probe of step 3 was not run; five real red runs showed the same behaviour.
- Release mode skips `check`: a tag has no "before" commit, and the tagged commit was checked when it was pushed.
- `ci/**` branches also trigger CI, so a workflow change can be tried before it reaches the default branch.

## Found along the way
- Host tools pair LLVM 23's libc++ headers with the system libc++: `docs/repairs/261117/ci.md` item 1.
- `.gitmodules` still declares `system/lib/bits`: `docs/repairs/261117/ci.md` item 2.
- APIC calibration refused the timer on a loaded runner: fixed, `docs/changes/261118/x86-clock.md`.
- An older task document's Status failed `lint-docs` on the first baseline push: fixed in 0253cf5d.

## Not done
- Going back to `macos-15` (needs the libc++ repair item).
- Nightly releases (D4), branch protection and a status badge (non-goals).
- Tuning the release run's cold ccache (about 17 minutes); recorded in `docs/changes/261117/ci.md`.
