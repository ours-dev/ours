# init changes

## init counts its four resource servers, not three, when it sizes its Loop (2026-10-12)

**Files**
- `system/service/init/pools.hpp` — `RESOURCE_SERVERS` is 4, and its comment names the four.

**Symptom**
`init.cpp` builds a `ResourceServer` for each of Debug, Irq, Ioport and Sampling, but `RESOURCE_SERVERS` said 3, so `LOOP_SLOTS` fell short by one server's connections (`MAX_RESOURCE_CONNECTIONS`, 4 slots). With every connection in use at once, the Loop would refuse a wait it was sized to take. Not seen in a boot: the shell and routing_checks open one resource connection at a time.

**Cause**
The Sampling server was added to `init.cpp` (the SAMPLING resource served from `/svc`, 2026-10-07) without the count beside the Loop's size following it; nothing ties the two together, and the comment still listed three protocols.

**Change**
`RESOURCE_SERVERS` 3 -> 4, and the comment names Debug, Irq, Ioport and Sampling and says that a server added in `init.cpp` must be counted here. `LOOP_SLOTS` grows by 4 and stays within `PORT_MAX_OBSERVERS` (the `static_assert` beside it holds).

**Reference**
None, this project's own sizing (D27 of the component framework's design).

**Verification**
In `.worktrees/wip-proc-self` at baseline `93c802f5` plus only this change: build ok, dbg PASS (35 passed), rel PASS (33 passed, 6/6 required), host tests 144 passed.

**Deliberately not done**
Deriving the count from the servers `init.cpp` builds, which would need them in one table; the next server (the root job protocol) is about to be added and will be counted with it.

**How it was found**
A read-only survey of init's protocol serving, for the root job protocol the system-wide profiler needs (2026-10-12).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What does a Loop slot hold, and why does each resource connection cost one?
2. What would a client see if the Loop ran out of slots?
3. Why did adding the Sampling server not fail the build?

## The root serves ours.kernel.RootJob, and routes it to the shell (2026-10-12)

**Files**
- `system/lib/ours.resource/include/ours_resource/ours_resource.hpp` — `ROOT_JOB_PROTOCOL`: `ours.kernel.RootJob`, one Get, selector `ours.kernel/RootJob.Get`, ordinal `0x765b31d7e30c32b5`.
- `system/service/init/init.cpp` — a `ResourceServer` over the root job the root was started with.
- `system/service/init/realm.cpp`, `realm.hpp` — the protocol among the root's built-ins, bound to that server.
- `system/service/init/pools.hpp` — `RESOURCE_SERVERS` 5.
- `kernel/products/image/dev/BUILD.gn` — `root.cm` offers it to `#shell`.
- `system/app/shell/BUILD.gn` — `shell.cm` uses it.
- `system/app/shell/shell_tests.cpp` — `check_root_job()`.

**Symptom**
None, a new capability. A profiler the shell starts needs a handle to the root job, to list the processes under it and to bind a job debugger channel to it, and capabilities here are reached by path (`ours-kernel-design` boot.md).

**Cause**
The fourth piece of `.agents/tasks/process-introspection/DESIGN.md`. Each component already receives a duplicate of the root job as its JobDefault startup handle, and the profiler could have used the one the shell holds, but that would be a stand-in: once components get jobs of their own, it would silently narrow to the component's job.

**Change**
The protocol has the same shape as the `ours.resource` ones, one Get answered with a handle, so it reuses their codec and `ResourceServer`, which answers each Get with a SameRights duplicate of the handle it holds; here that is the root job the root was started with, with every right the root has. It is declared beside the `ours.resource` protocols but named in its own library, `ours.kernel`, as Fuchsia's is `fuchsia.kernel.RootJob`. Full rights rather than an inspect-only form: reading a process's memory needs Read and Write on it, which `object_get_child` can only grant if the parent job has them, as Fuchsia's cpu_profiler uses `fuchsia.kernel.RootJob` and not `RootJobForInspect`. Only the shell is offered it; the profiler, which is not a component, reaches it through the namespace the shell hands on.

**Reference**
Fuchsia `fuchsia.kernel.RootJob` (`sdk/fidl/fuchsia.kernel/root-job.fidl`), served by component_manager with SAME_RIGHTS (`src/sys/component_manager/src/builtin_environment.rs`, `root_job.rs`); `src/performance/profiler`'s manifest uses it.

**Verification**
- clang-format and `format_gn.py` clean; clang-tidy (the worktree's build) reports nothing on the changed lines; `wire/test/ordinals_test.py` OK (the new ordinal is its selector's hash).
- In `.worktrees/wip-proc-self` at baseline `376d5cd5` plus only this change: build ok, `dbg PASS 38 passed, 4 skipped` (the new check skipped: no namespace), `rel PASS 37 passed, 8 skipped, 0 failed, 6/6 required`, host tests 144 passed. rel logs `shell: ours.kernel.RootJob Get = Ok` and `shell: root job checks passed`: a Job with the same koid as the job the shell holds, with Enumerate, Read and Write, through which JobProcesses lists processes.
- With the offer to `#shell` removed from `root.cm`: `rel FAIL`, `init: shell's use of "ours.kernel.RootJob" routes nowhere` and `shell: ours.kernel.RootJob Get = NotFound (expected Ok)`.

**Deliberately not done**
`ours.kernel.RootJobForInspect`: nothing needs a narrower handle yet.

**How it was found**
None, a new capability.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is the JobDefault handle the shell already holds not the profiler's route to the root job?
2. Why does the profiler need the root job with Read and Write, not only Inspect and Enumerate?
3. How does a program the shell starts reach a protocol offered only to the shell?
