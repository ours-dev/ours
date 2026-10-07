# build outstanding repairs

## 1. lldb started in a checkout under .worktrees/ shows the project root's sources instead of its own

**Severity**
Medium: the debugger shows the wrong file, silently, but only when lldb's working directory is the root of a checkout under `.worktrees/` and that checkout's sources differ from the project root's.

**Symptom**
In `.worktrees/<name>`, `lldb -o 'command script import scripts/lldb_ours.py' -o 'target create <kernel ELF>' -o 'list vpanic'` prints source lines, but they are read from the project root's `kernel/top/panic.cpp`, not the checkout's. On the same commit the two are equal, so it looks right; on another branch, breakpoints by line and the listed source disagree with the code that was built.

**Cause**
Since `-ffile-compilation-dir=.` (`docs/changes/261007/build.md`), every source path in the debug info is relative to the build directory: `DW_AT_comp_dir` is `.` and the file is `../../kernel/top/panic.cpp`. lldb resolves such a path in two tries:

1. the path as it stands, against lldb's own working directory;
2. only if that file does not exist, `target.source-map`, where `scripts/lldb_ours.py` maps `../..` to its checkout.

Every way of starting lldb here runs it from a checkout's root. From the project root, `../../kernel/...` is outside the project and does not exist, so try 2 runs and maps it correctly. From `.worktrees/<name>`, `../..` is the project root, so try 1 finds the project root's copy and the mapping is never consulted.

A decoy test showed the order: lldb started in `x/a/b`, with a fake `x/kernel/top/panic.cpp` and the mapping set, listed the fake.

**Change**
Options, for the user to choose:

- **(a) Record source paths relative to the checkout root, not the build directory** (recommended): add `-fdebug-prefix-map=../../=` to `relative_paths`, so the debug info names `kernel/top/panic.cpp`, which try 1 resolves against the checkout's own root; change the mapping in `lldb_ours.py` to `. -> <checkout>` for lldb started anywhere else. The objects stay the same in every worktree. A one-file trial showed one more prefix is needed: an include directory came out as `./../../kernel/include/ours`, unmapped, so `-fdebug-prefix-map=./../../=` as well. Sources generated into the build directory (`gen/...`) would then resolve against the root and not be found.
- **(b) Start lldb two directories below the checkout root**, for example in `out/.debug`: try 1 then lands in the right checkout. Every launch path passes repository-relative paths (`target create out/.debug/kernel.elf` in `qemu_debug.py`'s line, `lldb_mcp.py`'s commands), and all of them would need rewriting; a forgotten one fails quietly.
- **(c) Leave it** and say in the debugging documents that under `.worktrees/` lldb shows the project root's copy.

**Reference**
None, this project's own tooling. lldb's order is in `SourceManager::File::CommonInitializer` (the file as named first, `target.source-map` only when it is missing).

**Acceptance**
In a checkout under `.worktrees/` whose `kernel/top/panic.cpp` differs from the project root's (a comment line changed is enough), `list vpanic` with `lldb_ours.py` imported shows the checkout's line; the same command from the project root still lists the project root's file; `boot_check.py` passes both images.
