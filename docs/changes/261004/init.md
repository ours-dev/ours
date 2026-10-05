# init changes

## Title: init serves /boot from the BootFS, resolves meta/root.cm and starts its first component, routing_checks (2026-10-04)

**Files**
- `system/service/init/boot_server.hpp`, `boot_server.cpp` — new: BootServer, /boot as a vfs tree built from the BootFS at ingest, a read-only slice per file made executable under `bin/`, served over fuchsia.io on the root's Loop, one slot per connection
- `system/service/init/realm.hpp`, `realm.cpp` — new: the instance pool (Instance satisfies `component::Instance`), the boot resolver, the ELF runner that routes each directory use and starts the program with `launch::start()`, and the TERMINATED and outgoing-directory slots
- `system/service/init/pools.hpp` — new: D27's pools, with static_asserts against `PORT_MAX_OBSERVERS` and the handle quota
- `system/service/init/init.cpp` — main rebuilt around them: ingest, resolve and start the root, the standalone launcher started with `launch::start()` on its file from /boot instead of `BootfsLauncher`, VMEX kept, and init's three blocking waits made Loop slots
- `system/service/init/BUILD.gn` — the new sources, and the component, fuchsia-io, loop, vfs and wire libraries
- `system/service/init/test/routing_checks/BUILD.gn`, `routing_checks.cpp` — new: the on-target test component and its manifest
- `kernel/products/image/dev/BUILD.gn` — dev's `root.cm`, with routing_checks as its one eager child, packaged into the user BootFS
- `system/lib/ort/src/cpp_dependencies.cpp` — `std::__libcpp_verbose_abort`, which the first program linking fsio needs
- `.agents/skills/ours-verify/scripts/boot_check.py` — required lines per image, waited for past the end marker
- `docs/repairs/261004/init.md` — new: the six items the plan names for this step
- (the vfs, wire and fsio changes this step needed have their own entries in `vfs.md` and `fsio.md`)

**Symptom**
None; this is step 13 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`). Before it, init started one program itself (the standalone launcher, through `BootfsLauncher`), handed VMEX away, and waited on the shell with three blocking calls in a row. No process had a namespace and nothing served a file protocol.

**Cause**
Why each piece is shaped the way it is.

- **Why the root reaches its own /boot by direct call.** The resolver reads `meta/<name>.cm` and the runner takes `bin/<program>` from the same tree that clients open through fuchsia.io. It would be natural to give the root a namespace of its own and use fsio like everyone else. But the root has one thread, and the Loop has no continuations (D16): a callback that wrote an Open on its own /boot connection and then waited for the reply would block the only thread that could read that Open. That is invariant I2, "a blocking call to yourself deadlocks". So `BootServer::file()` is a plain lookup in the tree, and only clients go through connections. component_manager hits the same wall when its boot resolver calls `Path::exists` on its own /boot, and gets out of it by serving /boot on a thread of its own (`BootfsSvc::create_and_bind_vfs()`, "to avoid reentrancy issues").
- **Why a slice per file, and Execute only under `bin/`.** A VMO with the Execute right is what lets a process map code. Before this step the launcher made the whole BootFS executable, so one handle with Execute covered configuration files and manifests too (D15's rejected draft). Now each file gets its own slice of the read-only BootFS VMO, and only a file whose name starts with `bin/` has its slice replaced with `vmo_replace_as_executable`. GetBackingMemory hands out Execute only to a connection that holds EXECUTE on a node the tree marked executable, so "may this be run" is decided once, at ingest, by directory, as BootfsSvc decides it (`ingest_bootfs_vmo()`, which calls `replace_as_executable` on each executable file's own VMO). The slices share the BootFS's pages, and none of them can write, which is what I6 needs: every VMO a client can hold with Execute has bytes nobody can write, so `launch::start()` can parse it in place (D14).
- **Why the slice works at all.** A slice must start on a page and must not share a page with another file, or a program's last page would carry its neighbour's bytes into an executable mapping. Step 2 made `bootfs::View` refuse a BootFS that breaks that rule, so every entry ingest sees is sliceable.
- **Why directory uses are routed at start, and why a failure arrives as an epitaph.** The runner routes each directory use before the program exists (D8), and either binds the entry's server end as a fresh /boot connection, at the route's subdirectory and with the use's rights, or, if the route fails, writes an epitaph with the route's status on it and closes it. The component gets the client end either way, so its namespace always has every entry its manifest names, and a broken one says why on first use. Fuchsia routes an entry on its first message instead and closes it the same way, `server.close_with_epitaph(e.as_zx_status())` (`NamespaceBuilder::serve()`); ours routes earlier to save a wait slot per entry, and the client sees the same answer.
- **Why the connection's rights are the use's, not the offer's.** The route checks that every hop on the way had at least the use's rights; the connection then carries exactly what the component asked for. `/boot-r` is offered with r*, used with r*, and holds r*: no EXECUTE, so an Open asking for it is refused by the vfs's "a connection never grows" check. `/denied` is offered with r* and used with rx*: the offer gives less than the use asks, so the walk stops at the root's offer with AccessDenied before any connection exists.
- **Why every wait became a Loop slot.** The old init made three blocking waits in a row: for the shell's "pong", for the shell to close its channel, for the shell's process to end. Each one stopped the process completely, so nothing else could be served while it waited. A server cannot work that way: /boot must answer routing_checks while the shell is still starting. So every wait is now a slot on one port, and `Loop::run()` is the only place the root blocks (I1). The shell's channel keeps one slot for both of its old waits: first the answer (READABLE), then the shell letting go (PEER_CLOSED with nothing readable), re-armed between them.
- **Why the process handle is closed only by its own slot.** The TERMINATED callback reads the return code and answers Close; the Loop closes the handle. The callback closes nothing else, because every other handle the component held -- its /boot connections, its outgoing directory -- has a slot of its own, which sees PEER_CLOSED and closes itself. If the exit callback closed them, a packet already queued for one of those slots would name a handle that had been closed and perhaps reused: I3, "only a slot's own callback closes its handle".
- **Why `resolve()` refuses before it makes anything.** `route()` terminates only because resolving fails once the instance pool is full: a manifest that names itself as a child and exposes a capability from that child makes a tree with no bottom, and each step down resolves one more instance (`component/route.hpp`, Termination). So `Realm::resolve()` checks that the pool has room for all of an instance's children first, and only then creates the outgoing pair and the children. It never grows the pool and never evicts: the pool only grows by that one line. Checking first also makes resolve all-or-nothing, so a failed resolve leaves no half-made instance behind.
- **Why the big globals live in `ustl::LazyInit`.** Nothing runs a user program's dynamic initialisers, so the root's storage must be constant-initialised: `constinit`. But a global whose type has a destructor registers that destructor with `__cxa_atexit` at start, and the instance pool and /boot's connection table hold `ustl::Option`, whose destructor is user-provided. The link failed with `undefined symbol: __cxa_atexit`. The root never exits, so there is nothing to run at exit; `LazyInit` is raw, trivially destructible storage that `main()` constructs the objects in, and the problem goes away without adding an atexit the runtime does not need.
- **Why ort now defines `std::__libcpp_verbose_abort`.** routing_checks is the first program to link fsio, and fsio's `Namespace::resolve()` calls `std::string_view::substr()`. substr checks its position and throws `std::out_of_range` when it is past the end; with `-fno-exceptions`, libc++ calls `__libcpp_verbose_abort` instead. Where the optimizer cannot prove the check passes, the call stays, and the link failed. The kernel (`//kernel/lib/libc`, `libcxx.cpp`) and phys (`//kernel/lib/ccrt`, `itanium.cpp`) each define it; user programs had not needed it yet. The runtime is where it belongs, beside the other symbols the compiler and the C++ library expect (`operator delete`, `__cxa_pure_virtual`), and like them it is weak and stops the program: it prints the format string as it is and traps.
- **Why boot_check waits past the end marker.** `--- end ---` is the shell's last line, and `routing_checks: passed` comes from another process, which the scheduler may run after the shell is done. boot_check used to stop reading one second after the end marker, so the required line could be missed on a slow or busy host and the boot would fail for no reason. Now it keeps reading until the marker and every required line have appeared, or the timeout runs out.

