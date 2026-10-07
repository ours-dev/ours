# build-cache: one compiler cache shared by every worktree, and what it saves on a full build

**Date**: 2026-10-07

## Delivered
All on branch `build/cache`:

- `5e8379bf` (step 1): `-ffile-compilation-dir=.` in `//build/config:relative_paths`, so two worktrees compile a source to the same bytes; `scripts/lldb_ours.py` maps `../..` to its own checkout.
- `60b523cd` (step 2): `scripts/fetch_ccache.py` fetches ccache 4.14.1, a universal macOS binary, pinned by SHA-256, into `prebuilt/ccache/` (D1 (a)).
- `483c9c6f` (step 3): the `compiler_launcher` GN argument in front of every C, C++ and assembly compile, `build/ccache.conf` (`cache_dir = $HOME/Library/Caches/ours-ccache`, `max_size = 10G`, `sloppiness = locale`), switched on by `tools/set.py` and `scripts/new_worktree.py`.
- Step 4, the commit that adds this file: the measurement below, D4 settled, and `docs/repairs/261007/build.md` items 5 and 6.
- Step 5: nothing landed; D5 (c).

## Verification
How it was measured:

- One commit, `483c9c6f`, in two fresh worktrees made by `scripts/new_worktree.py` (`build/cache-cold` and `build/cache-warm`), each with `out/bringup.x86` configured by `tools/set.py`. Both were removed afterwards, with their branches.
- The cache was a new, empty directory, `out/tmp/ccache-step4` of the `build/cache` worktree, given to every compile by `CCACHE_DIR`, which overrides `cache_dir` from `build/ccache.conf` (`ccache -k cache_dir` showed it). The shared `~/Library/Caches/ours-ccache` was neither read nor emptied. The directory was removed afterwards.
- Three full builds, each `/usr/bin/time -l prebuilt/ninja/ninja -C <out>`, one run each, in this order:
  1. no cache: the cold worktree's `out/nocache.x86`, configured with `--args 'compiler_launcher=""'`;
  2. cold: the cold worktree's `out/bringup.x86`, the cache empty;
  3. warm: the warm worktree's `out/bringup.x86`, the cache holding what build 2 stored and nothing else, its statistics zeroed in between.
- All three were started the same way: by one Python driver that ran ninja with the agent shell's environment, after removing the four variables macOS's `python3` shim adds (`CPATH`, `LIBRARY_PATH`, `MANPATH`, `SDKROOT`; repairs item 3). Each waited for 30 s with no ninja, clang, ccache, linker or QEMU process on the machine; another session was booting QEMU when the run began.
- Host: Apple M5 Pro, 5 performance and 10 efficiency cores, 24 GB; ninja ran its default 17 jobs. The 1-minute load average at the three starts was 2.1, 6.8 and 9.4, the last two mostly the previous build's tail.

The builds (CPU is user plus sys; cores busy is CPU divided by wall):

| Build | Wall | User | Sys | CPU | Cores busy | Compiles: miss, direct hit, preprocessed hit |
|---|---|---|---|---|---|---|
| no cache | 31.6 s | 366.2 s | 38.8 s | 405.0 s | 12.8 | none cached |
| cold | 38.4 s | 419.7 s | 74.2 s | 493.9 s | 12.9 | 638, 1, 46 |
| warm | 6.6 s | 15.3 s | 17.2 s | 32.5 s | 4.9 | 0, 619, 66 |

- Warm against cold: 17% of the wall time and 6.6% of the CPU time. Against no cache: 21% and 8.0%.
- Cold against no cache: 22% more wall time (6.8 s) and 22% more CPU time (88.9 s, of which 35.4 s is sys).
- Each build ran 950 edges. 685 of them are the compiles ccache sees when it is on, and all 685 were cacheable.

Hit rate:

- Cold: 47 of 685 (6.9%), 46 of them from the preprocessor mode: compiles whose preprocessed text and code-generation flags equal those of another compile earlier in the same build.
- Warm: 685 of 685 (100%), 619 direct and 66 through the preprocessor. Both builds had the same environment, so the 66 are not item 3; their number changes from build to build (50 and 55 in step 3), as item 4, `-I` directories that exist or not by ninja's order, predicts. The warm build wrote 66 manifests for them.

Cache size:

