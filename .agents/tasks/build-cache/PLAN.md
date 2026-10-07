# build-cache: share compiled objects between worktrees, and keep builds and QEMU off the performance cores

**Date**: 2026-10-06
**Status**: Accepted

## Request
The user's machine gets hot whenever an agent builds, and several worktrees building at once is more than it can take (2026-10-06). The user asked for a cache like Bazel's, so that a second worktree does not compile again what the first already compiled, and for a plan with a schedule before any work starts.

In scope:
- a compiler cache shared by every worktree;
- what makes compiled objects identical across worktrees;
- running builds and QEMU at background priority.

Out of scope:
- caching links, OBI packing or QEMU boots;
- a remote cache;
- replacing GN or ninja.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1 cache tool | (a) ccache from its upstream release; (b) sccache from its upstream release, which also caches rustc; (c) none, only reuse worktrees | (a), if upstream ships a macOS arm64 binary; otherwise (b). ccache's direct mode answers a hit without running the preprocessor, the cheapest hit there is, and it needs no daemon | None | (a) if upstream ships a macOS arm64 or universal binary, else (b), 2026-10-07. It does, so (a): ccache 4.14.1, whose macOS archive is one universal binary (step 2) |
| D2 path independence | (a) `-ffile-compilation-dir=.` in the base compiler config; (b) ccache's `hash_dir = false` | (a): every worktree then produces the same object for the same source. (b) shares the objects but leaves the first worktree's absolute path in their debug info, which points the debugger at the wrong tree | None | (a), 2026-10-07 |
| D3 cache location | (a) `~/Library/Caches/ours-ccache`; (b) `out/` of the main worktree; (c) inside `prebuilt/` | (a): outside every worktree, so it survives removing one, and outside the repo | None | (a), 2026-10-07 |
| D4 cache size | (a) 10 GB; (b) ccache's 5 GB default | (a): one full build of `out/bringup.x86` is measured in step 4 first, and the limit is set to a few builds' worth | None | (a) 10 GB, 2026-10-07, to be adjusted after step 4's measurement with the reason recorded |
| D5 background priority | (a) background priority for the build, which puts it on the 10 efficiency cores; (b) `ninja -j5`; (c) neither, until the cache is in | (a) before measuring; (b) after, at 1.35x the wall time against (a)'s 6.5x (step 5, Revised) | None | (c), 2026-10-07: the cache is the fix; no priority change until it lands |

## Steps
Each step is one commit, and the user approves each before it starts (CLAUDE.md: one step at a time).

| Step | Work | Size | Depends on |
|---|---|---|---|
| 1 | Make objects independent of the worktree path | small | D2 |
| 2 | Fetch the cache tool into `prebuilt/` | small | D1 |
| 3 | Put the cache in front of every compile | medium | 1, 2, D3, D4 |
| 4 | Measure the cache | small | 3 |
| 5 | Run builds and QEMU at background priority | small | D5 |

Step 5 needs nothing from steps 1 to 4. It is the cheapest relief from the heat, so it may go first if the user prefers.

1. **Objects independent of the worktree path.**
   - What: add `-ffile-compilation-dir=.` to the base compiler config that every toolchain inherits (kernel, phys, user, host), for C, C++ and assembly.
   - Why now: clang records the absolute working directory in the debug info by default, so the same source compiled in two worktrees gives two different objects. A cache keyed on the command line either misses or hands out an object that names the other tree. GN's commands are already relative (`../../`), so this flag is the one thing missing.
   - Verified by:
     - building one commit in two worktrees and comparing a sample of objects with `cmp`, which must find them identical;
     - `boot_check.py`;
     - `scripts/lldb_ours.py` still listing the source of a kernel function, with relative paths resolved from `out/bringup.x86`.
   - Done (2026-10-07), branch `build/cache`:
     - `-ffile-compilation-dir=.` is in `//build/config:relative_paths`, for C, C++ and assembly.
     - Two worktrees at the same commit (`build/cache` and a throwaway `build/cache-twin`) built every object byte for byte the same: host 198, phys 44, kernel dbg.0 197, kernel dbg.1 195, user 49, default toolchain 4. Before the flag, a sample of 11, one object per compile rule, split by language: the 7 from C or C++ differed, by their `DW_AT_comp_dir` (the flag alone made them equal), and the 4 from assembly matched already, since assembly gets no `-g`.
     - `boot_check.py`: dbg and rel PASS, one run.
     - lldb listed nothing for `list vpanic` once the compilation directory became `.`, so `scripts/lldb_ours.py` now maps `../..` to its own checkout. With that, `list vpanic` shows `kernel/top/panic.cpp`. Under `.worktrees/`, lldb still shows the project root's copy, because it tries a relative path against its working directory before any mapping: `docs/repairs/261007/build.md` item 1.
