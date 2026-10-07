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
