# shell-tests changes

## Title: Move syscall self-checks out of the interactive shell source (2026-10-03)

**Files**
- `examples/shell/shell.cpp` — retain startup, the single BootFS launcher, and the interactive prompt; call the extracted test runner before the boot marker.
- `examples/shell/shell_launcher.hpp` — share launcher state, the launcher callback type, and byte comparison used by both source files.
- `examples/shell/shell_tests.hpp` — define the handles and callback supplied to the test runner.
- `examples/shell/shell_tests.cpp` — contain syscall checks, process probes, and process-info checks.
- `examples/shell/BUILD.gn` — compile the extracted tests into the same shell executable.

**Symptom**
The interactive shell source contained more than twenty syscall check functions and fault-probe routines alongside line editing and command dispatch, making it difficult to find or maintain the shell behavior.

**Cause**
The initial user-mode shell was also the first ring-3 syscall exerciser. As new kernel interfaces arrived, each check accumulated in the executable's entry-point translation unit. They need the shell process's startup handles and launcher, so removing them or building a separate executable would either lose boot coverage or duplicate launcher setup. The missing boundary was a test runner that receives only the handles it needs and calls the existing launcher through a small callback.

**Change**
Before, `main()` directly invoked each check among its startup and shell setup:

```cpp
check_process_create(job);
check_thread_create(job);
check_process_start(job);
check_channel_loopback();
```

After, `main()` passes one input record to the extracted runner, which retains the original check order and runs before `--- end ---`:

```cpp
shell::tests::Inputs const test_inputs{...};
shell::tests::run(test_inputs);
```

The test source owns the syscall checks and fault probes, including the concurrent `object_get_info` coverage. `shell.cpp` owns command input and dispatch plus the one launcher implementation; tests receive a function pointer and opaque launcher context rather than a copied launcher.

**Reference**
None, this is a translation-unit organization change. The tests remain in the same user executable because boot verification depends on their startup results and they use the shell process's handles.

**Verification**
- `gn gen target/default` and `gn check target/default` passed.
- `clang-tidy` compiled both `shell.cpp` and `shell_tests.cpp` with no diagnostics after the generated syscall declarations were refreshed.
- `ours-verify` passed: debug image 20 passed, 0 skipped, 0 failed; release image 18 passed, 0 skipped, 0 failed. Both reached `--- end ---`.

**Deliberately not done**
The tests were not moved into a separate process or executable; doing so would require another startup contract and launcher capability handoff. The syscall tests were not deleted or reordered.

**How it was found**
The `main()` call list and the contiguous test-function region in `examples/shell/shell.cpp` showed that the checks shared startup handles but did not need to own prompt state. The fault-probe path needed one launcher callback; all other checks needed only the values already available at startup.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why do the checks remain in the same process even though they now live in another source file?
2. Why does the test runner receive a launcher callback instead of a second launcher implementation?
3. Why must the tests still run before the `--- end ---` marker?
