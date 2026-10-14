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
