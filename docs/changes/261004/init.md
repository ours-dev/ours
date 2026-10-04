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
