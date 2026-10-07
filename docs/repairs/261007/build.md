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

## 2. The prebuilt link that new_worktree.py makes shows as an untracked file in every worktree

**Severity**
Low: nothing breaks, but `git status` in every worktree under `.worktrees/` lists `?? prebuilt`, and a `git add -A` or `git add .` there would commit a symbolic link to one machine's absolute path.

**Symptom**
In `.worktrees/build-cache`, `git status --short` prints `?? prebuilt`, and `git check-ignore -v prebuilt` prints nothing (exit 1): git does not ignore it.

**Cause**
`.gitignore` ignores the toolchains with `prebuilt/`. A pattern ending in `/` matches only a directory. In the project root `prebuilt` is a directory, so it is ignored. `scripts/new_worktree.py` makes `prebuilt` in a new worktree a symbolic link to the project root's, and git treats a symbolic link as a file, never as a directory, so the pattern does not match it.

**Change**
Write the pattern as `/prebuilt`, with no trailing slash: it then matches the name at the top of the checkout whether it is a directory or a link, and no longer matches a `prebuilt` directory deeper in the tree, which nothing has. The comment above it, which still says the entries are links into the dev container, should say what `prebuilt/` holds now.

**Reference**
None, this project's own tooling. The rule is gitignore's: "If there is a separator at the end of the pattern then the pattern will only match directories".

**Acceptance**
In a worktree made by `new_worktree.py`, `git status --short` does not list `prebuilt`, and `git check-ignore -v prebuilt` names the `.gitignore` line; in the project root, `git status` still lists nothing under `prebuilt/`.

## 3. A build started from Python compiles with CPATH=/usr/local/include, so it misses the cache's direct mode for every object a shell-started build stored

**Severity**
Low: every object is still a cache hit, and on this machine `/usr/local/include` does not exist, so no object changes. Latent: on a Mac where `/usr/local/include` holds headers, a build started from Python searches it, which a freestanding kernel compile must never do.

**Symptom**
In a worktree whose objects had all been built by `boot_check.py`, every object was deleted and rebuilt with `ninja -C out/bringup.x86` from a shell: 0 of 685 compiles were direct hits and all 685 were preprocessed hits. Run again from the shell, all 685 were direct hits, because the first run had stored the shell's manifests. Started from Python (`subprocess.run([ninja, "-C", out])`, as `boot_check.py` does), the build matches the manifests `boot_check.py` stored instead. A full rebuild of the objects took 5.9 s wall and 23.7 s CPU from direct hits, 11.3 s and 105.4 s from preprocessed hits.

**Cause**
`/usr/bin/python3` on macOS is the Command Line Tools' shim. It runs the real interpreter with a developer environment, and everything the script starts inherits it: comparing `env` with `python3 -c 'print(os.environ)'` shows four added variables, `CPATH=/usr/local/include`, `LIBRARY_PATH=/usr/local/lib`, `SDKROOT` and `MANPATH`. Clang reads `CPATH` as extra include directories, after the `-I` ones. ccache knows that, so it puts `CPATH` into the direct-mode hash (`CCACHE_DEBUG=1`'s input text shows a `### CPATH` entry only in the Python-started compile). The two ways of starting a build therefore look up two different manifests for the same compile; the preprocessed output is the same, so the preprocessor-mode lookup still hits, at the price of running the preprocessor. `boot_check.py`, `kshell_run.py` and every other Python script that runs ninja are on one side; a shell, the editor's tasks and the user's terminal on the other.

**Change**
Options, for the user to choose:

- **(a) Compile without the include variables, cache or not** (recommended): every compile command starts with `/usr/bin/env -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u OBJC_INCLUDE_PATH`, in `ours_toolchain.gni` beside `compiler_launcher_prefix`. The include search then depends only on the command line, whoever starts ninja. Clang has no flag that ignores these variables (checked: with `-nostdinc`, a header that is only in a `CPATH` directory is still found), so the environment is the only place to remove them.
- **(b) Remove them only in front of the cache**, inside `compiler_launcher_prefix`: fixes the cache split, but leaves a cached and an uncached build compiling with different search paths.
- **(c) Remove them in the Python scripts that start ninja**: covers `boot_check.py` and `kshell_run.py`, not a script written later.

**Reference**
None, this project's own tooling. The variables are clang's (`clang::driver::tools::addDirectoryList` reads `CPATH`); ccache's list of hashed variables is in its manual, "Cache debugging" and "The direct mode".

**Acceptance**
With the cache warm and every object deleted, a build from a shell and then one from `boot_check.py` both report direct hits for all compiles (`CCACHE_CONFIGPATH=build/ccache.conf prebuilt/ccache/ccache --print-stats`: `direct_cache_hit` equal to the number of compiles); `boot_check.py` passes both images.

## 4. A compile's direct-mode cache key depends on whether its toolchain's gen directory exists yet, which depends on ninja's order

**Severity**
Low: the objects are still cache hits, through the preprocessor instead of directly; nothing built is wrong.

**Symptom**
A fresh worktree built from a cache filled by another worktree got 630 or 635 of 685 compiles as direct hits and the other 50 or 55 as preprocessed hits (two runs). In one run, all 55 were host objects, and each was written a second before `out/bringup.x86/host/gen` existed (`stat`: objects at 05:22:43, `host/gen` at 05:22:44).

**Cause**
`//build/config:default_include_dirs` gives every compile `-I$root_gen_dir` (`-Ihost/gen`, `-Ikernel_x86.dbg.0/gen`, ...). The directory exists only once a generator in that toolchain has written a file into it. ccache's direct mode cannot see a header that a compile did not read but would have read had it existed, so it hashes whether each `-I` directory exists (MANUAL.md, "The direct mode"). A compile that ninja runs before the first generated file gets "does not exist" in its key; the same compile run after gets "exists". Ninja orders independent edges by timing, so which compiles land on which side changes from build to build, and an incremental build in an old worktree, where every directory exists, takes the other key from a fresh one.

**Change**
Options, for the user to choose; measure first how many compiles a fresh build loses to it (the `direct_cache_hit` and `preprocessed_cache_hit` counters of one fresh worktree's build):

- **(a) Create every toolchain's gen directory at `gn gen` time** (recommended): for instance a `write_file()` of a marker into `root_gen_dir` from a file every toolchain evaluates. Every compile then hashes "exists", in every build.
- **(b) Give `-I$root_gen_dir` only to targets that include generated headers**: also removes a search directory from most compiles, but means auditing every target.
- **(c) Leave it**: each affected object costs one preprocessor run the first time it meets the other state, after which ccache has both manifests.

**Reference**
None, this project's own tooling; the behaviour is ccache's, described in its manual under "The direct mode".

**Acceptance**
Two fresh worktrees at one commit, the second built from the cache the first filled: the second's `direct_cache_hit` equals its number of compiles; `boot_check.py` passes both images.
