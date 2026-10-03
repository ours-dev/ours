# shell-tests: separate self-checks from the interactive shell

**Date**: 2026-10-03
**Status**: Accepted

## Request
Move the many syscall test functions out of `examples/shell/shell.cpp` while keeping the interactive shell and startup self-check behavior.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Test execution | Separate source in same executable; separate test executable; remove checks | Separate `shell_tests.cpp` called before the existing `--- end ---` marker | None | 2026-10-03 |
| Launcher dependency | Duplicate launcher; pass a callback and opaque context; move launcher wholesale | Keep one launcher in shell.cpp and pass the tests a narrow callback | None | 2026-10-03 |

## Steps
1. Extract check helpers, syscall checks, and fault probes to `shell_tests.cpp`; call them through `shell::tests::run(Inputs)` before printing the existing marker. Validate by compiling both translation units and checking GN dependencies.
2. Format, run clang-tidy, build and boot both dev images with `ours-verify`, and inspect that concurrent test additions remain present.

## Files
- `examples/shell/shell.cpp` — keep startup, launcher, and interactive command loop; invoke the test runner.
- `examples/shell/shell_launcher.hpp` — share launcher data, callback type, and byte comparison.
- `examples/shell/shell_tests.hpp` — define the test runner input contract.
- `examples/shell/shell_tests.cpp` — own syscall and process-launch checks.
- `examples/shell/BUILD.gn` — compile both source files into the same user executable.
- `docs/changes/261003/shell-tests.md` — append the change record.

## Test plan
- Run `gn gen target/default` and `gn check target/default`.
- Run clang-format and clang-tidy for all changed C++ files.
- Run `python3 .agents/skills/ours-verify/scripts/boot_check.py`; require both images to reach `--- end ---` with no failed check or error line.