**Change**
The boot, in order (design "Events: boot"):

```text
init: started with the root job
init: /boot has 8 files, 4 executable
init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")
init: started routing_checks
init: launched the launcher
init: launched shell
init: shell says "pong"
routing_checks: passed
init: routing_checks exited with 0
init: shell closed its channel
```

Ingest, before and after: the launcher used to be started from an executable copy of the whole BootFS,

```cpp
auto made = launch::make_bootfs_launcher(bootfs, vmex, vdso, vma, ...);   // whole BootFS + vmex
launch::launch_from_bootfs(library, job, user_programs::LAUNCHER, there, ...);
auto _ = ours::handle_close(vmex);   // "vmex is the launcher's alone"
```

and now every file is its own VMO in /boot, and the launcher is just one of them:

```cpp
ours::vmo_create_child(bootfs, ours::VmoChildOptions::Slice, entry.data_offset, entry.data_size, &slice);
if (lies_under(name, user_programs::DIRECTORY)) {             // "bin"
    ours::vmo_replace_as_executable(slice, vmex, &vmo);        // one call per program
}
m_tree.add_file(parent, rest, vfs::VmoFile{.vmo = vmo, .bytes = file_data, .executable = ...});
...
launch::start({.file = boot.file(user_programs::LAUNCHER)->vmo, .vma = vma, ...});
```

The runner, for each directory use:

```cpp
auto const routed = component::route(instance, use, BUILTINS);   // BUILTINS: `boot`, rx*
if (routed.is_err())        served = routed.unwrap_err().status;  // and one log line
else                        served = m_boot->serve(server, routed->subdir.view(),
                                                   to_operations(*routed->rights));
if (served != Status::Ok)   close_with_epitaph(server, served);
handed.add(client, handle_info(HandleType::NsDir, index));        // names[index] = use.target_path
```

then JobDefault (a duplicate of the root job, D6), DirectoryRequest (the outgoing server end made at resolve), VmoVdso and VmaRoot, argv `/boot/<binary>` and the manifest's args (D19). The processargs is encoded before `launch::start()`, with VmaRoot's info already listed, so a message that cannot be encoded starts nothing.

