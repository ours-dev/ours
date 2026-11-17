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
