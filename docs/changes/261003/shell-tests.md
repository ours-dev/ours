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

## Title: Every fault probe is started by the launcher and judged by the code it ended with (2026-10-03)

**Files**
- `examples/shell/shell_tests.cpp` — `run_probes()` passes the launcher to every probe. `return_code_of()` waits for TERMINATED and reads the code with `object_get_info`. `signed_decimal()` prints the code. The probe list gains `pf_write_rodata`.
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — the new probe `pf_write_rodata`, a store into constant bytes in `.rodata`.

**Symptom**
None: step U6 of `docs/design/260924/userspace-filesystem.md`.

Before this step, the 15 faulting probes were spawned by the kernel (`process_spawn_boot`), which returns no process handle. The shell called a probe killed when its answer channel closed empty. A probe that never reached main looked exactly the same.

**Cause**
The objects:
- Two ways to start a probe:
  - `process_spawn_boot`: the kernel loads the image and hands back only the root VMA;
  - the launcher: user space loads the image and hands back the process handle.
- The process handle, through which K24's `object_get_info` reads the code the process ended with.
- `TASK_RETCODE_EXCEPTION_KILL` (-1028): the code the kernel gives a process it kills for an unhandled exception (`ThreadDispatcher`, as Zircon's `ZX_TASK_RETCODE_EXCEPTION_KILL`).

The events, per probe:
1. The launcher starts `fault_probe <name>`, and the shell keeps the process handle.
2. The probe commits its fault. The kernel kills the process with -1028.
3. The answer channel closes empty.
4. The shell waits for TERMINATED. A kill may still change the code until then.
5. The shell reads the code and compares it with -1028.

Only the kernel's own code says "killed for an exception". The closed channel was an inference.

The `.rodata` probe covers the one segment the other write probes did not:
- `pf_write_text` writes the RX segment;
- `pf_write_relro` writes RELRO, which is RW in its program header and narrowed to read-only afterwards;
- the first segment is R in its program header alone, so a loader that ignored the header and mapped it writable would let a write through.

**Change**

```cpp
// before: the kernel spawned every faulting probe
auto const run = run_probe(job, probe);
if (run.killed) { ... " killed" ... }

// after: the launcher, and the kernel's own code
auto const run = run_probe(job, probe, launcher, launcher_context);
if (run.process != ours::Uandle::Invalid) {
    auto const code = return_code_of(run.process);   // TERMINATED, then get_info
    auto _ = ours::handle_close(run.process);
    if (code.is_ok() && code.unwrap() == ours::TASK_RETCODE_EXCEPTION_KILL) { ... }
}
```

Without a launcher, the kernel still spawns the probes and the closed-channel verdict stays as the fallback.

**Reference**
- Zircon `ZX_TASK_RETCODE_EXCEPTION_KILL`, read through `zx_object_get_info(ZX_INFO_PROCESS)`.
- Zircon's core tests judge a crashed process by that code in the same way: they wait for `ZX_TASK_TERMINATED`, then read `zx_info_process_t::return_code`.

**Verification**
`boot_check.py`:
- dbg PASS, 20 passed: `probes done (16 killed, 0 survived)`, each line `killed (retcode -1028)`, launcher checks and object_get_info checks passed;
- rel PASS, 18 passed.

Each probe printed its `fault_probe: <name>` line, so each reached main.

`pf_write_rodata` faulted where intended:
- `RODATA_BYTES` is at 0x72b6 in `.rodata`, inside the first PT_LOAD, flags R;
- the fault report has `cr2=0x2072b6`, the load base 0x200000 plus that offset;
- `rip=0x2086b9` lies inside `pf_write_rodata`;
- `error_code=0x7`: present, write, user.

**Deliberately not done**
- The PMM free-frame check that U6's row lists is not done.
- A probe whose image the launcher maps wrongly dies before main with the same -1028. Only the `fault_probe: <name>` log line tells the two apart; nothing in the verdict does.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the shell wait for TERMINATED before reading the code, rather than reading it as soon as the answer channel closes?
2. What did the closed-channel verdict fail to tell apart that the return code does, and what can the code still not tell apart?
3. Why is a write to `.rodata` a separate case from writes to `.text` and to RELRO?
