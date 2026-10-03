# shell-tests: separate self-checks from the interactive shell

**Date**: 2026-10-03

## Delivered
- Moved the syscall checks, fault probes, and process-info checks from `shell.cpp` into `shell_tests.cpp`.
- Added a narrow runner input containing startup handles and an optional launcher callback; the shell remains the single owner of launcher setup.
- Kept the test run before the `--- end ---` marker and the interactive prompt after it.
- No commit was made because the shell worktree contains concurrent user edits.

## Verification
- `gn gen target/default` and `gn check target/default` passed.
- clang-tidy compiled both shell translation units without errors.
- C++ and GN formatting checks, task/change document lint, and `git diff --check` passed.
- `ours-verify` passed: debug image 20 passed, 0 skipped, 0 failed; release image 18 passed, 0 skipped, 0 failed. Both reached `--- end ---`.

## Deviations from the design
None.

## Found along the way
- The existing and concurrently added `object_get_info` process check belongs with the fault-probe test helpers and was preserved there.
- The vDSO declarations are generated during Ninja builds; clang-tidy needed the `syscalls.gen` output refreshed before it could compile the concurrent `object_get_info` check.

## Not done
The tests remain in the same executable and are not run as a separate process; moving them would require another processargs and capability setup path.
