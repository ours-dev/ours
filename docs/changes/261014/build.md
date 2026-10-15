# build changes

## The host tests' GN template is taken out, and the rule that finds them is one shared module (2026-10-14)

**Files**
- 33 build files, `build/cpp/host_test.gni` and the root `BUILD.gn` — back to what they were before `4a7d48db` and `8ab9400b`, by reverting those two commits; their change records in `docs/changes/261010/build.md` stay as history
- `scripts/host_tests.py` — new; the one rule that finds the host tests
- `scripts/cov_host.py`, `.agents/skills/ours-verify/scripts/boot_check.py` — each drops its own copy of the rule and calls the shared one
- `scripts/test_host_tests.py`, `scripts/test_cov_host.py` — the rule's tests move with it
- `.agents/tasks/cov/PLAN.md` — the step marked withdrawn

**Symptom**
Nothing was broken. The `host_test()` template, the generated `host_tests.json` in the root build file, and a `cov_host.py` that joined two sources of tests were GN machinery added to a build that ran its tests well without it, and the user's line for this tree is that the kernel may have its own GN interface for making tests and that otherwise GN stays a fairly clean environment, with no templates that earn nothing.

**Cause**
The template did not earn its place, and the numbers show why. Of the 143 tests moved to it, 113 were outside `kernel/` (`system/lib` 110, `system/service`, `examples/echo`, `build/toolchain`), which is outside the line above. The rule that finds tests by asking GN could not go anyway: `system/lib/concurrent` is an uninitialised git submodule, so its test could not be declared (`docs/repairs/261014/build.md`). So the template, the generated file, the `set_defaults`, and the two-source join in `cov_host.py` would all have been kept for tests that mostly sit outside the kernel, next to the rule they were meant to replace. The cost was complexity in GN, and what it bought was a list that the scripts could already get another way.

The duplication that started the question was real, though: the same three `gn` queries and the `//benches/` exclusion were written in `boot_check.py` and again in `cov_host.py`, which said so in a comment. That is a fault in the scripts and is fixed in the scripts.

**Change**
`git revert --no-commit 8ab9400b 4a7d48db`, then the change records and the plan restored from `HEAD`, since a change record is history and a revert would have deleted it. The rule moved out of the two scripts into `scripts/host_tests.py`:

```python
def find(root: Path, out: Path) -> list[tuple[str, str]]:
    """(label, file relative to `out`) of every host test, in label order; empty
    if //:default builds none."""
```

