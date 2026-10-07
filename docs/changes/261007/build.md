# build changes

## User code keeps a frame pointer in leaf functions (2026-10-07)

**Files**
- `build/config/BUILD.gn` — `default_frame_pointers` picks the new `frame_pointers_in_leaves` (`-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`) in the user toolchain; the kernel and phys keep `frame_pointers`
- `build/toolchain/ours/elf_link.py` — the new flag is a compiler-driver flag, kept off the linker's command line
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `RESULT.md`, `.agents/skills/ours-kernel-design/references/task.md` — the decision

**Symptom**
Every user sample taken in a leaf function lost that function's caller: profile's stacks read `spin_inner` → `main`, with `user_work` missing.

**Cause**
User code was built with `-momit-leaf-frame-pointer`, so a leaf did not push a frame. The sampler walks user stacks by frame pointers, and in a leaf rbp still points at the caller's frame, so the first return address it reads is the caller's own, into the caller's caller.

**Change**
User code keeps a frame in leaf functions too; it costs a leaf call its `push %rbp; mov %rsp,%rbp` and the `pop`. The user weighed a build switch that keeps the frames only in a profiling build, and chose to keep them always, so that any build can be profiled as it is. A switch at run time was not an option: whether a function builds a frame is fixed when it is compiled. The kernel keeps `-momit-leaf-frame-pointer`, since its stacks are unwound through `.eh_frame`, which describes every instruction.

What still hides a caller is a function's first instructions, before its frame is pushed, and its last, after the pop: a frame-pointer walk cannot see a frame that is not there yet or any more.

**Reference**
None in the upstream kernel for this choice. Distributions that profile with frame pointers have made the same one; not checked here.

**Verification**
`spin_inner` in `bin/profile` now starts `push %rbp; mov %rsp,%rbp`; a kernel target's compile still carries `-momit-leaf-frame-pointer`. In a clean worktree at HEAD with only this change: a full build, and `boot_check.py`: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), `profile: 43 kernel samples, 8 user samples, 0 dropped`; 7 of the 8 user samples read `spin_inner` → `user_work` → `main` → `ort_start` → `_start`.

**Deliberately not done**
None.

**How it was found**
The user asked whether to keep leaf frames, and chose always.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a sample in a leaf without a frame lose the leaf's caller and not the leaf itself?
- Why could the frames not be switched on only while a session runs?
- Why does the kernel not need them?

## Title: objects record "." as their compilation directory, so two worktrees compile the same source to the same bytes (2026-10-07)

**Files**
- `build/config/BUILD.gn` — `relative_paths` adds `-ffile-compilation-dir=.` to `cflags` and `asmflags`, so every toolchain (kernel, phys, user, host) gets it
- `scripts/lldb_ours.py` — on load, maps the debug info's `../..` to the checkout the script lives in (`settings append target.source-map`)
- `.agents/tasks/build-cache/PLAN.md` — the Decided cells of D1 to D4, and step 1's progress
- `docs/repairs/261007/build.md` (new) — lldb under `.worktrees/` shows the project root's sources

**Symptom**
None, a planned change: step 1 of `.agents/tasks/build-cache/PLAN.md`. Before it, the same commit built in two worktrees gave different bytes for every C and C++ object, so a compiler cache shared between worktrees could not hand one worktree's object to another without handing out the wrong thing.

**Cause**
The objects:
- The build directory: `out/bringup.x86` inside a worktree. ninja runs every compile there.
- A compile command: GN writes every path in it relative to the build directory (`-I../../kernel/include`, `-c ../../kernel/top/panic.cpp`), so the command line is the same text in any worktree.
- The debug info's compilation unit: `DW_AT_name` is the source path as the command gave it (`../../kernel/top/panic.cpp`); `DW_AT_comp_dir` is the directory that relative path is relative to. Clang fills `DW_AT_comp_dir` with the absolute working directory unless told otherwise.
- A compiler cache (step 3): it looks an object up by a hash of the compile's inputs and, on a hit, copies out the object it stored earlier.

