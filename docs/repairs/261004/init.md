# init outstanding repairs

## 1. Nothing notices when the root itself dies

**Severity**
Medium: not reached today, but when it is, the system keeps running with no /boot, no supervision and no way to start anything, and says nothing.

**Symptom**
Latent, cannot be triggered today: init has no known way to fault. If it did -- a bug in a Loop callback, a malformed manifest the decoder lets through and the runner trips on -- its process would end, every /boot connection and every namespace entry it served would see PEER_CLOSED, every component's next Open would fail, and nothing would log why. The kernel would go on as if nothing happened, and so would the shell, a component since step 15, until its next command found /boot and the Launcher gone.

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
Latent, not triggered by today's components, though any component could trigger it: routing_checks, echo and the shell, the only ones, keep nothing they are given but the shell's Debug resource (echo uses nothing from the root, so it is given nothing to keep), and the shell closes each command's process handle once the command has ended. Every GetBackingMemory and Debug.Get duplicate the root hands out is charged to the root until the client closes it, and every Launch costs the root three handles while the process runs and one for as long as the client keeps the process handle. A client that keeps them makes every later GetBackingMemory, Debug.Get and Launch in the system answer NoResource, once the root's 1024 handles are spent (`HANDLE_MAX_PER_PROCESS`).

**Cause**
The kernel charges a handle to the process that created it, wherever it is now (`system/public/ours/syscall.hpp`). That rule protects the kernel's arena from a process that hides handles in channels, but it makes the root pay for what its clients hold. `pools.hpp` checks that the root's own pools fit the quota with 463 handles to spare (`HANDLES_FOR_CLIENTS`, 14 more since step 15, when init stopped making handles for the standalone launcher and the shell it used to start itself); what clients take from those is not bounded.

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

## 7. At the pools' own limits, namespace entries take almost every /boot connection

**Severity**
Low: a capacity choice made in step 13 beyond D27's list, not a fault; every request past the pool still answers NoResource as D27 says.

**Symptom**
Latent, cannot be triggered by today's manifests: routing_checks holds 4 entries. Each directory use a running component has is one live /boot connection (`BootServer::serve()`), from the same 128 (`MAX_CONNECTIONS`) that every Open and Clone through those entries draws on. `MAX_DIRECTORY_USES` is 15, and 7 of the 8 instances can have a program, so manifests inside the pools' own bounds can pin 105 of the 128 connections before any component opens a file. The 23 left serve every Open in the system, and a component whose Opens find none gets NoResource epitaphs that look like a broken /boot. From step 14 each running component with a protocol use also holds its /svc connection from the same table, so the 23 are 16 at the pools' limits.

**Cause**
D27 settled 8 components and 128 vfs connections; step 13 added `MAX_DIRECTORY_USES = 15` so that the uses plus /svc fill fsio's 16 bindings, and sized nothing against the connection pool. The two pools are checked separately against the port and the handle quota (`pools.hpp`), but not against each other: no static_assert says how many connections must stay free for Opens once every entry is bound.

**Change**
A decision for the user, since it caps how many files the system can have open at once:
- (a) lower `MAX_DIRECTORY_USES` to what manifests need (4 today), with a static_assert that `(MAX_INSTANCES - 1) * MAX_DIRECTORY_USES` leaves at least half of `MAX_CONNECTIONS` for Opens;
- (b) count namespace entries apart from Opens: a second connection pool, or a reserve inside the one pool;
- (c) leave it, and record the bound in `pools.hpp`.
Recommended: (a), the smallest change, which keeps the pools honest with one more static_assert.

**Reference**
Fuchsia has no fixed pool: the Rust vfs allocates a connection per Open (`src/storage/lib/vfs/rust/src/directory/connection.rs`), and a namespace entry costs nothing until its first message routes it (`NamespaceBuilder::serve()`, `src/sys/component_manager/lib/serve_processargs/src/namespace.rs:145-175`). ours's pools exist because a user program has no heap (D27).

**Acceptance**
`pools.hpp` has the chosen bound with a static_assert relating `MAX_DIRECTORY_USES`, `MAX_INSTANCES` and `MAX_CONNECTIONS`, and `boot_check.py` passes.

## 8. Every Open of a /svc entry walks the route again

**Severity**
Low: a performance ceiling (design D11 a), not measured; correct at every load.

**Symptom**
Not visible at today's load: routing_checks connects to /svc a few times, and the shell once at start, for ours.resource.Debug, and once per command it runs, for the Launcher. Every `Open` of a /svc entry, which every `fsio::connect()` and every `fsio::spawn()` makes, reaches `Realm::connect()`, which runs `component::route()` from the component up to the built-ins or the provider again, on the root's one thread (D16).

**Cause**
D11 (a) chose to walk on every Open, as Fuchsia's `RouterEntry::open_entry_async()` asks its router on every request (`src/sys/lib/runtime_capabilities/src/fidl/router.rs:72-104`). The walk is linear in the manifests it crosses: per hop, a scan of the parent's offers or the child's exposes (`component::detail::find_offer()`, `find_expose()`), and at the root a scan of BUILTINS. While the tree is static, every walk for one (instance, use) gives the same answer, apart from D23's exited providers.