2. **Fetch the cache tool.**
   - What: `scripts/fetch_ccache.py` downloads a pinned upstream release, checks its SHA-256, and unpacks it into `prebuilt/ccache/`. If upstream has no macOS arm64 binary, the script fetches sccache instead (D1 (b)).
   - Why now: the repo's rule is that toolchains come from upstream releases into `prebuilt/`, never from a system package manager.
   - Verified by: `prebuilt/ccache/ccache --version`.
   - Done (2026-10-07), branch `build/cache`:
     - D1 came out (a). ccache's latest release, 4.14.1 (2026-09-27), has one macOS asset, `ccache-4.14.1-darwin.tar.gz`: a universal binary (x86_64 and arm64), ad-hoc signed by the linker, linking only the system's libc++ and libSystem.
     - `scripts/fetch_ccache.py` pins it by SHA-256 `c279fa81…a464563`: GitHub's digest of the asset, equal to the hash of the downloaded file, whose minisign signature was checked once by hand against the public key on ccache.dev (key id `7C55E705DB25EF17`).
     - It unpacks into `prebuilt/ccache/`, which is the project root's through the worktree's link. `prebuilt/ccache/ccache --version` prints `ccache version 4.14.1`. A second run does nothing; `--force` replaces the tree; a wrong pin, and archive members that are links, absolute or outside the archive's top directory, are refused without touching `prebuilt/ccache/`.
     - `boot_check.py`: dbg and rel PASS, one run; the build is unchanged by this step.
3. **Put the cache in front of every compile.**
   - What: a GN arg, `compiler_launcher`, empty by default. When it is set, every C, C++ and assembly command in the toolchain templates is prefixed with it. A checked-in `build/ccache.conf` sets:
     - `base_dir` to the worktree root;
     - `cache_dir` per D3;
     - `max_size` per D4;
     - the sloppiness the repo needs, decided from the first misses.

     The launcher passes the config through `CCACHE_CONFIGPATH`, and `mkwt.py` and the build docs set the arg.
   - Why now: steps 1 and 2 make it possible; this makes it happen.
   - Verified by: `ccache -s` showing hits on a second worktree, and `boot_check.py`.
   - Done (2026-10-07), branch `build/cache`:
     - `compiler_launcher` is declared in `build/toolchain/compiler_launcher.gni`, empty by default. Set, it puts `/usr/bin/env CCACHE_CONFIGPATH=../../build/ccache.conf CCACHE_BASEDIR=<checkout root> <launcher>` in front of the `asm`, `cc` and `cxx` commands of `ours_toolchain()`, which every compiling toolchain comes from (default, host, user, phys, both kernels).
     - `build/ccache.conf`: `cache_dir = $HOME/Library/Caches/ours-ccache` (D3), `max_size = 10G` (D4), `sloppiness = locale`. `base_dir` is each checkout's own root, so it comes from the build in `CCACHE_BASEDIR`, not from the file.
     - Sloppiness: only `locale`. All 685 compiles were cacheable; the one miss the first builds showed was `LANG` (an agent's `C.UTF-8` against a terminal's `en_US.UTF-8` missed every object).
     - On by default: `tools/set.py` writes `compiler_launcher = "//prebuilt/ccache/ccache"` whenever that file exists, and `scripts/new_worktree.py` now configures `out/bringup.x86` through it. Chosen over a GN default that looks for the binary, which would need an `exec_script()` on every `gn gen` and would switch on the cache in directories already configured, the project root's included, at their next regeneration.
     - With the argument empty, `gn gen` wrote the same 643 `.ninja` files byte for byte, and ninja reran no compile.
     - Cold (cache emptied): build 37.6 s, 635 of 685 compiles missed. Warm, in a second worktree from `new_worktree.py`: build 6.6 s, 685 of 685 hits (635 direct, 50 through the preprocessor). One full build is about 0.1 GB of cache.
     - `boot_check.py`: dbg and rel PASS in both worktrees; four runs in all.
     - For step 4: two causes of a hit falling back from the direct mode to the preprocessor, `docs/repairs/261007/build.md` items 3 (`CPATH` from macOS's `/usr/bin/python3`, so builds from `boot_check.py` and from a shell look up different keys) and 4 (`-I<toolchain>/gen` existing or not, by ninja's order). A rebuild of every object took 23.7 s CPU from direct hits and 105.4 s from preprocessor hits, so step 4 should say which kind its warm build got and start both builds the same way.
