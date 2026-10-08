# tooling changes

## ours-verify runs the host tests: every testonly executable //:default builds, asked of GN, beside the boots (2026-10-07)

**Files**
- `.agents/skills/ours-verify/scripts/boot_check.py`:
  - New `HostTests`. It asks GN for the host-toolchain executables marked `testonly` that `//:default` builds, leaving out `//benches`, and for the file each one links.
  - It runs them four at a time from the build directory while the images boot, prints a `host-tests` line, and fails the check when one exits non-zero.
  - All of their output goes to `host-tests.log`. New flag `--no-tests`.
- `.agents/skills/ours-verify/SKILL.md` — "done" includes the host tests; the `host-tests` line, the flag, and a row for `host-tests FAIL` in the failure table.
- `docs/repairs/261005/prof.md` — item 11 is fixed and removed. The other items keep their numbers, since other documents cite them.

**Symptom**
Repair prof item 11: 143 host test binaries were compiled by every build and run by nothing. Reverting a fix whose regression test existed, such as 74e2a5e4's unwinder start location, left `Unwinder.RowsAfterThePcDoNotApply` failing unseen. `boot_check.py` built it, booted both images, saw them PASS, and the commit could land. The pre-commit hook checks only formatting and documents, and CI only checks out.

**Cause**
The tests were put into `//:default` so that they would keep compiling, which ninja does. But compiling a test is not running it, and nothing between an edit and a commit ran them. A regression test that is never run guards nothing. It only looks as if it does, which is worse than having none, because whoever reads the tree believes the case is covered.

**Change**
The list of tests is asked of GN each run, not written down anywhere:

```text
gn ls out --testonly=true --type=executable   → the host toolchain's, less //benches
gn desc out //:default deps --all             → intersected: what the default build compiles
gn outputs out <labels>                       → the binary of each
```

`testonly` is GN's own mark for a test: only a testonly target may depend on one. Every test executable already carries it, so no BUILD.gn changed, and a test added to `//:default` is run from its first build. This is the user's option A.

The tests run beside the two QEMUs, which take tens of seconds. The tests take about 1 s when their binaries were run before, and about 6 s the first time, when macOS checks each new binary as it first executes. So the check takes no longer.

Rejected, option B: listing the tests through GN metadata, as `<image>.debug.json` lists binaries. Each test target would have to declare itself, and 91 of the 143 live in `system/lib/ustl`, an uninitialised submodule directory whose files this repository cannot commit. B would have left most of them out.

**Reference**
None, this project's own tooling. Fuchsia's build records its tests in a `tests.json` written from GN metadata (`test_spec`). That is option B's mechanism, which ustl being a submodule rules out here.

**Verification**
- In `.worktrees/wip-proc-self` at 9c5cb42a plus this change: `boot_check.py --out out/fresh-unwind`: `dbg PASS`, `rel PASS`, `host-tests PASS 142 passed, 0 failed · 0.7s`. That is 143 less the one bench.
- `host-tests.log` holds 142 sections, every one `ok`.
- With one expectation in `stacks_test.cpp` broken on purpose: `host-tests FAIL 141 passed, 1 failed`, the failing test named with its last lines, and the script exits 1. Restored, it exits 0.
- `--no-tests` prints no `host-tests` line.

**Deliberately not done**
- Running the host tests in the pre-commit hook: the hook does not build, and a check that needs a build belongs to ours-verify, which CLAUDE.md requires before every commit.
- CI: it only checks out, and building there is a decision of its own.
- `scripts/kshell_run.py`'s kernel tests (`*.test`): they need the dbg image's kernel shell and are run after a change to what they cover, as the skill says.

**How it was found**
The sampler audit of 2026-10-05 (tools-tests-3), recorded as repair prof item 11. The user asked for it to be done first, ahead of the stepping-loop test (item 9) and the stop-versus-writer stress test.

**The user's prediction**
None. The user chose option A (ask GN on each run) over B (GN metadata).

**Three questions you should be able to answer**
- Why is `testonly` a sound way to recognise a test, and what does GN refuse because of it?
- Why would listing tests through GN metadata have left out most of them in this tree?
- What does a regression test that is compiled but never run cost, compared with having no test at all?