The pools (`pools.hpp`): 8 instances, 128 /boot connections, 4 Launcher connections (counted now, served from step 14), 256 /boot nodes, 15 directory uses per component (with /svc, fsio's 16 bindings), and `3 * 8 + 128 + 4 + 2 = 158` Loop slots, the last 2 init's own until the shell is a component. The handle sums: `1 + 256 + 6 + 3 = 266` fixed, `8 + 2 * 15 = 38` per component, `266 + 8 * 38 = 570` of 1024, leaving 454 for what clients take (D18).

dev's root.cm offers `boot` three times: rx* as is, r* `as` `boot-r`, and r* `as` `denied`. routing_checks.cm uses them four ways: `/boot` rx*, `/boot-r` r*, `/bin` rx* with `subdir = "bin"`, `/denied` rx*.

routing_checks checks, through fsio: Read of its own first four bytes ("\x7fELF") under `/boot`, `/boot-r` and `/bin`; ReadDirents with "." first and `bin/`, `meta/` and `routing_checks` found with their types; GetBackingMemory(READ), whose VMO maps R but not RX, and GetBackingMemory(READ|EXECUTE) refused on a connection without EXECUTE; GetBackingMemory(READ|EXECUTE|PRIVATE_CLONE), whose VMO maps RX; EXECUTE refused under `/boot-r` and on `meta/routing_checks.cm`; and AccessDenied from three opens under `/denied`, the first read from the epitaph, the others from the binding.

Rejected:
- A namespace for the root itself, with fsio over its own /boot: deadlocks on one thread (I2), as above.
- Logging and halting when a component fails to start: D28 halts only when the root itself cannot start; a child that fails is logged and the next one starts, as Fuchsia's `start_eager_children_recursive()` logs and goes on.
- Keeping `init: vmex resource ok`, `init: vdso vmo ok` and `init: bootfs has N entries`: the D28 line now covers a missing handle, and `init: /boot has ...` replaces the count. `init: vmex is the launcher's alone` went with the code that closed VMEX.

**Reference**
- Fuchsia component_manager, at `51f05104c13`: `BootfsSvc::ingest_bootfs_vmo()` and `create_vmo_from_bootfs()` (`src/sys/component_manager/src/bootfs.rs:233`, `:204`), its `replace_as_executable` per executable file (`:301`), and `create_and_bind_vfs()`, /boot on a thread of its own (`:385`, `:394-395`); the boot resolver (`src/sys/component_manager/src/builtin/fuchsia_boot_resolver.rs`); `ComponentInstance::start_eager_children_recursive()` (`src/sys/component_manager/src/model/component/mod.rs:1205`); `configure_launcher()` (`src/sys/lib/runner/src/component.rs:329`); `NamespaceBuilder::serve()` closing a failed entry with `close_with_epitaph(e.as_zx_status())` (`src/sys/component_manager/lib/serve_processargs/src/namespace.rs:119`, `:145-175`); `async_loop_run()` (`sdk/lib/async-loop/loop.c:327-337`).
- libc++: `std::__libcpp_verbose_abort` (`<__verbose_abort>`), and the kernel's and phys's definitions (`kernel/lib/libc/libcxx.cpp`, `kernel/lib/ccrt/src/itanium.cpp`).
- Design: `docs/design/261004/component-framework.md`, "Events: boot", "Events: starting a component", "Events: a component exiting", I1-I6, D6, D8, D14-D16, D19, D22-D24, D27, D28, and "The routing walk", Termination.

**Verification**
- `boot_check.py`: dbg PASS (28 passed, 1 skipped, 0 failed, 0 error lines, the same as before), rel PASS (25 passed, 3 skipped, 0 failed, 1/1 required, 0 error lines).
- The dev log holds the lines above; dbg_1's is unchanged.
- Mutation check: with dev's `denied` offer widened to rx*, the rel boot printed `routing_checks: /denied/bin/routing_checks = Ok (expected AccessDenied) FAILED`, the same for `/denied` and `/denied/bin`, `routing_checks: 3 check(s) FAILED` and `init: routing_checks exited with 1`, and boot_check failed on the FAILED lines.
- boot_check's waiting, checked offline on the rel log: a required line that never comes fails with `no "..." within 60s`; a required line fed after the end marker holds the stop until it arrives.
- Host tests of the libraries this step changed: wire 12, vfs 42, fsio 33, all passed.
- clang-format clean on every changed line; clang-tidy reports nothing on `init.cpp`, `boot_server.cpp`, `realm.cpp`, `routing_checks.cpp`, `cpp_dependencies.cpp` and the library sources and tests touched; the only output, in `bootfs/view.hpp`, predates this change.

**Deliberately not done**
- /svc, the built-in protocols, and the Launcher in the root: step 14. The runner skips protocol uses, and BUILTINS holds only `boot`.
- The shell as a component, and dropping the standalone launcher and the ping: steps 15 and 16. Autorun still goes through the standalone launcher.
- No root-side test of the instance pool on the host: Realm makes system calls throughout, and its one pure rule, refuse before making anything, is three lines in `resolve()`.
- The six repair items in `docs/repairs/261004/init.md`: the root's own death, no job per component, dropped writes on full channels, the shared handle quota, one thread's head-of-line stall, and the root never running under DEBUG_ASSERT.
- `.agents/tasks/component-framework/RESULT.md` is not updated here: its sections are written as steps land on the integration branch.

**How it was found**
None for the step itself. Two link failures on the way: `undefined symbol: std::__libcpp_verbose_abort` from `namespace.namespace.cpp.o` (found with `llvm-objdump -dr`, the caller being `std::__throw_out_of_range`, which `string_view::substr` reaches), and `undefined symbol: __cxa_atexit` from `_GLOBAL__sub_I_init.cpp`, the registration of the pool's destructor.

**The user's prediction**
None.

**Three questions you should be able to answer**
- The root needs `meta/routing_checks.cm` to resolve routing_checks. Why does it read it with `BootServer::file()` rather than opening it through fsio like routing_checks reads its files, and what would happen on the first boot if it did?
- `/denied` is offered with r* and used with rx*. At which point of the boot is the AccessDenied decided, what does routing_checks find on the channel when it first opens something under `/denied`, and why does its second open answer AccessDenied without touching the channel?
- When routing_checks exits, its process's TERMINATED and its /boot connections' PEER_CLOSED arrive in some order. Why does the exit callback close only the process handle, and what would go wrong if it also closed the connections?

## Title: init's handle sums count what it makes for the launcher and the shell, and not what userboot made (2026-10-04)

**Files**
- `system/service/init/pools.hpp` — `INIT_OWN_HANDLES` (13), the handles init makes for the standalone launcher and the shell; `FIXED_HANDLES` is the port, the slices and those, without the six startup handles

**Symptom**
Latent, cannot be triggered today: the static_asserts held before and hold now. The sum they check was wrong in two directions. It said the root pays for 266 handles before any component starts and leaves 454 for clients; the root actually makes 270 and leaves 450. Had the pools been sized close to the quota, the assert would have passed on a layout that runs out of handles at boot.

**Cause**
The kernel counts a handle against the process that created it, wherever the handle is now (`HANDLE_MAX_PER_PROCESS` in `<ours/syscall.hpp>`): sending a handle away does not give room back, or a process could fill the system-wide arena from a table that always looks empty. So the question for the root's budget is "which handles did init create", never "which handles does init hold". The step 13 sum answered the second question in both of its guesses:
- It counted the six startup handles userboot hands init (the job, the BootFS, VMEX, the vDSO's VMO, the Debug resource, the root region). init holds them but did not create them; they are charged to their creator. This over-counted by 6, which is harmless but hides the rule.
- It counted 3 for what init still starts outside the tree ("the launcher connection, the shell's channel and the shell's process"). That is what init holds afterwards, not what it made. init makes, and the processes it started keep: for the launcher, a connection channel pair (2), duplicates of the BootFS, VMEX and the vDSO's VMO (3) and the launcher's root region, which `process_create` inside `launch::start()` makes for init (1); for the shell, the ping channel pair (2), duplicates of the job, the BootFS and the vDSO's VMO (3), and the launcher connection `launch::Client::connect()` makes for it (2). That is 13. The shell's process handle, which the sum did count, is the launcher's: the launcher called `process_create`. This under-counted by 10.

Net, 4 too few, so the margin the second static_assert promises was 4 handles thinner than it said. The design's "Handle charges" already lists the right things (the port and one slice per file as fixed costs); the sum in the code drifted from it.

**Change**
Before:

```cpp
inline constexpr auto FIXED_HANDLES = usize{1} + MAX_BOOT_NODES + 6 + 3;
```

After:

```cpp
inline constexpr auto INIT_OWN_HANDLES = usize{6} + 7;   // launcher, shell; until steps 15-16
inline constexpr auto FIXED_HANDLES = usize{1} + MAX_BOOT_NODES + INIT_OWN_HANDLES;
```

The sums are now `1 + 256 + 13 = 270` fixed, `270 + 8 * 38 = 574` for a full pool, and 450 left for clients, still above the 256 the second static_assert asks for. Steps 15 and 16 remove `INIT_OWN_HANDLES` with the code that makes those handles.

Rejected: keeping the six startup handles in the sum as a safety margin. A margin belongs in the static_assert's bound, where it says it is one; inside a sum of charges it teaches the wrong rule.

**Reference**
None in Zircon, which has no per-process quota at all: every handle comes from one system-wide arena of `kMaxHandleCount` (`zircon/kernel/object/handle.cc:20`), and `HandleTableArena::Alloc()` (`:103`) only fails when that arena is empty and warns past `kHighHandleCount`. Counting against the creator is this project's own rule, stated at `HANDLE_MAX_PER_PROCESS` in `system/public/ours/syscall.hpp` so that no one process can exhaust the arena, and the design's "Handle charges" applies it to the root.

**Verification**
- The two static_asserts in `pools.hpp` compile.
- `boot_check.py`: dbg PASS, rel PASS (see the commit message for the counts).

**Deliberately not done**
- No run-time check of the root's handle count against these sums: there is no `object_get_info` topic that reports a process's charged handles yet.

**How it was found**
Review of step 13: listing every `channel_create`, `handle_duplicate` and `launch::start()` in `init.cpp` against the terms of `FIXED_HANDLES`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- init holds the root job handle userboot gave it. Does that handle count against init's quota, and why does the kernel charge handles that way rather than by holder?
- The shell's process handle sits in init's table and its Loop slot. Why is it still not one of init's charges?
- The launcher's root region is held by the launcher. Why is it charged to init?

## Title: the root logs a namespace entry it routed but could not serve (2026-10-04)

**Files**
- `system/service/init/realm.cpp` — `Realm::run()` logs one line when a directory use routes but `BootServer::serve()` (or the D3 check after the route) refuses it

**Symptom**
Latent, cannot be triggered by today's manifests. A directory use whose route succeeds can still fail to be served: its joined subdir names nothing in /boot (NotFound), names a file (NotDir), or the /boot connections or the Loop are full (NoResource). The root then closed the entry with an epitaph, as it should, but logged nothing, so the boot log of a component whose /bin was empty looked exactly like a healthy one. With `subdir = "bin"` in routing_checks.cm changed to `"nobin"`, the only trace was routing_checks' own `/bin/routing_checks = NotFound (expected Ok) FAILED`; nothing in the root's lines said which entry, or why.

**Cause**
Step 13 logged in one of the two ways an entry can end up closed. A failed route gets a line naming the target, the hop, the verb and the capability, as the design asks ("Events: starting a component", step 2). A failed serve went straight to `close_with_epitaph()`. From the component's side the two look the same: in both, the entry's first Open finds the channel closed and reads the epitaph's status. Fuchsia gets away with logging only routing errors because its serving step cannot fail on its own: the source is opened with the route's rights and path through the provider's own vfs, which answers on the request channel. Here the root is both the router and the server of /boot, and its server has failure modes of its own (a pool, a lookup), so they need their own line; otherwise the root's log is not enough to say why a component's namespace is broken.

**Change**
Before:

```cpp
if (served != Status::Ok) {
    close_with_epitaph(server, served);
}
```

After:

```cpp
if (served != Status::Ok) {
    if (routed.is_ok()) {
        auto _ = ort::debug_line({"init: ", moniker.view(), "'s use of \"", use.source_name,
                                  "\" cannot be served: ", ours::to_string(served)});
    }
    close_with_epitaph(server, served);
}
```

A failed route still logs its own, more detailed line, and only that one.

Rejected: failing the component's start instead. The design closes a broken entry and lets the component run (D8), as Fuchsia does; the component decides what a missing entry means.

**Reference**
Fuchsia `NamespaceBuilder::serve()` (`src/sys/component_manager/lib/serve_processargs/src/namespace.rs:145-175`), which closes an entry it cannot route with `close_with_epitaph()` and leaves the logging of routing errors to the ErrorReporter router (the comment at `:167-169`); serving the source is the provider's vfs, which answers on the request channel.

**Verification**
- Mutation: with routing_checks' `/bin` use given `subdir = "nobin"` (reverted afterwards), the rel log read `init: routing_checks's use of "boot" cannot be served: NotFound`, then routing_checks' two `/bin` FAILED lines and `init: routing_checks exited with 1`; boot_check failed.
- `boot_check.py` on the real manifests: dbg PASS, rel PASS (see the commit message); no new line, since every entry is served.
- clang-format clean; clang-tidy reports nothing on `realm.cpp`.

**Deliberately not done**
None.

**How it was found**
Review of step 13: following each way out of the directory-use loop in `Realm::run()` and asking which of them leaves a line in the root's log.

**The user's prediction**
None.

**Three questions you should be able to answer**
- routing_checks' `/bin` entry and its `/denied` entry are both closed before it runs. How does the component tell them apart, and how does the root's log?
- Name the three ways `BootServer::serve()` can refuse an entry whose route succeeded.
- Why does Fuchsia not need this second log line?

## Title: boot_check stops once a FAILED line has decided a boot with a required line, and the skill names the new verdict (2026-10-04)

**Files**
- `.agents/skills/ours-verify/scripts/boot_check.py` — `Boot.feed()` sets the stop once the shell has finished and either every required line is in or a FAILED line has appeared
- `.agents/skills/ours-verify/SKILL.md` — a row for `no "<line>" within Ns` in "Reading a failure"

**Symptom**
Any failure in routing_checks made the rel boot run to the 60-second timeout before boot_check printed FAIL. With dev's `denied` offer widened to rx*, the old script took 61 s for a verdict whose cause, three FAILED lines, was on the console within the first five seconds; the new one takes 5 s for the same verdict and the same lines. And a boot that failed only for a missing required line printed a verdict the skill's table did not list.

**Cause**
Step 13 made boot_check wait past `--- end ---` for every required line, because `routing_checks: passed` comes from another process and may follow the shell's last line. But that line has a precondition: routing_checks prints it only when none of its checks failed. So the wait had two ways to end, the required line or the timeout, and a failing routing_checks always took the second, even though its FAILED lines had decided the verdict already (`verdict()` looks at FAILED lines before required ones). The stop condition tested "is anything still missing" when the question was "can anything still change the verdict". After a FAILED line, nothing can.

**Change**
Before:

```python
if self.end_at is not None and self.stop_after is None and not self.missing():
    self.stop_after = now + END_GRACE
```

After:

```python
if (self.end_at is not None and self.stop_after is None
        and (not self.missing() or self.failed)):
    self.stop_after = now + END_GRACE
```

Still only once the end marker is in, as before step 13: a FAILED line from the shell's own checks keeps the old stop, one second after the end marker. The skill's table gains the row for `no "<line>" within Ns`, which points at the root's `init:` lines.

Rejected: stopping at the first FAILED line even before the end marker. The shell's later checks would then go unreported, and a boot with one FAILED check would print less than it used to.

**Reference**
None, this project's own tooling.

**Verification**
- Offline, by feeding lines to `Boot.feed()`: the end marker alone sets no stop; the required line after it does; the required line before it, then the marker, does; the marker then a routing_checks FAILED line does, with a FAIL verdict; a FAILED line before the marker sets none until the marker comes.
- The mutation above (dev's `denied` offer at rx*, reverted): the old script 61 s, the new one 5 s, both `rel FAIL 4 FAILED line(s) ... 0/1 required`.
- `boot_check.py` on the real tree: dbg PASS, rel PASS (see the commit message).

**Deliberately not done**
- Waiting for the root's `init: routing_checks exited with N` line as well. `routing_checks: passed` already says every check ran and passed; the exit line adds nothing a verdict needs.

**How it was found**
Review of step 13, on the step's own mutation check: its FAIL took a full minute.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can `routing_checks: passed` come after `--- end ---`, and why does that force boot_check to wait past the end marker at all?
- After this change, what are the three ways a rel boot's reading ends, and which verdict does each give?
- Why does a FAILED line before the end marker still not stop the boot early?

## Title: two claims of the step 13 entry hold only once the standalone launcher is gone, and say so (2026-10-04)

**Files**
- `docs/changes/261004/init.md` — this entry, which narrows two sentences of the step 13 entry's "Cause" to what step 13 actually ships

**Symptom**
None in the code; the step 13 entry describes the end state of the plan in two places where step 13 is not there yet. Its "Why a slice per file" says "Before this step the launcher made the whole BootFS executable ... Now each file gets its own slice", and its "Why every wait became a Loop slot" says "`Loop::run()` is the only place the root blocks (I1)". On dev after step 13, the standalone launcher still makes the whole BootFS executable, and init still blocks outside the Loop.

**Cause**
Plan decision P2 (a) cuts the switch into four commits so that each failure points at one piece: step 13 adds the root beside the old path, and steps 15 and 16 take the old path away. Until then both run, and two of the design's invariants hold only in a weaker form:
- **VMEX has two holders, and one of them holds Execute over the whole image.** Before step 13, init handed the launcher a duplicate of VMEX and closed its own ("vmex is the launcher's alone"). Step 13 keeps init's VMEX, as the design says the root must, but still hands the launcher its duplicate, and the launcher still calls `launch::make_bootfs_launcher()` (`system/service/launcher/launcher.cpp:315`), which makes an executable copy of the whole BootFS with it: the one handle D15 rejected, covering manifests and configuration too. I6 says it holds "while the root is the only VMEX holder on dev". What keeps its substance true meanwhile is narrower: the second holder is trusted code that makes only the read-only BootFS executable, and it answers Launch with process handles only, never a VMO, so no client can hold a VMO with Execute whose bytes someone can write. Step 14's Launcher in the root accepts VMOs from clients and parses them in place on the strength of I6 (D14 b); the premise "the root alone holds VMEX" comes back with step 16, which deletes the standalone launcher.
- **init still blocks outside the Loop.** `start_shell()` and `start_autorun()` (`system/service/init/init.cpp:243`, `:213`) call `launch::Client::launch()`, which writes the request and then waits in `object_wait_one` for the launcher's reply (`system/lib/launch/src/client.cpp`). They run after `Realm::start_root()` has started routing_checks and before `Loop::run()`, so routing_checks' first Read waits until the shell has been launched, and a launcher that never answered would leave /boot unserved for good. It cannot deadlock, since the launcher needs nothing from init, but it is a wait outside `port_wait`, which I1 forbids. I1 holds in full from step 15, when init stops starting the shell itself, and once autorun is started by direct call (D25 a) instead of through the standalone launcher.

**Change**
Read the step 13 entry's two sentences as:

```text
Before:  Before this step the launcher made the whole BootFS executable ... Now each file gets its own slice ...
After:   The root's /boot gives each file its own slice, executable only under bin/; until step 16 the
         standalone launcher, which still gets a VMEX duplicate, keeps its executable copy of the whole BootFS.

Before:  So every wait is now a slot on one port, and Loop::run() is the only place the root blocks (I1).
After:   init's own waits on the shell are slots now; until step 15 it still blocks in launch::Client::launch()
         for the shell and autorun, before the Loop runs.
```

The step 13 entry is not edited: change records are append-only.

**Reference**
- Fuchsia component_manager is the one holder of VMEX at boot: `BootfsSvc::ingest_bootfs_vmo_with_system_resource()` (`src/sys/component_manager/src/bootfs.rs:170-188`) mints it from the System resource and spends it in `ingest_bootfs_vmo()` (`:233`) on each executable file's own VMO; no second process holds an executable copy of the image.
- Fuchsia's root never blocks on another process: `async_loop_run()` (`sdk/lib/async-loop/loop.c:327-337`) is its only wait, and its runner reaches the Launcher through futures, not a blocking call.
- Design I1, I6, D14, D15 and D25; plan decision P2 (a).

**Verification**
- `python3 scripts/lint_docs.py --changed docs/changes/261004/init.md`: no violations.
- `boot_check.py`: dbg PASS, rel PASS, unchanged, since nothing but this document changed.

**Deliberately not done**
- No code change and no repair item: both are the transitional state P2 (a) chose, and steps 15 and 16 of the plan already remove them.

**How it was found**
Review of step 13: checking each claim of its "Cause" against `system/service/launcher/launcher.cpp` and the calls `init.cpp` still makes before `Loop::run()`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Between steps 13 and 16, which processes hold VMEX on dev, and why does I6 still hold in substance even though its stated premise does not?
- Step 14's Launcher parses a client's VMO in place. Which property of every Execute-carrying VMO makes that safe, and what would break it?
- init calls `launcher.launch()` after routing_checks is running. What does routing_checks see during that call, and why can it not deadlock?

## Title: the root serves each component's /svc, routed on every Open, and its built-in protocols fuchsia.process.Launcher and fuchsia.kernel.DebugResource (component-framework step 14) (2026-10-04)

**Files**
- `system/service/init/launcher_server.hpp`, `launcher_server.cpp` — new: `LauncherConnection`, one entry of the Launcher's pool, a constexpr-constructed `launch::ProcessLauncherState` whose hooks point back at the entry; `LauncherServer`, which binds connections, hands each message to the state and starts what a Launch asks for with `launch::start()`
- `system/service/init/debug_resource_server.hpp`, `debug_resource_server.cpp` — new: `DebugResourceServer`, which answers each Get with a duplicate of the root's Debug resource
- `system/service/init/environment.hpp` — new: `Environment`, moved out of `realm.hpp`, since the runner and the Launcher both start programs with it
- `system/service/init/boot_server.hpp`, `boot_server.cpp` — the tree also holds every component's /svc: `add_svc_directory()`, `add_service()`, `serve()` of a node, and a `Connector` that an Open of a service node is handed to; /boot and /svc each held to their own share of the nodes
- `system/service/init/realm.hpp`, `realm.cpp` — BUILTINS gains the two protocols; `run()` serves a /svc for a component that uses any protocol and hands it on as the NsDir `/svc`; `serve_svc()`, `connect()`, which routes every Open again, and `forward()`, which writes the Open into a provider's outgoing directory; `log_route_error()`, shared by directory and protocol routes
- `system/service/init/pools.hpp` — `MAX_DEBUG_RESOURCE_CONNECTIONS`, `MAX_PROTOCOL_USES`, `MAX_SVC_NODES`; `LOOP_SLOTS` counts the DebugResource connections; `INIT_OWN_HANDLES` counts the shell's duplicate of the Debug resource
- `system/service/init/init.cpp` — the Launcher's storage, `[[clang::no_destroy]] constinit`, and DebugResource's; both servers built before the Realm; the root keeps the Debug resource and gives the shell a duplicate
- `system/service/init/BUILD.gn` — the new sources, and the fuchsia-kernel, fuchsia-process and `launch:process_launcher` libraries
- `system/service/init/test/routing_checks/BUILD.gn`, `routing_checks.cpp` — three protocol uses, and the checks of /svc, DebugResource, the Launcher's refusals and a spawn of itself; `child` mode
- `kernel/products/image/dev/BUILD.gn` — root.cm offers the two built-in protocols to routing_checks
- `docs/repairs/261004/init.md` — items 8 (the route walked on every Open, D11) and 9 (the Launcher's fixed pool, D13), and item 7 counts /svc's connections
- (fsio's export and spawn have their own entry in `fsio.md`, and ProcessLauncherState's constexpr constructor in `launch.md`)

**Symptom**
None; this is step 14 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`). Before it, the root skipped every protocol use, BUILTINS held only `boot`, and no component could reach a protocol or start a program.

**Cause**
The objects of a connect through /svc:
- the component, with its namespace entry `/svc`, a channel to a directory the root serves;
- that directory, a node of the root's vfs tree, beside /boot, with one service node per protocol the manifest uses, each carrying a key: which instance, which of its uses;
- the router (`component::route()`), which walks one use through the parents' offers to a built-in or down through a child's exposes to a provider;
- the built-in servers in the root (LauncherServer, DebugResourceServer), and a provider's outgoing directory, reached through the client end the root made at resolve.

The events of `fsio::connect("/svc/fuchsia.process.Launcher", server)`:
1. fsio writes `Open("fuchsia.process.Launcher", PROTOCOL_SERVICE, server)` on the component's `/svc` channel and returns: Open is one-way.
2. The root's slot for that connection fires. vfs checks the Open (CONNECT among the rights, a service node, no other flag) and answers `Action::Connect` with the node's key and `server`.
3. BootServer hands both to its Connector, `Realm::connect()`, which takes the key apart and routes that use again, now.
4. The route ends at a built-in: `server` becomes a fresh connection of LauncherServer, on a slot of its own. Or at a provider: the root writes `Open("svc/<name>", PROTOCOL_SERVICE, server)` on the provider's outgoing directory and forgets it. Or nowhere: `server` is closed with an epitaph carrying the route's status, NotFound or AccessDenied, and the root logs one line.
5. From then on the component talks to whoever holds `server`. The root is out of the path unless it is the server.

Why each piece is shaped the way it is:
- **Why /svc lives in /boot's tree.** A /svc is a fuchsia.io directory like any other: Open, ReadDirents, Clone and Close are the same code, and the vfs tree was built as a set of trees for exactly this (`vfs::Tree::add_root()`). One tree means one server and one connection table, the 128 of D27, and one slot callback. The cost is that BootServer's name now undersells it, and that /boot and /svc share node storage; each is held to its own share (`MAX_BOOT_NODES`, `MAX_SVC_NODES`), so /boot cannot eat /svc's nodes and the handle sums, which count a slice per /boot node, stay true.
- **Why route again on every Open (D11).** The route is a function of the manifests and of the providers' states. The manifests never change, but a provider can exit (D23), after which its routes answer NotFound. Walking again each time is what Fuchsia's `RouterEntry::open_entry_async()` does per request, and it costs a few linear scans per Open; caching is D11 (b), left for a measurement (repair item 8).
- **Why a built-in gets a fresh connection, and a provider a forwarded Open.** The root is the server of its built-ins, so it binds `server` straight into the built-in's pool, as component_manager starts one task per connection (`LaunchTaskOnReceive`, `root_input_builder.rs:111-119`). A provider is another process: the root only passes the request on, through the outgoing directory the provider serves, as `OutgoingConnector::send()` does (`instance.rs:800-822`), and does not wait for an answer (I1). Nothing exercises the provider path until step 17's echo, but `route()` can already return one, and the root must do something with it.
- **Why the Launcher's pool is constinit, and its hooks point at the entry.** A user program runs no dynamic initialisers and has no `__cxa_atexit`. The pool's states need their start and close hooks in place before `main()` runs anything, and `ProcessLauncherState` takes them in its constructor, which step 14 made constexpr (`launch.md`). A hook's context must then be an address known at compile time: the entry itself (`this` in `LauncherConnection`'s constexpr constructor), not the LauncherServer, which `main()` builds on its stack. The entry finds its server through `m_server`, set when `bind()` takes it. The state's destructor is not trivial, so the global is `[[clang::no_destroy]]`: nothing registers it, and a connection that ends is `reset()`, which closes every handle the client added. The price is that the pool is written into init's image (repair item 9).
- **Why the Launcher's start is the root's own `launch::start()`.** The state judges the Add* calls and encodes the processargs (step 6); the root's hook makes the bootstrap channel, calls `launch::start()` on the client's VMO under the client's job, and writes the processargs with the client's handles and its own two, VmaRoot and VmoVdso (D19). `start()` maps the VMO with Execute into the root first, so a VMO without the right is refused before a byte of it is parsed (D14, I6): routing_checks' "VMO without Execute" check is that refusal, logged by the root as `init: launch failed: mapping the program's file into this process: AccessDenied`.
- **Why the root keeps the Debug resource.** Until now it handed its only handle to the shell. DebugResource.Get answers with a duplicate, so the root keeps the original and the shell gets a duplicate instead, one more handle charged to init until step 15 (`INIT_OWN_HANDLES`, 14). Fuchsia's builtin answers with a duplicate the same way (`DebugResource::serve()`, `debug_resource.rs:29-38`); where a duplicate fails, its `?` drops the connection, while ours answers the reply's status, since every reply here has room for one.
- **Handle charges.** Per running component with a protocol use, the /svc pair was already in the design's 8 + 2k. Per Launch, the root pays three handles while the process runs (the process, its root region, the vDSO duplicate) and one for as long as the client keeps the process; per DebugResource.Get, one until the client closes it. All of that comes out of `HANDLES_FOR_CLIENTS` (D18, repair item 4).

**Change**
The runner, after the directory uses:

```cpp
if (has_protocols) {
    ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
    if (auto const served = serve_svc(instance, server); served != Status::Ok) {
        close_with_epitaph(server, served);              // and one log line
    }
    names[num_names] = SVC_PATH;                         // "/svc"
    handed.add(client, ours::handle_info(HandleType::NsDir, num_names));
}
```

`serve_svc()` adds a directory and one service node per protocol use, keyed `instance << 32 | use`, and serves it with r*. Every Open of a node comes back to:

```cpp
auto const routed = component::route(instance, use, BUILTINS);   // again, every time (D11)
if (routed.is_err())              { log_route_error(...); close_with_epitaph(channel, status); }
else if (routed->provider)        { forward(*routed->provider, routed->name, channel); }
else if (name == LAUNCHER_NAME)   { status = m_builtins.launcher->bind(channel); }
else if (name == DEBUG_RESOURCE_NAME) { status = m_builtins.debug_resource->bind(channel); }
```

with a full pool answering NoResource on the channel (D27). The Launcher's pool:

```cpp
class LauncherConnection final {
  public:
    constexpr LauncherConnection()
        : m_state(launch::LauncherOps{.start = &LauncherConnection::start,
                                      .close = &LauncherConnection::close, .context = this}) {}
    ...
};