4. **Measure.**
   - What: build one commit in a fresh worktree twice, cold (empty cache) and warm (cache filled by another worktree). For each, record wall time and CPU time (`/usr/bin/time -l`), the hit rate (`ccache -s`), and the cache size.
   - Why now: the size limit (D4) and the claim that this helps both need numbers.
   - Verified by: the numbers themselves, recorded in `RESULT.md` with the user's prediction beside them.
5. **Background priority.**
   - What: `boot_check.py` and `kshell_run.py` start ninja and QEMU under `taskpolicy -b` (D5 (a)), with a flag to turn it off.
   - Why now: QEMU emulates x86 in software on this ARM host, which no cache can help. Keeping it, and the links, off the performance cores is the only lever on the heat of a boot.
   - Verified by: one `boot_check.py` run each way, recording wall time and whether every check still passes. A test that times out on the slower cores is a finding to report, not a reason to drop the change.
   - Revised (2026-10-07): measured, then dropped by the user's choice of D5 (c). Nothing landed. What the measurement found:
     - **QEMU cannot run at background priority.** Its emulated timers then fire so late that the kernel's APIC calibration finds the tick device and the clock source more than a factor of two apart, logs an error, and `boot_check.py` fails both images. The calibration's fragility under a slow host is `docs/repairs/261007/x86-apic.md` item 1. Only the build was ever a candidate after that.
     - **`taskpolicy` cannot start QEMU at all.** macOS strips `DYLD_*` variables when it runs a protected system binary, and the prebuilt QEMU finds its libraries through `DYLD_FALLBACK_LIBRARY_PATH`. Without them it aborts with `Symbol not found: _cs_close`. A process can instead lower itself with `setpriority(PRIO_DARWIN_PROCESS, 0, PRIO_DARWIN_BG)`, and its children inherit that across fork and exec.
     - **The cost on the build.** Deleting the 91 host objects of `system/lib/ustl`'s tests and rebuilding them once each way gave:

       | Way | Wall | CPU | Mean cores busy |
       |---|---|---|---|
       | all cores, as today | 3.7 s | 20.5 s | 5.5 |
       | utility QoS (`taskpolicy -c utility`) | 4.0 s | 21.3 s | 5.4 |
       | all cores, `-j5` | 5.0 s | 16.7 s | 3.3 |
       | background priority, efficiency cores only | 24.1 s | 69.1 s | 2.9 |

       Heat itself was not measured: `powermetrics` needs root.

## Files
- `build/config/BUILD.gn` (or the compiler config it includes) — `-ffile-compilation-dir=.` (step 1)
- `scripts/fetch_ccache.py` — new (step 2)
- `build/toolchain/**` — the `compiler_launcher` arg and its prefix on compile commands (step 3): `compiler_launcher.gni` (new) and `ours/ours_toolchain.gni`
- `build/ccache.conf` — new (step 3)
- `tools/set.py`, `scripts/new_worktree.py` — turn the cache on for every build directory they configure (step 3)
- `.agents/skills/ours-verify/scripts/boot_check.py`, `scripts/kshell_run.py` — `taskpolicy -b` (step 5)
- `.agents/skills/ours-verify/SKILL.md` — how to turn on the cache and what the priority costs (steps 3 and 5)
- `.agents/tasks/build-cache/RESULT.md` — new, with the measurements (step 4)

## Test plan
- Step 1: identical objects across two worktrees (`cmp` on a sample of at least 20, from each toolchain); `boot_check.py` dbg and rel PASS; lldb lists a kernel function's source.
- Step 2: the tool runs and reports its version, and its SHA-256 matches the pinned one.
- Step 3: a warm build in a second worktree reports a hit rate near 100% for compiles; `boot_check.py` dbg and rel PASS; with the arg empty, the build is unchanged.
- Step 4: cold and warm numbers recorded; expected a warm build of `out/bringup.x86` dominated by links and packing, a small share of the cold one's CPU time.
- Step 5: `boot_check.py` PASS with and without the flag; wall times recorded.
