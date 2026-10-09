# build changes

## A script makes llvm-cov, llvm-profdata and the profile runtime available for whichever compiler is in the tree (2026-10-08)

**Files**
- `scripts/fetch_llvm_coverage.py` — new; finds the compiler, and downloads and installs the three files when they are missing or belong to another release
- `scripts/test_fetch_llvm_coverage.py` — new; ten tests against synthetic archives served from `file://`, so none of them touches the network
- `.agents/tasks/cov/DESIGN.md`, `.agents/tasks/cov/PLAN.md` — new; the coverage task this is step one of

**Symptom**
`prebuilt/clang/llvm-23` could compile a program with `-fprofile-instr-generate -fcoverage-mapping` but not link it: the driver went looking for `lib/clang/23/lib/darwin/libclang_rt.profile_osx.a`, which was not there (`ld: library ... not found`). Even with a binary in hand there was no `llvm-profdata` to merge its raw profile and no `llvm-cov` to read it, so no coverage report could be made from a host test.

**Cause**
The vendored compilers are trimmed to what the build uses: `llvm-23` carries the compiler, `ld.lld` and a handful of `llvm-*` binutils, and `lib/clang/23/` held only `include/` and, for the kernel's target, `libclang_rt.builtins.a`. Source-based coverage needs three more files, and they cannot come from just anywhere. The compiler writes a coverage map into the binary (`__llvm_covmap`, `__llvm_covfun`) in a format that carries a version number, and `llvm-cov` refuses a map written by a different LLVM, or worse, misreads it. So the tools must come from the same release as the compiler. A version written into a script (or a hash pinned beside it) makes that a second place to keep in step with the compiler, and the two hosts already differ: `build/config/compiler/clang.gni` picks `llvm-23`, a vendored macOS build, on a Mac, and `llvm-22`, symlinks into the dev container's own apt install, anywhere else. A pin that is right for one is wrong for the other, and every compiler update would need an edit nobody remembers to make. The user's rule for this task is no pinned versions.

**Change**
The compiler is the only source of truth. `clang --version` prints `clang version 23.1.0-rc3 (https://github.com/llvm/llvm-project 7196f931f212...)`; the script reads the version and the source commit from it. A tool is ready when it runs and reports exactly that version (`LLVM version 23.1.0-rc3`, compared for equality: a substring test would take `23.1.0` for `23.1.0-rc3`). If one is missing or reports another release it builds the download address from the version, `.../llvmorg-23.1.0-rc3/LLVM-23.1.0-rc3-macOS-ARM64.tar.xz`, with the platform part chosen from the host.

Which compiler: the first of `prebuilt/clang/llvm-*`, highest number first, whose `bin/clang` runs on this host. On a Mac `llvm-22/bin/clang` is a broken symlink and fails to run, in the container `llvm-23/bin/clang` is a Mach-O file and fails to run; no directory name is in the script. Where to look for a tool: next to the compiler, or next to the real file `bin/clang` points into, so a container whose `/usr/lib/llvm-22/bin` already has the tools downloads nothing.

What it does with the archive, before anything under `prebuilt/` changes:

```python
clang = resolve(archive, name, f"{name}/bin/clang")        # a link to bin/clang-23
with archive.extractfile(clang) as stream:
    if not contains(stream, compiler.commit.encode()):     # the compiler's source commit
        raise FetchError("the archive's compiler was not built from commit ...")
```

The archive's own compiler has to carry the commit the tree's compiler reports. Then the three files are staged in a scratch directory inside the compiler's own, the staged tools are run and must report the compiler's release, and only then are they moved into place with `os.replace`, which stays on one filesystem. The runtime goes to the directory the driver itself names (`clang --print-resource-dir`), and only if that is inside the compiler's tree: it never writes into a system directory.

Rejected: pinning a version and a SHA-256 as `fetch_ccache.py` does (the user's rule, and the reason above); asking the GitHub API for the release's digest (an unauthenticated address gets 60 requests an hour, and this machine's egress address had used them up, so the call returned 403); copying the three files from a Homebrew LLVM (not an upstream release, a different version number, and not reproducible on another machine).

**Reference**
None, this project's own design. The shape (download into a scratch directory, check, install, report the version) follows `scripts/fetch_ccache.py`; what differs is that nothing is pinned. The archive's layout (a top directory named after the asset, `bin/clang` a link to `bin/clang-23`, other tools links too) came from reading the first 64 MiB of the real archive.

**Verification**
- The ten unit tests pass: version and commit read from the compiler; a distribution's compiler (no commit) is refused with a message; tools and runtime installed through links; a second run downloads nothing; an archive built from another commit changes nothing and leaves no scratch directory; tools of another release than the archive claims change nothing; a new compiler makes the next run fetch again; a missing archive is reported; the commit search finds a needle across two reads; the highest-numbered compiler that runs is chosen.
- A real run on this Mac, `llvmorg-23.1.0-rc3`: 1496 MiB downloaded, `llvm-cov` 5.7 MB, `llvm-profdata` 6.9 MB and `libclang_rt.profile_osx.a` 115 KB installed, both tools report `LLVM version 23.1.0-rc3`, no scratch directory left. A second run took 0.1 s and downloaded nothing.
- End to end with the installed files: `prebuilt/clang/llvm-23/bin/clang++ -fprofile-instr-generate -fcoverage-mapping t.cpp` now links, the program runs and writes a `.profraw`, `llvm-profdata merge` and `llvm-cov report` give 5 regions with 1 missed (80.00%), 2 functions, 2 lines, 2 branches with 1 missed (50.00%), the same numbers as the earlier run that borrowed Homebrew's tools.
- Not verified: the Linux paths. The container's compiler is not reachable from this Mac, so "tools already next to the real compiler" and the Linux archive names are exercised only with faked banners and a fake host. The failure cases (another commit, another release) ran against synthetic archives, not a real mismatched one.

**Deliberately not done**
- Nothing wires the profile runtime or the tools into GN yet; this step only makes them exist.
- No change to `CLAUDE.md`. The rule "no pinned versions, read the version from the tool in the tree" is the user's for this task; whether it becomes a repository convention is theirs to say.
- No attempt to fetch for a compiler that is not an upstream release build: without a release tag there is nothing to fetch, so the script says so and stops rather than guess.

**How it was found**
The first design pinned `llvmorg-23.1.0-rc3` and a hash, as `fetch_ccache.py` does; the user objected that every compiler swap would then be painful. Reading `build/config/compiler/clang.gni` showed the pin would be wrong on one of the two hosts anyway. A request to the GitHub API for the asset's digest returned 403 with `x-ratelimit-remaining: 0`, which is why the script uses the plain download address. `tar -tvJf` on the first 64 MiB of the archive showed the links (`clang -> clang-23`, `llvm-windres -> llvm-rc`), which is why `resolve()` follows them before reading or extracting anything.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `llvm-cov` come from the same release as the compiler, and which two checks in the script enforce it?
- The script records no hash. What does comparing the source commit inside the archive's compiler protect against, and what does it not?
- On the dev container, which path through the script ends with nothing downloaded, and which ends with an error instead?