[[clang::no_destroy]]
constinit init::LauncherServer::Storage launcher_storage{};
```

The pools: 4 Launcher and 4 DebugResource connections, 15 protocol uses per component, `8 * (1 + 15) = 128` /svc nodes beside /boot's 256, and `3 * 8 + 128 + 4 + 4 + 2 = 162` Loop slots. The handle sums: `1 + 256 + 14 = 271` fixed, `271 + 8 * 38 = 575` for a full pool, 449 left for clients.

routing_checks uses `fuchsia.process.Launcher` and `fuchsia.kernel.DebugResource`, which root.cm offers from its parent, and `ours.test.Unoffered`, which nobody offers. It now also checks:
- its namespace holds five entries, `/svc` among them, and ReadDirents of `/svc` lists `fuchsia.process.Launcher` as a service;
- DebugResource.Get answers Ok with a handle that `debug_read(resource, nullptr, 0, ...)` accepts, which the kernel allows only for the System Debug resource;
- the unoffered protocol's connection carries a NotFound epitaph;
- the Launcher answers AccessDenied for a VMO without Execute, InvArgs after AddHandles with a VmaRoot, InvArgs after 63 handles, and closes the connection with nothing to read after an eight-byte message;
- `fsio::spawn()` of `/boot/bin/no_such_program` answers NotFound;
- `fsio::spawn()` of itself with `child` starts a copy that reads its own first bytes through the namespace it was handed, finds no `/denied`, and exits 0.

The rel log gains two lines from the root, both expected:

```text
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
init: launch failed: mapping the program's file into this process: AccessDenied
```

Rejected:
- A second vfs tree and server for /svc: a second connection table to size, a second slot callback, and the same fuchsia.io code twice, for a name that would read better.
- Renaming BootServer to say it serves /svc too: the design names the object BootServer; whether to rename it is the user's call.
- The Launcher's hooks pointing at the LauncherServer, set when `main()` builds it: the pool could then not be constinit, and every entry would need a run-time step before it is safe to use.
- Checking that the Debug resource is the System Debug resource before serving it, as Fuchsia's `DebugResource::new()` does: the root takes it from userboot by its handle_info, which already says so; a wrong one would show as the shell's `debug_read` failing.

**Reference**
- Fuchsia component_manager at `51f05104c13`: `RouterEntry::open_entry_async()`, the route on every request (`src/sys/lib/runtime_capabilities/src/fidl/router.rs:72-104`); `add_builtin_protocol_if_enabled()` and `LaunchTaskOnReceive` (`src/sys/component_manager/src/root_input_builder.rs:69-120`); `OutgoingConnector::send()` (`src/sys/component_manager/src/model/component/instance.rs:800-822`); the namespace's /svc as a dictionary served with `PROTOCOL_DIRECTORY | PERM_READABLE` (`src/sys/component_manager/lib/serve_processargs/src/namespace.rs:189-196`); `RouteSourceShutdown` as NOT_FOUND (`src/sys/lib/routing/src/error.rs:424-429`).
- `ProcessLauncher::serve()` and `ProcessLauncherState` (`src/sys/lib/elf_runner/src/process_launcher.rs:57-63`, `:87-111`); `DebugResource::new()` and `serve()` (`src/sys/component_manager/lib/builtins/src/debug_resource.rs:18-38`).
- Design: `docs/design/261004/component-framework.md`, "Events: a component connecting to a protocol through /svc", "Events: the shell running a program", "built-ins", I1, I2, I6, D10, D11, D13, D14, D18, D19, D23, D27.

**Verification**
- `boot_check.py`: dbg PASS (28 passed, 1 skipped, 0 failed, 0 error lines), rel PASS (25 passed, 3 skipped, 0 failed, 1/1 required, 0 error lines), both as before the step; dev's log holds `routing_checks: passed` and `init: routing_checks exited with 0`, and the kernel logs the spawned copy's process, `/boot/bin/routing_checks`, being destroyed.
- Mutation checks, each made by hand on the rel image and reverted:
  - DebugResource dropped from root.cm's offer: `init: routing_checks's use of "fuchsia.kernel.DebugResource" routes nowhere: NotFound ...` and a FAILED line for Get;
  - the spawned child made to exit 7: `routing_checks: the child exited with 7` and a FAILED line;
  - the two built-ins' servers swapped in `Realm::connect()`: five FAILED lines, Get, the three Launch refusals and the spawn, each answering PeerClosed, since each server closes a message of the other's protocol without an epitaph.