- After the cold build: 94.4 MiB by ccache's count (`cache_size_kibibyte` 96652), 1318 files; 101 MiB on disk by `du`. The 687 objects themselves are 262 MiB; ccache stores them compressed.
- After the warm build: 96.0 MiB, 1382 files.
- So one full build of `out/bringup.x86` is about 0.1 GB of cache, and the 10 GB limit holds about a hundred.

Where the time goes, from each build's `.ninja_log`:

- No cache and cold: compiles fill all 17 jobs until the last one ends (29.6 s into the uncached build). Summed edge time of compiles: 458.2 s without the cache, 573.3 s cold.
- Warm: every compile is done 2.0 s in (687 edges that write an object, 28.2 s summed, about 41 ms each) and the six images are packed by 2.6 s. From 2.6 s to 6.6 s exactly one edge runs at a time, each a host test link.
- The 164 links of the build never overlapped, in any of the three builds: 5.6 s of the warm build's 6.6 s is links run one after another. Every link tool is in the `highmem` pool, whose depth on this machine is 1: item 5.

Why the cold build costs more than no cache: one kernel compile (`top.panic.cpp.o`), traced with `CCACHE_LOGFILE` into an empty cache, ran the preprocessor (89 ms) and only then the compiler (565 ms). Every miss pays that preprocessor run. Step 3's rebuilds put the preprocessor runs of all 685 compiles at about 80 s of CPU (105.4 s from preprocessed hits against 23.7 s from direct hits), most of the 88.9 s: item 6.

Boot: `boot_check.py` in the warm worktree, on the images its cached build produced: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), and `rel.log` has `pc-ps2: key a` and `pc-ps2: key A`. One run, two boots.

Against expectations:

- The user's prediction: none was given (PLAN.md, Decisions).
- The plan's Test plan expected "a warm build of `out/bringup.x86` dominated by links and packing, a small share of the cold one's CPU time". It held: 6.6% of the CPU, and links take 5.6 of the 6.6 s. Packing takes 0.4 s, alongside the links.

D4 settled at 10 GB, kept. The plan's reason for (a) was "a few builds' worth", which, read as a few copies of one full build, would be 0.3 GB. That is the wrong unit. The cache must hold every object the active worktrees still need: one object for each source that is the same everywhere, plus one per branch and per commit for each source that differs. A change to a header that most kernel sources include gives new keys to most of the 392 objects of the two kernel builds (197 and 195, step 1's counts), about 58 MiB of cache at the 0.15 MiB per stored object measured here. With several worktrees committing many times a day, 0.3 GB would evict objects that another worktree builds again the same day, which is the heat the cache exists to save. A larger limit costs disk only: a lookup reads one manifest and one result by key, whatever the cache holds. 10 GB is about a hundred full builds' worth, a few days of that churn.

Not measured: heat (`powermetrics` needs root); variance (one run per build, so a difference of a few percent, such as cold against no cache, is one sample).

## Deviations from the design
- A third build, without the cache, was added. The plan asked for cold and warm only. Without it the numbers say what the cache saves, not what it costs on a miss, and that cost is item 6.
- D4's number stands, but not for the plan's reason: see "D4 settled" above.
- The measurement worktrees were made by the project root's `scripts/new_worktree.py`, as the repository's rules say. That copy predates step 3 and does not configure a build directory, so `tools/set.py bringup.x86` was run in each by hand: the same call the branch's script makes.
- The cold build used a temporary cache, not an emptied `~/Library/Caches/ours-ccache`, so the shared cache other worktrees use was left as it was.

## Found along the way
In `docs/repairs/261007/build.md`:

- Item 5, from this step: every link of every toolchain is in the `highmem` pool, which allows one link at a time on this machine, because each link is budgeted 32 GB, the figure for an LTO link. It is most of a warm build's wall time.
- Item 6, from this step: on a miss ccache runs the preprocessor before the compiler, which makes a cold build cost 22% more than no cache, and every edit-build cycle is all misses.
- Items 1 to 4, from steps 1 to 3: lldb under `.worktrees/` reads the project root's sources; the `prebuilt` link shows as untracked; `CPATH` from macOS's `python3` splits the direct-mode keys; `-I` directories that exist or not by ninja's order do the same.

## Not done
- Step 5, background priority: dropped by D5 (c). PLAN.md step 5 records what its measurement found.
- Repairs items 1 to 6: each is a choice for the user.
- Repeated runs: one build of each kind.