The events, before:
1. Worktree A compiles `panic.cpp`. Its object says `DW_AT_comp_dir = /…/.worktrees/build-cache/out/bringup.x86`.
2. Worktree B compiles the same source with the same command. Its object says `…/build-cache-twin/out/bringup.x86`. Every other byte is equal, but the two files differ.
3. A cache has two choices, and both are bad. If it hashes the working directory, B misses and compiles again: nothing is shared. If it ignores it (ccache's `hash_dir = false`, D2 option (b)), B gets A's object, and B's debugger is pointed at A's tree, which may be on another branch.

The only absolute path in a compile is the one the compiler adds itself, so the fix is to stop it adding that one: with `-ffile-compilation-dir=.` the compilation directory is recorded as `.`, and the object no longer depends on where the worktree is. That is what makes option (a) of D2 strictly better than (b): the objects are not merely shared, they are the same.

The flag has a cost on the debugger side, and it is the part worth remembering. A debugger joins `DW_AT_comp_dir` and `DW_AT_name` to find a source file. With `.` and `../../kernel/top/panic.cpp`, the joined path is still relative, and lldb resolves a relative path against its own working directory, which is the repository root for every way this project starts lldb (`lldb_mcp.py` changes to it, `qemu_debug.py` prints a command run from it). From the root, `../../kernel/top/panic.cpp` is two directories too high: `list vpanic` printed the file name and no lines. So the change needs a second half: a mapping from the build directory's view to the checkout's. Every build directory is `out/<name>`, two below the root, so `../..` is the root, and one mapping covers every build directory.

**Change**
`build/config/BUILD.gn`, before:

```gn
config("relative_paths") {
  cflags = [ "-fmacro-prefix-map=//=" ]
}
```

After:

```gn
config("relative_paths") {
  cflags = [
    "-fmacro-prefix-map=//=",
    "-ffile-compilation-dir=.",
  ]
  asmflags = [ "-ffile-compilation-dir=." ]
}
```

`relative_paths` is in `default_common_binary_configs` (`build/config/BUILDCONFIG.gn`), which every toolchain's targets start from, so one line reaches all of them. `-ffile-compilation-dir` sets both `-fdebug-compilation-dir` and `-fcoverage-compilation-dir`, so a later coverage build stays path-independent too. Assembly gets no `-g` today, so it records no directory and its objects were already equal; the flag is in `asmflags` so that adding `-g` there later does not quietly bring the absolute path back.

`scripts/lldb_ours.py`, in `__lldb_init_module`:

```python
ROOT = Path(__file__).resolve().parent.parent
...
debugger.HandleCommand(f'settings append target.source-map ../.. "{ROOT}"')
```

Every way of starting lldb here already loads this script (`.lldbinit`, both launch configurations in `ours.code-workspace`, `lldb_mcp.py`, the command `qemu_debug.py` prints), and it loads before `target create`, so the mapping is in place for the kernel's target. The root is the script's own checkout, so a worktree's lldb maps to that worktree.

Rejected:
- `"." -> <build directory>` as the mapping: it would also cover generated sources (`gen/…`), but the script would have to know which build directory the target came from, and `out/.debug/kernel.elf` is a symlink whose target varies.
- Running lldb from the build directory instead of mapping: every launch path passes repository-relative paths (`target create out/.debug/kernel.elf`), which would all break.

**Reference**
None in Linux or Zircon's kernel code: this is build tooling. The build this one is modelled on, Fuchsia's GN build, has a config of the same name, `//build/config:relative_paths`, for the same purpose: source paths in debug info and `__FILE__` relative to the build directory, so the build does not depend on where the checkout is. Chromium's `//build/config/compiler:compiler_deterministic` passes `-ffile-compilation-dir=.` under its `strip_absolute_paths_from_debug_symbols` argument, for cache hits across checkouts.

**Verification**
- Before the flag, one object per compile rule built in two worktrees at `b414b4e6` (`build/cache` and a throwaway `build/cache-twin`): the 7 objects from C or C++ differed, the 4 from assembly matched. `dwarfdump` showed `DW_AT_comp_dir` as each worktree's absolute `out/bringup.x86`.
- After the flag, both worktrees built in full and every object was compared byte for byte (`cmp` on 20 per toolchain, and a whole-tree byte comparison):

  | Toolchain | Objects compared | Identical |
  |---|---|---|
  | host | 198 | 198 |
  | phys (`kernel.phys_x86.phys.dbg_0`) | 44 | 44 |
  | kernel (`kernel_x86.dbg.0`) | 197 | 197 |
  | kernel (`kernel_x86.dbg.1`) | 195 | 195 |
  | user (`user-x86`) | 49 | 49 |
  | default (`obj/`) | 4 | 4 |

  `dwarfdump` now shows `DW_AT_comp_dir (".")`.
- `boot_check.py`: build 33.9 s, dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), `pc-ps2: key a` and `pc-ps2: key A` in `rel.log`. One run.
- lldb (Xcode's, lldb-2103) on `kernel_x86.dbg.1/obj/kernel/products/bin/vmours`: without the script, from a directory where `../../kernel` does not exist, `list vpanic` printed `File: ../../kernel/top/panic.cpp` and no lines; with the script imported, it lists lines 202 to 212 of `kernel/top/panic.cpp`, and `settings show target.source-map` shows `"../.." -> "<checkout>"`.

**Deliberately not done**
- The `.worktrees/` case of the debugger: from a checkout's root under `.worktrees/`, `../../kernel/...` exists, in the project root, and lldb uses it before any mapping. That needs a choice between options, so it is `docs/repairs/261007/build.md` item 1, not a quiet fix here.
- Generated sources (`gen/…`, relative to the build directory, not to `../..`) are not mapped, and neither is a build directory deeper than `out/<name>`. Neither is stepped through today.
- The objects still hold one absolute path: Clang's own resource headers, at the project root's `prebuilt/clang/llvm-23/lib/clang/23/include`, because Clang finds them from its real path and every worktree's `prebuilt/` links to the project root's. So the objects are the same in every worktree `new_worktree.py` makes, but not in a checkout with a `prebuilt/` of its own somewhere else. Step 3 decides whether the cache needs to care.
- `-fmacro-prefix-map=//=` is left as it was. No path the compiler sees starts with `//`, so it looks like it maps nothing, and `__FILE__` is already relative (`../../kernel/...`); it does not affect this change.
- Links, and the absolute object paths a macOS host link writes into its executables, are outside the cache's scope (PLAN.md, Request).

**How it was found**
The lldb behaviour came out of the step's own check. `list vpanic` from the worktree's root printed source, which looked like success, but the path it read was `../../kernel/top/panic.cpp` from `.worktrees/build-cache`, which is the project root's copy. A decoy test settled which file lldb reads: a directory `x/a/b` with a fake `x/kernel/top/panic.cpp` of 300 lines saying `// DECOY`, lldb started in `x/a/b` with `settings set target.source-map ../.. <checkout>`, and `list vpanic` showed `// DECOY line 202`. lldb tries the relative path against its working directory first and consults `target.source-map` only when that fails.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does one absolute directory in the debug info stop a compiler cache from sharing objects between worktrees, when every path in the compile command is already relative?
- With `DW_AT_comp_dir` set to `.`, why can lldb find the right source from the main checkout but the wrong one from a checkout under `.worktrees/`?
- Why is ccache's `hash_dir = false` a worse fix than `-ffile-compilation-dir=.`, even though both give cache hits?

## Title: scripts/fetch_ccache.py fetches ccache 4.14.1, pinned by SHA-256, into prebuilt/ccache/ (2026-10-07)

**Files**
- `scripts/fetch_ccache.py` (new) — downloads upstream's macOS release of ccache, checks it against a pinned SHA-256, and unpacks it into `prebuilt/ccache/`
- `.agents/tasks/build-cache/PLAN.md` — D1's outcome and step 2's progress
- `docs/repairs/261007/build.md` — item 2: the `prebuilt` link that `new_worktree.py` makes shows as untracked

**Symptom**
None, a planned change: step 2 of `.agents/tasks/build-cache/PLAN.md`. The compiler cache of step 3 needs a ccache binary, and the repository had none: not on the host (`which ccache` finds nothing) and not in `prebuilt/`.

**Cause**
The objects:
- The release archive: `ccache-4.14.1-darwin.tar.gz`, one of the assets of ccache's GitHub release `v4.14.1`. Its top directory holds the binary, the manual, the licence, and an `install.sh` with a `patch-binary.py`.
- The binary: a Mach-O universal file with an x86_64 and an arm64 slice. The kernel runs the slice for the host's CPU, so the one file serves Apple silicon and Intel Macs. It links only `/usr/lib/libc++.1.dylib` and `libSystem`, so it needs nothing from a package manager.
- The pin: the SHA-256 of the archive's bytes, written into the script.
- The signature: the asset `ccache-4.14.1-darwin.tar.gz.minisig`, an Ed25519 signature by ccache's maintainers over a BLAKE2b hash of the archive.
- `prebuilt/`: the toolchain directory, ignored by git. A checkout under `.worktrees/` links its `prebuilt` to the project root's, so a fetch into it serves every worktree.

Why a pinned hash and not just a pinned version. A version names a release, but the bytes behind its URL can change: a maintainer can re-upload an asset, and a download can be cut short or altered on the way. Then two machines that "have ccache 4.14.1" run different binaries, and a build cache is exactly the tool where that matters, because it hands out objects the compiler did not just make. A hash names the bytes themselves: the script refuses anything else, so the archive in `prebuilt/ccache/` is the one that was reviewed when the pin was written.

What the hash does not do is say who made those bytes. It came from GitHub's own record of the asset (`gh api repos/ccache/ccache/releases/tags/v4.14.1`, field `digest`), and recomputing it on the downloaded file only shows the download matched what GitHub holds. Who put it there is answered by the signature: it verifies under the public key that ccache.dev publishes on its download page, a different host from GitHub's release storage, and changing one appended byte of the archive makes it fail. So the pin was written after three checks that agree: GitHub's digest, the hash of the download, and the maintainers' signature over that download. The script itself repeats only the first kind, which is the one that needs no key and no third-party library.

Why D1 is (a). The plan said ccache if upstream ships a macOS binary for this host, else sccache. Upstream ships exactly one macOS asset, and it is universal, so (a) holds without a fallback: ccache answers a hit in direct mode by hashing the source and its include files, without running the preprocessor, and it runs no daemon.

Why the archive is unpacked as it is and `install.sh` is not run. `install.sh` copies the binary through `patch-binary.py`, which overwrites two 4096-byte path slots in it: the system configuration directory (`/usr/local/etc`) and the libexec directory (`/usr/local/libexec`). Neither matters here. Step 3 points ccache at the repository's own configuration through `CCACHE_CONFIGPATH`, and when that variable is set the manual says it names the only configuration file ccache reads, so `/usr/local/etc/ccache.conf` is skipped even if one appears there. The libexec directory is where ccache looks for remote-storage helper programs, and this repository's cache is local (PLAN.md, Out of scope). Leaving the binary as upstream built it also keeps it the file the signature covers.

**Change**
`scripts/fetch_ccache.py`, the pin:

```python
VERSION = "4.14.1"
ARCHIVE = f"ccache-{VERSION}-darwin.tar.gz"
ARCHIVE_URL = f"https://github.com/ccache/ccache/releases/download/v{VERSION}/{ARCHIVE}"
ARCHIVE_SHA256 = "c279fa81e2e806b4d9e64d4a06cb569a84ef54151feef796e5d4f4e95a464563"
```

What a run does:
1. On a host that is not macOS, it stops and says so: only the macOS archive is pinned.
2. If `prebuilt/ccache/.archive` names the pinned archive and hash, it stops: the tree is current. `--force` skips this.
3. It downloads into a scratch directory inside `prebuilt/`, hashing as it writes, and stops if the hash is not the pin.
4. It unpacks into a staging directory in the same scratch directory, with the archive's top directory stripped, so the binary lands at `ccache` and not at `ccache-4.14.1-darwin/ccache`. Before unpacking it refuses any member that is absolute, contains `..`, lies outside the top directory, or is not a plain file or directory, and it masks every mode to `0755` at most. The interpreter here is the host's Python 3.9, which predates `tarfile`'s extraction filters, so these checks are written out; on a Python that has the filters, the `data` filter is applied as well.
5. It writes the stamp `.archive` into the staging tree, moves any old `prebuilt/ccache/` into the scratch directory, and renames the staging tree into place. Both renames stay inside `prebuilt/`, one file system, so neither copies, and the old tree is deleted with the scratch directory.
6. It runs `prebuilt/ccache/ccache --version` and fails unless the first line is `ccache version 4.14.1`.

The order is the point: nothing in `prebuilt/ccache/` changes until the download has matched the pin and unpacked in full, so a failed or interrupted run leaves either the old tree or none, never half of one.

Rejected:
- Reusing `scripts/activate.py`: it downloads files named in a YAML manifest and checks their hashes, but does not unpack, and it needs PyYAML, which a fresh host lacks. A self-contained script of the standard library runs on the host's Python as it comes.
- sccache (D1 (b)): it is the fallback for a missing macOS binary, and there is one. Its extra, caching `rustc`, does not pay here: the build's only Rust is three smoke-test files under `build/rust/`, against 565 C, C++ and assembly sources.
- Checking the signature in the script: Ed25519 is not in Python's standard library, and the `cryptography` package that checked it by hand is not a requirement of this repository.

**Reference**
None in Linux or Zircon's kernel code: this is build tooling. The pattern is how Fuchsia fetches its prebuilt tools: its `jiri` lock files pin each CIPD package by instance ID, which is a SHA-256 of the package, and the `cipd` client refuses a download whose hash differs. Upstream's own instructions for this archive are in its `install.sh` and in ccache's `doc/install.md`; the signature check follows the `minisign -Vm` command on ccache.dev's download page.

**Verification**
- Upstream's assets, from `gh api repos/ccache/ccache/releases/latest`: `v4.14.1`, published 2026-09-27, one macOS asset, `ccache-4.14.1-darwin.tar.gz`, 2217445 bytes, `digest: sha256:c279fa81…a464563`. `shasum -a 256` of the download gives the same value.
- `file` and `lipo -info`: universal, x86_64 and arm64. `otool -L`: `/usr/lib/libc++.1.dylib` and `/usr/lib/libSystem.B.dylib` only. `codesign -dv`: ad-hoc, linker-signed, which is what an arm64 Mac needs to run it.
- The signature: Ed25519 over the BLAKE2b-512 of the archive (minisign's hashed mode, `ED`), and the global signature over the trusted comment, both verify under the key `RWQX7yXbBedVfI4PNx6FLdFXu9GHUFsr28s4BVGxm4BeybtnX3P06saF` from ccache.dev (key id `7C55E705DB25EF17`). The same check on the archive with one byte appended raises `InvalidSignature`.
- `python3 scripts/fetch_ccache.py` on the empty `prebuilt/`: downloads, prints `prebuilt/ccache/ccache: ccache version 4.14.1`. Again: `already in place`, no download. With `--force`: downloads and replaces the tree, no scratch directory left in `prebuilt/`.
- With the pin replaced by zeros, `fetch(force=True)` raised `SHA-256 is c279fa81…, pinned 0000…`, and `prebuilt/ccache/` was untouched (stamp and binary as before). `stripped_members` refused a member with `..`, an absolute one, one outside the top directory, and a symbolic link, and kept `ccache` and `doc/x.md` from a good archive.
- `boot_check.py`: build ok (nothing to rebuild), dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), `pc-ps2: key a` and `pc-ps2: key A` in `rel.log`. One run.

**Deliberately not done**
- Nothing uses ccache yet. Putting it in front of the compiles, and its configuration, are step 3.
- Only macOS is pinned. The project builds on the macOS host and never in the dev container (CLAUDE.md), so a Linux archive would be pinned untested; the script says what to add if a Linux host ever needs it.
- `CCACHE_*` variables in a user's environment still override the configuration file even under `CCACHE_CONFIGPATH`. Whether step 3's launcher clears them is step 3's question.
- No lock against two fetches at once: the second one's rename fails and it reports an error; running it again finds the tree in place.
- The untracked `prebuilt` link in every worktree, noticed during this step, is `docs/repairs/261007/build.md` item 2, not fixed here.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- The script refuses an archive whose SHA-256 is not the pinned one. What attack or accident does that stop, and what does it not prove that the minisign signature does?
- ccache's macOS binary has `/usr/local/etc` written into it as its system configuration directory. Why can step 3 leave it there?
- Why does the script unpack into a directory inside `prebuilt/` and rename it into place, instead of extracting straight into `prebuilt/ccache/`?

## Title: every C, C++ and assembly compile runs through one ccache shared by all worktrees, turned on by tools/set.py (2026-10-07)

**Files**
- `build/toolchain/compiler_launcher.gni` (new) — the `compiler_launcher` argument, empty by default, and `compiler_launcher_args`, the words put in front of each compile
- `build/toolchain/ours/ours_toolchain.gni` — the `asm`, `cc` and `cxx` tools run their command behind those words, as GN's `command_launcher`, in every toolchain (kernel, phys, user, host)
- `build/ccache.conf` (new) — the cache's settings: `cache_dir`, `max_size`, `sloppiness = locale`
- `tools/set.py` — writes `compiler_launcher = "//prebuilt/ccache/ccache"` into `args.gn` whenever `prebuilt/ccache/ccache` exists
- `scripts/new_worktree.py` — configures the new worktree's `out/bringup.x86` through `tools/set.py`
- `.agents/skills/ours-verify/SKILL.md` — a "Compiler cache" section: how it is turned on and off, and how to read its statistics
- `.agents/tasks/build-cache/PLAN.md` — step 3's progress
- `docs/repairs/261007/build.md` — items 3 and 4: two ways a hit falls back from direct mode to the preprocessor

**Symptom**
None, a planned change: step 3 of `.agents/tasks/build-cache/PLAN.md`. Before it, every worktree compiled every source itself: about 40 seconds of all cores for `out/bringup.x86`, even when another worktree had compiled the same sources at the same commit minutes earlier.

**Cause**
The objects:
- A compile command: one line in a `toolchain.ninja`, for example `../../prebuilt/clang/llvm-23/bin/clang++ --target=… -MD -MF <obj>.d -o <obj> <flags> -c ../../kernel/top/panic.cpp`. ninja runs it in the build directory.
- A launcher: a program written in front of the compiler on that line. It is started instead of the compiler, gets the compiler's whole command line as its arguments, and decides whether to run it.
- ccache: such a launcher. It computes a key from what decides the object, and keeps objects in a directory under those keys.
- The direct mode: ccache's first lookup. The key hashes the compiler (path, size and modification time), the command line, the source file, and a few things about the environment. Under it sits a manifest: the include files an earlier compile read, with their hashes. If every listed header still has the same contents, the object is known without running anything. This is the cheap hit.
- The preprocessor mode: the fallback. ccache runs the compiler with `-E` and hashes its output, which already holds every header's text. It costs a preprocessor run, still far less than compiling.
- The cache directory, `~/Library/Caches/ours-ccache` (D3), and the configuration file `build/ccache.conf`.

The events of a cached compile:
1. ninja runs the line, which now starts `/usr/bin/env CCACHE_CONFIGPATH=../../build/ccache.conf CCACHE_BASEDIR=<checkout> ../../prebuilt/ccache/ccache clang++ …`.
2. `env` sets the two variables and runs ccache with the rest of the line.
3. ccache reads `build/ccache.conf` and computes the direct-mode key; on a hit it copies the stored `.o` and `.d` into place and exits.
4. On a miss it tries the preprocessor mode; on a miss there too it runs the compiler and stores the object and the manifest.

Why this hits across worktrees at all: step 1 made every object independent of the checkout's path, and the command lines were relative already. ccache hashes the compilation directory only for compiles with debug information, and then it hashes the directory the command names: its log says `Hashing CWD .`, the value of `-ffile-compilation-dir=.`, not the build directory's absolute path. So D2 (a) needs no `hash_dir = false` here.

Why `/usr/bin/env` and environment variables instead of options in the file. `CCACHE_CONFIGPATH` is how ccache is told which file to read; the alternative is the user's own `~/Library/Preferences/ccache/ccache.conf`, which is per machine, not per repository. `base_dir` cannot be in the file, because it has to be each checkout's own root. ccache rewrites an absolute path under `base_dir` to one relative to the build directory before hashing, so the same path in two worktrees hashes the same. A checked-in file has one value for every checkout, so the build passes `rebase_path(".", "", "//")`, its own root. Today no compile command holds an absolute path under the checkout (every `toolchain.ninja` was searched), so `base_dir` changes no key; it is there so that an absolute `-I` added later does not quietly split the cache by worktree.

Why it is on by default through `tools/set.py`, and not through the argument's default value. The argument's GN default stays empty, as the plan says, so a build directory nobody configured differently runs exactly the commands it ran before. Every build directory is configured by `tools/set.py`: by hand, by `boot_check.py` for a new `--out`, and now by `new_worktree.py`. So `set.py` writing the argument whenever the binary exists reaches every new build directory with a few lines, the same way it already writes `clang_tool_dir`, and `args.gn` shows whether a directory is cached. The rejected alternative, a GN default that looks for `prebuilt/ccache/ccache`, would need an `exec_script()` on every `gn gen`, since GN cannot test whether a file exists, and would switch the cache on in already configured directories, including the project root's, at their next regeneration, without anyone running anything.

Why `sloppiness = locale`. ccache hashes `LANG` and the `LC_*` variables, because a compiler may translate its diagnostics. A test with `LANG=zh_CN.UTF-8` missed an object that `LANG=C.UTF-8` had just stored: an agent's shell has `C.UTF-8`, a macOS terminal `en_US.UTF-8`, so the user's builds and the agents' would have filled two caches. The prebuilt clang has no translations, so the variables cannot change an object, and the setting is safe. No other sloppiness was needed: all 685 compiles were cacheable (no `unsupported_compiler_option`, `unsupported_code_directive` or similar counter above zero), so the default checks on `__DATE__`, `__TIME__` and newly modified headers stay on.

**Change**
`build/toolchain/compiler_launcher.gni` (new):

```gn
declare_args() {
  compiler_launcher = ""
}

compiler_launcher_args = []
if (compiler_launcher != "") {
  _checkout_root = rebase_path(".", "", "//")
  compiler_launcher_args = [
    "/usr/bin/env",
    "CCACHE_CONFIGPATH=" + rebase_path("//build/ccache.conf", root_build_dir),
    "CCACHE_BASEDIR=$_checkout_root",
    rebase_path(compiler_launcher, root_build_dir),
  ]
}
```

`build/toolchain/ours/ours_toolchain.gni`, the C++ tool, before:

```gn
      command =
          "$cxx $target_flag $switches {{cflags}} {{cflags_cc}} -c {{source}}"
      restat = true
      if (restat_cc) {
        command =
            string_join(" ", restat_command_launcher.local) + " " + command
      }
```

After (`cc` the same; `asm`, which has no restat launcher, sets only `command_launcher`):

```gn
      command =
          "$cxx $target_flag $switches {{cflags}} {{cflags_cc}} -c {{source}}"
      restat = true
      if (restat_cc) {
        command = string_join(
                      " ",
                      restat_command_launcher.local + compiler_launcher_args) +
                  " " + command
      } else if (compiler_launcher_args != []) {
        command_launcher = string_join(" ", compiler_launcher_args)
      }
```

Why the launcher goes in GN's `command_launcher` and not into `command`. The objects:
- `command`: the text GN writes into the ninja rule, and also into `compile_commands.json`, the compile database `.gn` exports on every `gn gen`.
- `command_launcher`: a prefix GN writes into the ninja rule in front of `command`, with one space, and leaves out of the compile database. GN's help describes it as the place for a compiler cache.
- The compile database's readers: the editor's clangd (`.vscode/settings.json` points it at the root's `compile_commands.json`, a link into `out/bringup.x86`) and `clang-tidy -p out/<dir>` (`cpp-write-and-verify`). Both take the first word of a command as the compiler driver and find the C++ standard library relative to it.

A first version of this change put the launcher into `command`. The rule ninja runs was right, but every entry of the compile database then started with `/usr/bin/env`. clangd took `/usr/bin/env` as the driver, found no libc++ beside it, and for host files fell back to the macOS SDK's (`/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1`) instead of `prebuilt/clang/llvm-23/include/c++/v1`, which the build compiles against; it also lost `--driver-mode=g++`. Nothing failed: the files tried parsed with the same diagnostics either way, so the editor and clang-tidy would have checked host code against a different standard library than the build's, silently. With `command_launcher`, ninja runs exactly the same text (every `.ninja` file is byte for byte the first version's), and the database is what it was without the cache.

The restat launcher (`restat_cc`, off by default and set nowhere) is the one case that still goes into `command`: it has to be outermost, to see the outputs whether the cache or the compiler wrote them, and a `command_launcher` would land in front of it, so ccache would run the Python script as if it were the compiler. With it on, the cache follows it inside `command`, as the first version did for every compile.

`build/ccache.conf` (new): `cache_dir = $HOME/Library/Caches/ours-ccache` (D3; ccache expands `$HOME`), `max_size = 10G` (D4, until step 4's measurement), `sloppiness = locale`.

`tools/set.py`: a `compiler_launcher()` that returns `//prebuilt/ccache/ccache` when the file exists and `--args` does not set `compiler_launcher`, otherwise `None`, with a note on stderr when the binary is missing. `set_build()` writes the line after `clang_tool_dir`. The value is a GN path, so the same line is right in every checkout, each resolving it through its own `prebuilt/`.

`scripts/new_worktree.py`: after the hook, `python3 tools/set.py bringup.x86` in the new worktree, its progress output dropped (the script's one line of output is the path) and its notes passed on; a failure is reported, not fatal, since `boot_check.py` configures an unconfigured directory itself.

Rejected:
- A Python wrapper instead of `/usr/bin/env`: a Python start costs tens of milliseconds per compile, as much as a direct-mode hit, and the repository's rule that tooling is Python is about scripts people run, not a per-compile exec.
- sccache: D1 settled on ccache.

**Reference**
None in Linux or Zircon's kernel code: this is build tooling. Linux's `Documentation/kbuild/llvm.rst` has a "Ccache" section: `make CC="ccache clang"`, the same launcher idea through `CC`. Chromium's GN build has the equivalent argument, `cc_wrapper` (`//build/toolchain/cc_wrapper.gni`), which its toolchain templates put in front of each compile command; this argument follows it, with the configuration file and `base_dir` added because the cache is shared by several checkouts.

**Verification**
- Argument empty: the 643 `.ninja` files `gn gen` wrote after the change were byte for byte the ones it wrote before it, and a `ninja` run after the regeneration started no compile, only the 36 actions that assemble and pack the images, which run on every build because their depfiles are absent.
- Argument set: `asm`, `cc` and `cxx` carry the prefix in all six toolchains (default, host, `user-x86`, `kernel.phys_x86.phys.dbg_0`, `kernel_x86.dbg.0`, `kernel_x86.dbg.1`); no other command in any `.ninja` file runs clang, and the link commands are unchanged.
- The compile database, argument set: all 658 entries start with `../../prebuilt/clang/llvm-23/bin/clang++` or `clang`, and as a set they equal the entries with the launcher removed. Moving the launcher from `command` to `command_launcher` left all 643 `.ninja` files byte for byte the same, so a configured directory recompiles nothing. With `restat_cc = true` as well, `gn gen` puts `restat_cacher.py --outputs ${out} --` first, then the launcher, then the compiler. `clangd --check` on a host file through the first version's database showed `-internal-isystem` of the SDK's libc++; through the fixed one, the prebuilt's.
- Cold, in `build/cache` with the cache emptied (`ccache -C`) and every object deleted, through `boot_check.py`: build 37.6 s; 685 compiles, all cacheable, 635 misses and 50 hits. Those 50 came from the preprocessor mode, which means another compile in the same build had already produced the same preprocessed text under a different command line. The cache then held about 0.1 GB.
- Warm, in a second worktree made by `scripts/new_worktree.py` (whose `args.gn` had the `compiler_launcher` line without anyone running `set.py`), through `boot_check.py`: build 6.6 s; 685 of 685 hits, 635 direct and 50 preprocessed, 0 misses.
- `boot_check.py` in both worktrees: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), `pc-ps2: key a` and `pc-ps2: key A` in `rel.log`. Four runs in all: the pair above, and the same pair before `sloppiness = locale` was added (cold 42.7 s, warm 6.4 s, 685 of 685 hits).
- `sloppiness = locale`: one object rebuilt under `LANG=C.UTF-8`, `en_US.UTF-8` and `zh_CN.UTF-8` was one miss and two direct hits; without it, `zh_CN.UTF-8` after `C.UTF-8` was a miss.
- Direct and preprocessed hits compared: every object of the warm worktree deleted and rebuilt, all from the cache: 5.9 s wall and 23.7 s CPU when all were direct hits, 11.3 s and 105.4 s with the direct mode switched off (`CCACHE_NODIRECT=1`).

**Deliberately not done**
- `CCACHE_*` variables in the environment are not cleared. Nothing here sets one (`env` shows none), and the ones a person would set on purpose are the useful ones: `CCACHE_DISABLE=1` runs the compiler directly for one build, `CCACHE_RECACHE=1` replaces stored objects. The two the build relies on, `CCACHE_CONFIGPATH` and `CCACHE_BASEDIR`, are set on every compile's line, so a stray value of either cannot leak in.
- Two causes of a hit going through the preprocessor instead of the direct mode, both found while reading the first misses, are recorded rather than fixed, because each fix is a choice about the build: `CPATH`, which macOS's `/usr/bin/python3` adds to the environment of every build a Python script starts (`docs/repairs/261007/build.md` item 3), and `-I` directories that exist or not depending on ninja's order (item 4). Either way the object is still a hit.
- `max_size` stays 10 GB until step 4 measures a full build; it took about 0.1 GB here, so 10 GB holds on the order of a hundred.
- Links, archives and Rust compiles are not cached (PLAN.md, Out of scope).
- Build directories configured before this change get the cache only when `tools/set.py` is run on them again, which the skill says. The project root's `out/` is not touched.

**How it was found**
None for the feature. The two repair items came from the first warm builds. One rebuild of every object from a shell got 0 direct hits where the build before it, run by `boot_check.py`, had stored 685 manifests. `CCACHE_DEBUG=1` writes the text ccache hashes next to the object, and comparing it between a shell-started and a Python-started compile of one file showed a single difference, `### CPATH /usr/local/include`; comparing `env` with Python's `os.environ` showed where it came from. The other 50 to 55 preprocessed hits of a fresh worktree were all host objects, and `stat` showed each written a second before `host/gen` existed, which matched the manual's account of direct mode recording whether each `-I` directory exists.

**The user's prediction**
None.

**Three questions you should be able to answer**
- ccache hashes the compilation directory for any compile with debug information. Why did that not stop two worktrees from sharing objects here?
- Why does `base_dir` have to come from the build through the environment, and not from `build/ccache.conf`?
- A build from `boot_check.py` and one from your terminal, at the same commit, both report 100% hits, but one takes four times the CPU of the other. What makes them look up different keys?

## Title: the compiler cache measured on a full build: a warm worktree builds in 6.6 s and 6.6% of a cold build's CPU, and the cache stays at 10 GB (2026-10-07)

**Files**
- `.agents/tasks/build-cache/RESULT.md` (new) — the measurement, method and numbers, and what the task delivered
- `.agents/tasks/build-cache/PLAN.md` — step 4's progress, D4 settled, Status Implemented
- `docs/repairs/261007/build.md` — items 5 (links run one at a time) and 6 (a miss runs the preprocessor first)

**Symptom**
None, a planned measurement: step 4 of `.agents/tasks/build-cache/PLAN.md`. The size limit (D4) and the claim that the cache helps needed numbers.

**Cause**
What decides each build's time. The objects:

- ninja's jobs: 17 edges at once by default on this 15-core machine, and pools, which limit some edges further.
- ccache's two lookups: the direct mode, which hashes the command line, the source and the headers a manifest lists, without running anything; and the preprocessor mode, which runs `clang -E` and hashes its output.
- The `highmem` pool, which every link tool is in, with depth 1 on this 24 GB machine.

The events of each build:

1. No cache: ninja runs 685 compiles 17 at a time, then the last links. 31.6 s.
2. Cold: each compile misses the direct mode, runs the preprocessor to look up the preprocessor mode, misses again, compiles, and stores the object and a manifest. The preprocessor run and the store are the 22% the cold build costs over no cache. 38.4 s.
3. Warm, in another worktree: 619 compiles find their direct-mode key, and ccache hashes the headers its manifest lists, sees them unchanged and copies the stored object out; the other 66 miss the direct mode (repairs item 4) and hit through the preprocessor. Every compile is done 2.0 s in. What is left is 164 links, which the `highmem` pool runs one at a time. 6.6 s, 5.6 s of it links.

So the cache removed what it can remove, the compiles, and the warm build's time is now set by the links: the first thing that does not scale with cores once compiling is cheap.

**Change**
No change to the build. D4 is settled at 10 GB, kept. The plan's reason for 10 GB was "a few builds' worth"; a full build turned out to be 0.1 GB, and a few of them would be 0.3 GB. That reading has the wrong unit: the cache must hold, for each source, one object per branch and commit that changes what it compiles to, across every active worktree, and a header most kernel sources include re-keys most of the 392 kernel objects at once. 0.3 GB would evict within a day objects another worktree builds again. A larger limit costs disk only, since a lookup reads one manifest and one result by key, so 10 GB, about a hundred full builds, stays.

**Reference**
None in Linux or Zircon: build tooling. ccache's manual, "The direct mode", "The preprocessor mode" and "The depend mode"; ninja's manual, "Pools".

**Verification**
One commit (`483c9c6f`) in two fresh worktrees, a new empty cache directory through `CCACHE_DIR`, `/usr/bin/time -l` on `prebuilt/ninja/ninja -C <out>`, every build started with the shell's environment and after 30 s with no other build or QEMU on the machine; one run each, in this order:

| Build | Wall | CPU (user + sys) | Compiles: miss, direct hit, preprocessed hit |
|---|---|---|---|
| no cache | 31.6 s | 405.0 s (366.2 + 38.8) | none cached |
| cold | 38.4 s | 493.9 s (419.7 + 74.2) | 638, 1, 46 |
| warm | 6.6 s | 32.5 s (15.3 + 17.2) | 0, 619, 66 |

- Cache after the cold build: 94.4 MiB, 1318 files (the 687 objects are 262 MiB uncompressed).
- `.ninja_log`: in all three builds the 164 links never overlapped; in the warm one they sum to 5.6 s.
- One compile traced with `CCACHE_LOGFILE` into an empty cache: `Running preprocessor` 89 ms, then `Running real compiler` 565 ms.
- `boot_check.py` on the warm worktree's images: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), `pc-ps2: key a` and `pc-ps2: key A`. One run, two boots.
- Not measured: heat (`powermetrics` needs root), and variance, with one run per build.

**Deliberately not done**
- Items 5 and 6 are recorded, not fixed: both are performance changes, which go through the user with a measurement first (CLAUDE.md).
- A third build without the cache was added to the plan's two, so the record says what a miss costs as well as what a hit saves.
- The shared cache, `~/Library/Caches/ours-ccache`, was neither used nor emptied for the measurement.

**How it was found**
The links: the warm build's `.ninja_log`, sampled every 0.5 s for the edges running at that moment, showed 17 compiles until 2.0 s and then one link at a time; the generated `build.ninja` had `pool build_config_highmem` at `depth = 1`, and `get_concurrent_jobs.py` divides memory by 32 GB per link. The preprocessor run: the cold build's 22% over no cache, then a `CCACHE_LOGFILE` trace of one miss.

**The user's prediction**
None was given. The plan's own expectation, "a warm build dominated by links and packing, a small share of the cold one's CPU time", held: 6.6% of the CPU, and links 5.6 of the 6.6 s.

**Three questions you should be able to answer**
- A cold build through the cache runs every compile that a build without it runs. Where do its extra 22% come from, and why would `depend_mode` remove most of them?
- The warm build used 4.9 cores on average on a 15-core machine. What limited it, and why does that limit not show in a cold build?
- One full build is 0.1 GB of cache. Why is "a few builds' worth" the wrong way to size the cache for several worktrees?