- `llvm-readelf -S` on init: no `.init_array`; `llvm-nm -S`: `launcher_storage` in `.data`, 29,024 bytes, `debug_resource_storage` in `.bss`.
- Host tests of the libraries this step touched: fsio 34, process_launcher 15, all passed.
- clang-format clean on every changed line; clang-tidy reports nothing on `init.cpp`, `boot_server.cpp`, `realm.cpp`, `launcher_server.cpp`, `debug_resource_server.cpp`, `routing_checks.cpp` and the fsio sources.

**Deliberately not done**
- The shell as a component, using the Launcher and DebugResource: step 15. The standalone launcher and the name-based protocol stay until step 16.
- The provider path of `connect()` runs only from step 17, when echo exposes a protocol; until then `forward()` is reached by no route.
- A per-(instance, use) route cache: D11 (b), repair item 8, for a measurement first.
- The Launcher's buffers out of `.data`: repair item 9.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- routing_checks' Open of `/svc/ours.test.Unoffered` succeeds, yet its first read on the new channel finds NotFound. Which process decided that, at which moment, and why could fsio::connect() not have said so itself?
- The Launcher's pool is built at compile time. Why must each state's hook context be its own pool entry rather than the LauncherServer, and what does `[[clang::no_destroy]]` keep the compiler from emitting?
- Every Open of a /svc entry walks the route again. What could change between two Opens of the same entry that a cached route would get wrong, and what would caching save?

