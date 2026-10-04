# init outstanding repairs

## 1. Nothing notices when the root itself dies

**Severity**
Medium: not reached today, but when it is, the system keeps running with no /boot, no supervision and no way to start anything, and says nothing.

**Symptom**
Latent, cannot be triggered today: init has no known way to fault. If it did -- a bug in a Loop callback, a malformed manifest the decoder lets through and the runner trips on -- its process would end, every /boot connection and every namespace entry it served would see PEER_CLOSED, every component's next Open would fail, and nothing would log why. The kernel and the shell would go on as if nothing happened.

**Cause**
The root is the one process every other component depends on, and the kernel treats it like any other process: its death is a TERMINATED signal that nobody waits for. userboot starts it and exits; no one holds its process handle with a wait on it. component_manager's first act in Fuchsia is to make itself critical to its job (`job_default().set_critical(JobCriticalOptions::RETCODE_NONZERO, &process_self())`, `src/sys/component_manager/src/main.rs:53-60`), so that its death kills the root job, and the kernel's root job observer then halts or reboots the system. ours has no `job_set_critical`, and no reboot call either (design D23).

**Change**
Add a critical-process mechanism in the kernel: a `job_set_critical(job, process)` system call, after Zircon's, and a kernel policy for the root job's death (halt with a crash record, the counterpart of Zircon's reboot). userboot would mark init critical before it exits. This is a kernel task of its own; the design leaves it open ("Open questions").

**Reference**
Zircon `sys_job_set_critical()` (`zircon/kernel/lib/syscalls/job.rs:140`, over `ProcessDispatcher::set_critical_to_job()`), and `RootJobObserver::CriticalProcessKill()` (`zircon/kernel/object/root_job_observer.cc:29`), which acts on the root job's death; component_manager's call in `main()` (`src/sys/component_manager/src/main.rs:53-60`).

**Acceptance**
A shell check (dbg_1, or a test component on dev) that kills init, or a deliberate fault in a test build of init, ends in a kernel log line naming the root's death and a halt, instead of a system that silently runs on.

## 2. No job per component, and nothing that can stop one

**Severity**
Medium: a deliberate gap (design D6), but every component holds the root job, so isolation between components rests on their good behaviour.

**Symptom**
Latent, cannot be triggered today: routing_checks, the only component, behaves. Any component can create processes directly under the root job with the JobDefault it is given, as long as it also holds an executable VMO (every component that uses /boot with rx* can get one through GetBackingMemory). And when a component misbehaves, the root has no way to stop it: there is no `task_kill`, so `exited` is the only end a component has, and only when it chooses.

**Cause**
D6 (a): each component's JobDefault is a duplicate of the root job, because there is no `job_create`, no `task_kill` and no job policy yet. Fuchsia's ELF runner makes a child job per component (`elf_runner/src/lib.rs:395-432`), applies the component's job policy to it, and stops a component by killing that job.

**Change**
A task of its own, as D6 (b) says: add `job_create`, `task_kill` and job policy to the kernel; the runner then creates a child job of the root job per component, hands that as JobDefault, and Stop kills it. The instance's process handle becomes its job handle in the TERMINATED slot.

**Reference**
Fuchsia `ElfRunner::create_job()` and `configure_launcher()` (`src/sys/lib/elf_runner/src/lib.rs:395-432`; `src/sys/lib/runner/src/component.rs:329`); Zircon `zx_job_create()`, `zx_task_kill()`, `zx_job_set_policy()`.

**Acceptance**
A component's process lies under a job of its own (an `object_get_info` check from a test component), a component asking for ManageProcess on the root job is refused, and a root-side stop ends a running test component with its job's kill code.

## 3. A write to a full channel is dropped, and the client is only told by losing its connection

**Severity**
Medium: correct by the design's rule (I1), but a client that does not read its replies loses its connection with no explanation, and a refused Open's epitaph can vanish.

**Symptom**
Latent, cannot be triggered today: every client reads its replies. A client that sends 64 requests without reading would fill its connection's channel; the root's next reply fails with NoResource, the root logs `init: /boot cannot answer a client: NoResource` and closes the connection, and the client sees PEER_CLOSED with no epitaph. A refused Open whose server end is already full gets no epitaph either, so its client reads PeerClosed instead of the reason.

**Cause**
I1: the root never blocks except in `port_wait`, because one thread serves everyone (D16), and a write that waited for room would stall every other client behind one that does not read. So every write is fire-and-forget, and a failed reply closes its connection from that connection's own callback, since its client would otherwise wait for an answer that never comes (`BootServer::handle()`, the Reply case). Fuchsia's vfs drops the channel the same way when a send fails.

**Change**
Nothing to fix in the rule itself; what is missing is visibility and a bound. Options to put to the user: count the dropped writes per connection and log once per connection instead of per write; or cap a connection's outstanding requests on the client side (fsio has one call in flight at a time, so only a hostile or buggy client can fill a channel). Measure nothing until a client that floods exists.

**Reference**
Fuchsia `src/storage/lib/vfs/rust/src/remote.rs:115-123` (a failed forward drops the channel), and component_manager's warning that a component that never reads its outgoing directory fills it (`src/sys/component_manager/src/model/escrow.rs:29-35`, `:160-188`).

**Acceptance**
A test component that sends 65 Reads without reading gets PEER_CLOSED, the root logs one line for it, and every other client keeps being served.

## 4. The root's handle quota is shared by every client, with no per-client cap

**Severity**
Medium: a deliberate choice (design D18 a), with a known way to exhaust it.

**Symptom**
Latent, cannot be triggered today. Every GetBackingMemory duplicate the root hands out is charged to the root until the client closes it, and from step 14 every Launch costs the root three handles while the process runs and one for as long as the client keeps the process handle. A client that keeps them makes every later GetBackingMemory and Launch in the system answer NoResource, once the root's 1024 handles are spent (`HANDLE_MAX_PER_PROCESS`).

**Cause**
The kernel charges a handle to the process that created it, wherever it is now (`system/public/ours/syscall.hpp`). That rule protects the kernel's arena from a process that hides handles in channels, but it makes the root pay for what its clients hold. `pools.hpp` checks that the root's own pools fit the quota with 454 handles to spare (`HANDLES_FOR_CLIENTS`); what clients take from those is not bounded.

**Change**
Options, as D18 lists them, for the user to choose: (b) per-connection lifetime caps, as FS's GRANT_MAX_BACKING_VMOS (FS:319), which break repeated spawns from one connection; (c) a kernel change that charges a transferred handle to its holder. Measure first how many handles a boot and a busy shell session actually take from the spare.

**Reference**
None in Zircon, which has no per-creator charge: a handle counts against the system-wide arena (`HandleTableArena`, `zircon/kernel/object/handle.cc`), wherever it is. The rule is ours (`HANDLE_MAX_PER_PROCESS`, `system/public/ours/syscall.hpp`); the design's "Handle charges" and D18.

**Acceptance**
A test component that asks for GetBackingMemory in a loop without closing is refused after its cap (b) or charged itself (c), and the shell can still spawn afterwards.

## 5. One thread serves everything, so the longest callback delays every client

**Severity**
Low: a performance ceiling (design D16), not measured; correct at every load.

**Symptom**
Not visible at today's load. Every /boot request, every component exit and, from step 14, every Launch is a callback on the root's one thread. A request that arrives while the root is starting a component -- a map, a plan, one system call per relocation, an unmap -- waits until that start is done (the design's worst interleaving 1).

**Cause**
D16 (a): a thread cannot be started in a running process yet (`syscalls.gni:603-611`), and one thread needs no locks: the Loop runs one callback at a time, to completion (I3, I4). Fuchsia's BootfsSvc serves /boot on a thread of its own, "to avoid reentrancy issues" (`src/sys/component_manager/src/bootfs.rs:394-395`), and component_manager's executor runs on one or two threads.

**Change**
Measure first, as CLAUDE.md asks of any performance work: the latency of a /boot Read from routing_checks while the root starts another component, against the start's own duration. If it matters, the candidates are D16 (b), /boot on a thread of its own once the kernel can start one, or splitting a start into Loop steps so that a Read can run between them. The choice goes to the user with the numbers.

**Reference**
Fuchsia `BootfsSvc::create_and_bind_vfs()` on its own thread (`src/sys/component_manager/src/bootfs.rs:394-395`); `async_loop_run()` (`sdk/lib/async-loop/loop.c:327-337`).

**Acceptance**
A benchmark in `benches/` gives the Read latency under a concurrent start, before and after, and the user has chosen from those numbers.

## 6. The root never runs with assertions on

**Severity**
Low: a gap in test coverage (design D22 a), not a fault.

**Symptom**
Not visible: dev runs the dbg.0 kernel (`kernel/products/image/dev/BUILD.gn`) and dev.dbg_1 the dbg.1 kernel with DEBUG_ASSERT, but only dev boots the root. So the busiest user of ports, full channels, handle transfer and `vma_unmap` in the system never runs under the kernel's DEBUG_ASSERTs, although boot_check keeps two images for exactly that reason.

**Cause**
D22 (a) kept dev.dbg_1 as the image where userboot starts the shell directly, with no root, like Fuchsia's userboot tests, so that the direct-start path stays tested. Booting the root there as well would lose that path (D22 b) or add a third image to every verification (D22 c).

**Change**
D22 (c) as a task of its own: a third image, dev with the dbg.1 kernel, booted by boot_check beside the other two. It touches `products/bringup.gni:34`, `boot_check.py`'s IMAGES and REQUIRED, and `scripts/kshell_run.py:72`.

**Reference**
None, this project's own design: boot_check's two images (`.agents/skills/ours-verify/scripts/boot_check.py`, IMAGES).

**Acceptance**
`boot_check.py` boots three images, and the third prints `routing_checks: passed` with the dbg.1 kernel.
