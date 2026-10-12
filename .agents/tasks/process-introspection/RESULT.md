# process-introspection: a profiler that sees every process's code from outside it

**Date**: 2026-10-12

## Delivered
- 2fcdf325: `object_get_info(ProcessMaps)`, a process's address space region by region, Zircon's v1 record.
- 007a72c1: `process_read_memory`, with Read and Write on the process, at most 64 MiB, cut at the mapping's end.
- a983516f: the shell's vDSO layout reader parses the whole file (exposed by the vDSO growing in step 3).
- 376d5cd5: a job lists its processes (JobProcesses) and hands one out by koid (`object_get_child`).
- a319b06b: `ours.kernel.RootJob`, served by init and routed to the shell.
- 064dfa46: a job's debugger channels (up to 32) hear ProcessStarting before a process's first instruction.
- Step 6 (this commit): `profile` describes every process from outside it, starts a worker, and prints markup for every process its samples name; `docs/repairs/261007/sampler.md` item 4 is closed.

## Verification
- Every step: built and booted in `.worktrees/wip-proc-self` at the previous step plus only that step, clang-tidy clean on its lines, a mutation where a test guards a line.
- At step 6: dbg PASS, 39 passed, 4 skipped; rel PASS, 37 passed, 8 skipped, 6/6 required; host tests 144 passed.
- rel's profile: 159 kernel and 97 user samples (36 in profile, 61 in its worker), 5 processes described, 2 named; prof-fold names the worker's frames down to `spin_inner`, a vDSO frame as `ours_handle_close`, and leaves no frame as an address.
- Not measured: the cost of the job lock on each process start, and how long a start waits for a busy debugger.

## Deviations from the design
- ProcessStarting stops at a kill instead of sending to the remaining debuggers, as decided in DESIGN.md.
- The design names "each job above" the process's; only the root job exists, so the walk is one job and no parent link was added.
- profile starts a worker of its own to have another process's samples: the design's test plan assumed some other process would run during the session, and none does while the shell waits.
- Module names in the markup are "unnamed": VMOs carry no names yet.

## Found along the way
- The shell's vDSO layout reader parsed only the first page (fixed, a983516f).
- `process_read_memory` commits a zero page in the target when it reads an untouched anonymous page: `docs/repairs/261012/mm.md` item 1.

## Not done
- `job_create`, child jobs and JobChildren; a job's own exception channel; a process's debugger channel; ThreadStarting; `process_write_memory`: non-goals in DESIGN.md.
- Naming VMOs so markup carries module names, and describing code mapped after a process was described: in `docs/changes/261012/sampler.md`, "Deliberately not done".