## Title: comments and repair item 4 catch up with step 14 (2026-10-04)

**Files**
- `system/service/init/pools.hpp` — `MAX_DEBUG_RESOURCE_CONNECTIONS` names its server's header, `debug_resource_server.hpp`, not a `debug_resource.hpp` that does not exist
- `system/service/init/init.cpp` — `start_launcher()`'s comment no longer says the Launcher has yet to move into the root
- `docs/repairs/261004/init.md` — item 4 counts DebugResource.Get's duplicates among what clients take, gives 449 handles to spare instead of 450, and says that any component could exhaust the quota, not that nothing can

**Symptom**
None in behaviour. Three texts still described the tree as it was before step 14: a comment pointing at a header that is not there, a comment saying the Launcher "moves into the root (step 14)" in the future tense, and a repair item whose number and list of client charges were step 13's.

**Cause**
Step 14 changed three things those texts depend on, and updated the code but not every sentence that restates it:
- the DebugResource server's files are `debug_resource_server.hpp` and `.cpp`, after the `LauncherServer` pattern, while the pool constant's comment was written with a shorter name;
- the root now serves `fuchsia.process.Launcher` itself, so the standalone launcher that `start_launcher()` starts is kept only because the shell still reaches it until step 15;
- `INIT_OWN_HANDLES` went from 13 to 14 (the shell's duplicate of the Debug resource), so `HANDLES_FOR_CLIENTS` went from 450 to 449, and DebugResource.Get became a third way, beside GetBackingMemory and Launch, for a client to make the root pay a handle it holds. The item's "cannot be triggered today" was already too strong at step 13: a component that calls GetBackingMemory in a loop and keeps the VMOs exhausts the quota, and from step 14 one that launches programs and keeps their process handles does too. What keeps it latent is only that routing_checks, the one component, keeps nothing.

A repair item that understates how close its trigger is reads as less urgent than it is, which is exactly what the item exists to prevent.

**Change**
Before (repair item 4): "Latent, cannot be triggered today. Every GetBackingMemory duplicate ... with 450 handles to spare". After: "Latent, not triggered by today's components, though any component could trigger it ... Every GetBackingMemory and DebugResource.Get duplicate ... with 449 handles to spare". The two comments are reworded to the same effect.

**Reference**
None, this project's own sums (`pools.hpp`, the design's "Handle charges" and D18).

**Verification**
- clang-format clean on the changed lines; `lint_docs.py` clean on both documents.
- `boot_check.py`: dbg PASS, rel PASS, unchanged, since only comments and documents changed.

**Deliberately not done**
None.

**How it was found**
Review of step 14: `grep` for the names and numbers step 14 changed (`debug_resource`, `step 14`, `450`, `INIT_OWN_HANDLES`) across `system/service/init` and `docs/repairs/261004/init.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which handles does the root pay for when routing_checks calls DebugResource.Get, and for how long?
- Why does keeping the original Debug resource in the root and giving the shell a duplicate cost the root one handle in its fixed sums, when the original costs it nothing?
- Which three requests can a component repeat, keeping what each returns, to exhaust the root's handles, and which of them only became possible with step 14?
