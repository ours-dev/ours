# sampler changes

## profile describes every process from outside it, so prof-fold names any process's user frames (2026-10-12)

**Files**
- `examples/profile/describe.hpp`, `examples/profile/describe.cpp` — new: `profile::describe()` reads a process's mappings (ProcessMaps) and, for each VMO it maps executable, the ELF header, program headers and build ID note from that VMO's mapping at offset 0 (`process_read_memory`)
- `examples/profile/profile.cpp` — takes `ours.kernel.RootJob`, binds a debugger channel on it, describes every listed process and every one that starts, starts a copy of itself as a worker, prints `PROFP` and markup for every described process its user records name; the in-process `read_module()` and `__ehdr_start` are gone
- `examples/profile/BUILD.gn` — the new sources, and `//system/lib/fsio` for starting the worker
- `system/app/shell/shell_tests.cpp` — `check_profile()`'s comment says what profile now checks

**Symptom**
Only a process that printed its own module markup could have its user frames symbolized, and only `profile` did; the vDSO was never described (`docs/repairs/261007/sampler.md` item 4, closed by this entry).

**Cause**
The module map was learned from inside the target, which had to cooperate. The kernel knew every process's mappings, but nothing handed them to a profiler; steps 1 to 5 of `.agents/tasks/process-introspection/PLAN.md` added the calls that do.

**Change**
The objects:
- **The root job handle**, from `/svc/ours.kernel.RootJob`, with Enumerate, Read and Write.
- **A debugger channel** bound to it: ProcessStarting for every process that starts while it is bound.
- **`profile::Process`**: a koid and up to four modules; a **module** is one VMO the process maps executable, with its build ID and its loaded segments.
- **The worker**: `profile --worker`, the same binary doing only the user work and printing nothing.

The events, in order:
1. Bind the debugger channel, then list the root job's processes (JobProcesses) and describe each through `object_get_child()`. Binding first means a process starting during the listing is reported on the channel, so none falls between the two.
2. Start the worker with `fsio::spawn()`; its first thread waits in the kernel until `answer_starts()` reads its ProcessStarting, describes it through `exception_get_process()`, and closes the exception.
3. Start the session; after each round, answer any other start without waiting (`answer_starts(debugger, 0)`).
4. Wait for the worker to exit, answer once more, stop the session, close the debugger channel.
5. For each described process some user record names, print `PROFP <koid>`, `{{{reset}}}`, then `{{{module:<i>:...}}}` and its `{{{mmap:...}}}` lines with module number `i`.

How one module is read from outside (`describe.cpp`):

```cpp
// The executable mapping names a VMO; the same VMO at offset 0 holds the ELF header,
// because the loader maps a program by segment out of one VMO.
auto const *const header = header_mapping(maps, vmo);
read_exactly(process, header.base, &ehdr, sizeof(ehdr));               // e_ident, e_phoff, e_phnum
read_exactly(process, header.base + ehdr.e_phoff, headers, n * 56);    // program headers
auto const bias = header.base - first_load_at_offset_0->p_vaddr;
read_build_id(process, bias, note_segment, module);                    // NT_GNU_BUILD_ID
```

Before, profile read itself in place through `__ehdr_start`; now it is described through the same path as every other process, so the path is tested on the process whose samples are best known.

prof-fold needed no change: it already kept each `PROFP` section apart by koid and looked modules up by build ID in the image's `.debug.json`, which lists the vDSO.

Choices made:
- The worker is the test of the acceptance line ("names the user frames of another that printed no markup"): nothing else in user space runs while the shell waits for profile.
- Starts are answered between rounds, not by a second thread: a start waits at most one round, and profile stays single-threaded.
- A module's name is its VMO's name, and VMOs here have none yet, so the markup says "unnamed"; prof-fold resolves by build ID and does not read it.

**Reference**
Fuchsia's cpu_profiler (`src/performance/cpu_profiler`): `JobWatcher` binds `ZX_EXCEPTION_CHANNEL_DEBUGGER` on a job (`job_watcher.cc`), `TaskFinder` and `ProfilerControllerImpl` reach the root job through `fuchsia.kernel.RootJob`, and the stack sampler reads `ZX_INFO_PROCESS_MAPS` (`stack_sampler.cc`) and process memory with `zx_process_read_memory` (`memory.h`). Linux perf learns the same from `PERF_RECORD_MMAP2` events, the approach rejected on 2026-10-11.

**Verification**
- clang-tidy clean on `profile.cpp` and `describe.cpp`.
- `boot_check.py` in `.worktrees/wip-proc-self` at 064dfa46 plus only this change: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 144 passed.
- rel's profile: "159 kernel samples, 97 user samples (36 in this program, 61 in its worker), 180 dropped", "5 processes described, 2 named in the samples".
- prof-fold on that log, with `kernel.dev.debug.json`: "2 processes described their code, 10 ELFs listed"; the worker's frames come out as `pid 2442;tid 2451;_start;ort_start;main;user_work();spin_inner(unsigned long) [user] 61`, a vDSO frame as `ours_handle_close [user]`, and no frame in the fold is left as an address.
- Mutation: with `note_process()` dropped from `answer_starts()` the worker is never described, profile prints "this program or its worker was not described", and rel fails with "shell: profile checks FAILED".

**Deliberately not done**
- Naming VMOs (a program's file, the vDSO) so the markup carries names: cosmetic, and it belongs to the loader.
- Mappings made after a process was described (a later `vma_map` of code): nothing here loads code after start.
- A second thread answering starts at once: one round's delay is short, and the cost of a start held longer is recorded rather than optimised.
- Processes past 32, or listings past 128: their samples stay addresses.

**How it was found**
None.

**The user's prediction**
None (delegated: "你看着来，尽可能完善的做").

**Three questions you should be able to answer**
1. Why is the debugger channel bound before the job's processes are listed, and what would be missed the other way round?
2. Why is a module found through the mapping of the same VMO at offset 0 rather than through the executable mapping itself?
3. What does profile have to do between rounds, and what happens to a process that starts while profile is in the middle of a round?
