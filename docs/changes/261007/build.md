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