`cov_host.py` and `boot_check.py` import it (`boot_check.py` puts the repository's `scripts/` on its path from its own location, `parents[4]`, the same fallback its `repo_root()` uses). `HostTestsError` is a `RuntimeError`, so the handler `boot_check.py` already had still catches it.

Rejected: keeping the template for the kernel's tests only (30 tests, and the rule has to stay for the rest, so there would be two ways to find tests and no gain); moving the template under `kernel/` and calling that the kernel's interface (the same objection, and an interface nobody needs yet is the kind of template the user does not want).

**Reference**
None, this project's own design.

**Verification**
- After the revert, `gn gen` of both directories passes, and for each of the 144 host tests the generated `ninja` file is byte-for-byte the one in the tree that was verified earlier to build exactly what the unmigrated tree builds. 144 of 144.
- `scripts/host_tests.py` finds 144 host tests, `concurrent`'s included. `cov_host.py` over the coverage directory: 144 pass, 144 profiles merged. Its totals have moved with the kernel sources other sessions committed in between (13,326 of 18,264 lines, 73.0%, against 13,307 before).
- The tests of all four scripts pass: `test_host_tests.py` (4), `test_cov_host.py`, `test_cov_kernel.py`, `test_fetch_llvm_coverage.py`.
- `ours-verify`, which runs `boot_check.py` with the shared rule, in a clean worktree: see the commit message for the numbers.
- Not verified: `boot_check.py` run by another session with its own checkout of the skill; the import goes through the path of the file that runs.

**Deliberately not done**
- No test interface in GN, for the kernel or anyone. The user allows the kernel one; there is no need for it yet, and it is added when there is.
- The submodules are not touched. `git submodule update --init` would delete the only copy of `system/lib/concurrent`; `docs/repairs/261014/build.md` has the evidence and the options.
- The stale `host_tests.json` the template made stays in the two build directories until they are regenerated from scratch; nothing reads it.

**How it was found**
The user's "don't make meaningless GN templates", and a count of where the 143 tests sit. It was the first count that showed 113 of them outside `kernel/`; before it, the template had looked like a general improvement.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did removing the template not mean going back to the duplicated rule?
- Why could the rule that asks GN not be deleted, even with the template, and what would have had to be true for it to go?
- A revert would have deleted the change records of the two commits. Why were they restored from `HEAD`?

## `system/lib/concurrent` and `system/lib/lockdep` are real submodules again: the code the kernel builds from is pushed, and the pins point at it (2026-10-14)

**Files**
- `system/lib/concurrent` — gitlink moved from the Bazel-era commit (two files) to `9a34c871`, the tree that was on disk
- `system/lib/lockdep` — gitlink moved from the Bazel-era commit (18 files, seven of them different) to `2eefe4ab`, the tree that was on disk
- `docs/repairs/261014/build.md` — the item for this problem deleted; the same hazard in `vendor/*` written as its own item

**Symptom**
Latent. `git submodule status` showed both with a `-`, so they were uninitialised gitlinks whose directories held the only copy of the code 15 build files depend on. A fresh clone had two empty directories, and `git submodule update --init` would have replaced the disk copy with the pinned commit: two files for `concurrent`, an older tree with `tests/` for `lockdep`.

**Cause**
The repository moved from Bazel to GN and the submodules did not follow. The GN-layout code was written in the working directories, inside submodule paths, so the superproject saw one gitlink each and none of the files, and the submodule repositories were never told. A gitlink pins a commit of another repository; it stores no files. So "the file is on disk and the build works" says nothing about whether any repository holds it, and the one command that makes a gitlink real (`submodule update --init`) is the one that discards the difference.

**Change**
The user chose to keep the submodules ("we need the submodule way"), so the disk code went into the submodule repositories first and the pins moved second:
- Each directory's exact on-disk tree was committed in a scratch clone of its repository and pushed to a branch of its own, `import/baseline-20261014` (`ours-dev/concurrent` at `9a34c87`, `ours-dev/lockdep` at `2eefe4a`). The branches are not merged into `main`; a pull request can be opened from `github.com/ours-dev/<name>/pull/new/import/baseline-20261014`.
- The two directories were moved aside, `git submodule update --init` made them real checkouts at the pushed commits, and the trees were compared with the moved copies: `lockdep` identical; `concurrent` has the same files plus the `LICENSE` and `.gitignore` of the commit it was built on, and lacks an empty `src/`, which git cannot hold.
- The pins went into `aac7503b` (`git commit -- system/lib/concurrent system/lib/lockdep`, which needs the submodules checked out so the working tree matches the new gitlink).

Rejected: making the two directories ordinary directories of this repository (it drops the upstream repositories from the picture; the user wants them).

**Reference**
None, this project's own layout.

**Verification**
- `git submodule status`: both initialised, on `remotes/origin/import/baseline-20261014`.
- `gn gen` and `ninja` of `out/bringup.x86` pass in the shared tree after the change; `ours-verify` exit 0: dbg 39 passed, 4 skipped, 0 failed; rel 37 passed, 8 skipped, 0 failed; 145 host tests passed.
- Not verified: a fresh `git clone --recurse-submodules` of the superproject. The pushed branches are reachable from the pins, which is what it needs, but it was not run.

**Deliberately not done**
- The other submodules (`vendor/cxxopt`, `vendor/googletest`, `vendor/nlohmann_json`) are in the same state, uninitialised with files in their directories; whether the files equal their pins was not checked. They are public upstream projects, so the risk is lower, and they are the item left in `docs/repairs/261014/build.md`.
- `scripts/new_worktree.py` still copies the submodule directories into a new worktree by hand. That was needed while they were uninitialised and is no longer needed for these two; not changed.
- No pull request was opened, and the branches were not merged.

**How it was found**
Moving a host test's `BUILD.gn` into `system/lib/concurrent` made the formatter say "skipped: inside a submodule", which is what a path inside a submodule looks like to the superproject. `git submodule status` showed the `-`, and `git --git-dir=<stored module repo> archive <pinned commit>` diffed against the directory showed what initialising would remove.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A gitlink is a commit id. Why does it follow that code written in a submodule's directory is in no repository until it is committed and pushed there?
- Why were the submodule branches pushed before the pins were committed, and not after?
- Why did `git commit -- system/lib/concurrent` report "no changes" while the submodule was uninitialised?