**Change**
Measure first, as CLAUDE.md asks: the time `Realm::connect()` takes per Open for a use one, two and three levels deep, against the cost of the Open's own messages; and how many Opens a spawn-heavy shell session makes. If it matters, D11 (b): cache the Route per (instance, use) while the tree is static, checking the provider's state (D23) on each use of the cache. The choice goes to the user with the numbers.

**Reference**
Fuchsia `RouterEntry::open_entry_async()` (`src/sys/lib/runtime_capabilities/src/fidl/router.rs:72-104`), which routes per request; design D11.

**Acceptance**
A benchmark in `benches/` or a timed check gives the per-Open routing cost, and the user has chosen between (a) and (b) from it.

## 9. The Launcher's connections are a fixed pool of four, each about 5 KiB, in init's image

**Severity**
Low: a capacity and size choice (design D13 a, D27), not a fault; a fifth connection answers NoResource as D27 says.

**Symptom**
Not visible today: routing_checks holds at most one Launcher connection at a time, and so does the shell, for as long as it takes to start one command. Four clients that each hold a Launcher connection open leave the fifth with a NoResource epitaph, however idle the four are. And because the pool is constinit with hooks that point at each entry, it lies in init's `.data`, not `.bss`: `launcher_storage` is 29,024 bytes of init's file in the BootFS (`llvm-nm -S`: the four connections' 20 KiB of state plus the server's 8 KiB of message buffers), nearly all zeros.

**Cause**
D13 (a) chose Fuchsia's stateful Add* calls, which keep state on the server between messages; Fuchsia keeps it in growable vectors per connection (`ProcessLauncherState`, `src/sys/lib/elf_runner/src/process_launcher.rs:57-63`), and the root has no heap, so each connection has room for the most a processargs can carry (`launch::ProcessLauncherState`, about 5 KiB). D27 sized the pool at four. The `.data` cost comes from the review of step 6 settled in step 14: a constexpr constructor lets the pool be constinit, so its hooks are in place without code at start, but an object with any non-zero initializer is written into the image whole.

**Change**
Options for the user, none needed today:
- move the server's 8 KiB of message buffers out of `LauncherServer::Storage` into a `.bss` global of their own, which takes them off the image and changes nothing else;
- size the pool from the image's manifests (D27 b) once more components use the Launcher;
- D13 (b), one Launch message carrying everything, which needs no state between messages, at the cost of fdio_spawn's protocol.

**Reference**
Fuchsia `ProcessLauncherState` and `ProcessLauncher::serve()` (`src/sys/lib/elf_runner/src/process_launcher.rs:57-63`, `:87-111`); design D13, D27.

**Acceptance**
Whichever option the user picks: `llvm-readelf -S` on init shows the chosen `.data` size, or a test component that opens five Launcher connections sees the fifth served.

## 10. A route to a provider that has exited answers NotFound without the root's log line

**Severity**
Low: the client gets the right answer, and only the root's log is short of a line.

**Symptom**
On dev, after routing_checks asks echo to stop, its next connect to `/svc/ours.test.Echo` is closed with a NotFound epitaph, as D23 says, but the boot log says nothing about it: every other routing error prints `init: <moniker>'s use of "<name>" routes nowhere: ...` (for instance the unoffered protocol's, `NotFound at . (offer of "ours.test.Unoffered")`), and this one prints nothing. Someone reading the log cannot tell why a client lost a protocol.

**Cause**
The design treats a source that has exited as a routing error like any other: "The root sends an epitaph on `server` and logs one line naming the target, the hop and the capability ... A source that has exited also answers NotFound" ("Events: a component connecting to a protocol through /svc", step 5). In the code the two are decided in different places. `component::route()` knows nothing of instance states, so the walk to an exited echo succeeds, and it is `Realm::forward()` (`system/service/init/realm.cpp`) that finds `provider.m_state == Exited`, or `m_outgoing` already closed, and calls `close_with_epitaph(channel, Status::NotFound)` without logging, unlike `Realm::connect()`'s `log_route_error()` for a failed walk.

**Change**
In `Realm::forward()`, log one line before the epitaph, naming the target (the instance whose use was routed, which `forward()` does not have today and `connect()` would pass in), the provider's moniker and the capability, in `log_route_error()`'s shape: for example `init: routing_checks's use of "ours.test.Echo" routes nowhere: NotFound at echo (exited)`. Ruled out: teaching `route()` about instance states, which would make the pure router depend on the root's lifecycle and break its host tests' model.

**Reference**
Fuchsia `RoutingError::RouteSourceShutdown` (`src/sys/lib/routing/src/error.rs:353`, mapped to NOT_FOUND at `:424`), which component_manager reports through the same path as every other routing error, so it is logged with the target's moniker.

**Acceptance**
`boot_check.py` passes both images, and dev's log shows one `routes nowhere` line naming echo after `init: echo exited with 0`.
