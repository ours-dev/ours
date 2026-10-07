# component-framework: init grows into a component_manager-style root that serves /boot, starts components from manifests and routes their capabilities

**Date**: 2026-10-04
**Status**: Accepted

## Problem
Today init starts one program itself with the launch library. That program is the standalone launcher. init hands it VMEX, the BootFS and the vDSO, then closes its own VMEX (`system/service/init/init.cpp:143-206`, `:388-401`). Every later program is a Launch request that names a BootFS file (`system/lib/launch/include/launch/protocol.hpp:28-42`). The shell gets the raw BootFS VMO and a launcher connection (`init.cpp:264-286`). It lists files from its own mapping and runs commands by BootFS name (`examples/shell/shell.cpp:155-167`, `:240-261`, `:311-343`).

What is missing:
- No process has a namespace.
- Nothing serves a file protocol.
- Nobody reads the startup list. init starts the shell hard-coded (`init.cpp:411`), yet `config/init/startup` is still built (`kernel/user_bootfs.gni:40-54`, `kernel/products/image/dev/BUILD.gn:54-67`).

On 2026-10-04 the user chose Fuchsia's model in full (S1-S6 under Decisions). That choice supersedes three things:
- the standalone launcher (`system/service/launcher`) and its name-based protocol;
- the plan for a VFS process with a Linux-style mount table (`.agents/tasks/vfs/DESIGN.md:7-40`);
- the rule "Everything plugs into one Linux-style VFS" (`.agents/skills/ours-kernel-design/references/boot.md:68-73`), which has to be rewritten when this design is recorded.

The new model mostly restores stage 4 of `docs/design/260924/userspace-filesystem.md` (FS below), where init served /boot and /svc (FS:297-305). Four things change:
- the Launcher takes VMOs;
- a file's executability is decided by its directory at ingest;
- manifests and routing replace FS's fixed service list (FS:334-357);
- the codecs are hand-written.

Path conventions: Fuchsia paths are in the checkout at `51f05104c13`, and `cm/` means `src/sys/component_manager/src/`.

## Goals and non-goals
Goals:
- On dev, userboot starts `bin/init`, and init is the root. The root serves /boot from the BootFS with a fuchsia.io server library, holds VMEX, and makes each file under `bin/` executable at ingest.
- The root starts the components that `meta/root.cm` names. It builds each one's namespace by routing the `use` declarations in its manifest, and serves each one's /svc.
- `fuchsia.process.Launcher` lives in the root, and the shell runs commands the way fdio_spawn does.
- One component exposes a protocol that a sibling uses (D3 a), so that expose and forwarding into a provider's outgoing directory run on the target, not only in host tests.
- Manifests are GN targets that a Python compiler turns into `meta/<name>.cm` at build time. The BootFS contents stay generated from GN metadata.
- dev.dbg_1 keeps booting, and both images pass `ours-verify` after every step.

Non-goals: each item below is part of Fuchsia's model and is left out on purpose.
- **Lazy start.** Every child of the dev image is needed at boot. It can be added later without changing the manifest format, because outgoing channels are created at resolve (D4).
- **Collections, dynamic children, and the Realm and Binder framework protocols.** Nothing creates components at runtime; the shell spawns plain processes (D12).
- **Environments, and resolvers or runners other than the boot resolver and ELF.** There is one URL scheme and one runner.
- **Packages, `/pkg`, package directories and a package resolver.** The BootFS is the whole system (D7).
- **Shared libraries, ld.so and the loader service.** Programs link only the vDSO. The `lib/` executable rule waits for the first shared library.
- **Events and event streams.** Only the root observes lifecycle.
- **Storage capabilities and writable file systems.** There is no writable file system yet.
- **Structured configuration and config capabilities.** `program.args` and files under /boot cover today's needs.
- **Dictionaries, aggregated services, `include` shards and `to: "all"`.** At this size they only save typing.
- **`availability`, `from: "void"` and weak dependencies.** Every route is required and strong.
- **Stop, Kill, escrow, restart and `on_terminate: reboot`.** There is no `task_kill` and no reboot call (D23).
- **A job per component, and job policy.** There is no `job_create` or `job_set_policy` (D6).
- **component_manager's security policy.** Fuchsia checks each connection to a built-in against a capability allowlist (`cm/sandbox_util.rs:240-248`); `fuchsia.kernel.DebugResource` may reach only listed monikers (`src/security/policy/component_manager_policy_base.json5:27-38`). In ours routing is the only gate, so any parent can hand DebugResource to any child.
- **Directory capabilities provided by components, and vfs remote nodes.** A component cannot declare or expose a directory; the only directory source is the root's built-in `boot` (D3). So there is no forwarding of a namespace entry into a provider, and no remote node splicing another server into a served tree (`src/storage/lib/vfs/rust/src/remote.rs:108-123`).
- **The Launcher's `CreateWithoutStarting`, `SetOptions` and `AddEnvirons`.** `start()` starts the process itself (`system/lib/launch/include/launch/launch.hpp:68-80`), and nothing sends an environment.
- **Most of fuchsia.io:** writes and every other mutation, Seek, Watch, symlinks, node-only connections, the INHERIT flags, Open's options table, and the members no step uses (OnRepresentation, GetAttributes, ReadAt, Rewind). /boot is read-only, and nothing waits for entries to appear yet.
- **A whole-image route check at build time (Fuchsia's scrutiny).** Routes are checked at runtime, as in Fuchsia. This can be a later step.
- **Performance work.** The choices that cap performance are listed as decisions (D11, D13-D18, D27). Measuring and tuning go to the user item by item.

## Design
Objects first, then the events. Two functions are for the user to write by hand (P3 in `.agents/tasks/component-framework/PLAN.md`), so this section gives their specs, not their code: the router's `route()` and the Loop's dispatch.

### Objects
| Object | What it is | Where in ours | Fuchsia counterpart |
|---|---|---|---|
| userboot | The first user process, which starts `userboot.next` with the root job, the read-only BootFS, VMEX, the vDSO's VMO, the Debug resource and a root VMA; unchanged. | `kernel/lib/userabi/userboot/userboot.cpp:296-330` | userboot, whose default next program is `bin/component_manager+--boot` (`zircon/kernel/lib/userabi/userboot/main.rs:14`) |
| root | init grown into the owner of the component tree, serving /boot, /svc and the built-ins and running the ELF runner and the Launcher on one thread. | `system/service/init` | component_manager (`cm/builtin_environment.rs`) |
| Loop | One port and a fixed table of slots `{handle, signals, callback, generation}`, through which every wait in the root goes. | `system/lib/loop` (new; FS:308-315) | async-loop (`sdk/lib/async-loop`, `async_loop_run` at `include/lib/async-loop/loop.h:144`) |
| wire | Hand-written framing: the 16-byte header with a direction byte, the epitaph and handle placeholders. | `system/lib/wire` (new; FS:57-79) | `fidl_message_header_t` |
| fuchsia-io | The messages of the fuchsia.io subset. | `system/lib/fuchsia-io` (new) | `sdk/fidl/fuchsia.io` |
| vfs | The server library for that subset: connections with rights, the Open checks, and three node kinds (pseudo-directory, VmoFile, service). | `system/lib/vfs` (new) | `src/storage/lib/vfs/rust/src`: `Simple` (`directory/simple.rs:44`), `VmoFile` (`file/vmo.rs:41`), `Service` (`service.rs:85`) |
| BootServer | The vfs tree built from the BootFS at ingest, one slice per file, executable under `bin/`. | `system/service/init` (new file) | BootfsSvc (`cm/bootfs.rs:53`, `:262-312`) |
| bootfs | The flat-name parser `View`, which only gains the page rule. | `system/lib/bootfs` | the parser BootfsSvc calls |
| component manifest | A `component_manifest()` GN target whose Python compiler writes `meta/<name>.cm` into the BootFS. | `system/component_manifest.gni`, `system/component_manifest.py` (new) | `.cml` and cmc (`tools/cmc/src/compile.rs:18-82`); `fuchsia_component_manifest` (`build/components/fuchsia_component.gni:82-148`) |
| decl | The compiled manifest and its decoder. | `system/lib/component` (new) | `fuchsia.component.decl/Component` (`sdk/fidl/fuchsia.component.decl/component.fidl:13-62`) |
| instance | The root's record of one component: moniker, state, decl, children, outgoing client end, process. | in the root | `ComponentInstance` (`cm/model/component/mod.rs:322-330`); `InstanceState` (`cm/model/component/instance.rs:101-113`) |
| boot resolver | Turns a child's URL into `/boot/meta/<name>.cm` and decodes it, by direct call into BootServer. | in the root | `cm/builtin/fuchsia_boot_resolver.rs` |
| router | Walks one use through offers and exposes to its source, checking rights and joining subdirectories at each hop. | `system/lib/component` (new) | `route_directory` (`src/sys/lib/routing/src/lib.rs:952-975` at `9e015f591c4^`) over `route_from_use` (`legacy_router.rs:56`), Fuchsia's directory router until 2025-07-24 |
| ELF runner | Builds what a started component's process gets: the binary's VMO, argv, the namespace and the startup handles. | in the root | `configure_launcher` (`src/sys/lib/runner/src/component.rs:329`) |
| Launcher | The `fuchsia.process.Launcher` server: per-connection Add* state, then `launch::start()`. | in the root, over `system/lib/launch` | `ProcessLauncher` (`src/sys/lib/elf_runner/src/process_launcher.rs:57-63`, `:87-111`) over process_builder |
| built-ins | What `from: "parent"` means at the root: the directory `boot` (rx*) and the protocols `fuchsia.process.Launcher` and `fuchsia.kernel.DebugResource`, a fixed list. | in the root | `boot` is a namespace capability (`src/sys/component_manager/configs/bootfs_config.json5:7-14`); the protocols are builtin capabilities (`:25`, `:50`, with `use_builtin_process_launcher` at `:5`), registered by `add_builtin_protocol_if_enabled` (`cm/root_input_builder.rs:69-120`); `DebugResource` (`src/sys/component_manager/lib/builtins/src/debug_resource.rs:17-26`) |
| fsio | The client library: a namespace table from path prefix to directory channel, path cleaning, open, read, connect and spawn. | `system/lib/fsio` (new; FS:419-451) | the fdio namespace (`sdk/lib/fdio/namespace/local-filesystem.cc:56`, `:241`, `:454`, `:674`); `fdio_spawn` (`sdk/lib/fdio/spawn.cc`) |
| svc::Outgoing | The provider side: serves `svc/<name>` on the DirectoryRequest handle. | `system/lib/svc` (new, last step) | `component::OutgoingDirectory::ServeFromStartupInfo` (`sdk/lib/component/outgoing/cpp/outgoing_directory.h:127`) |
| routing_checks | An on-target test component whose manifest has deliberately narrowed and missing routes. | `system/service/init/test/routing_checks` (new) | `src/sys/component_manager/tests/rights`, `tests/routing` |

### States, ownership and locking
- **Instance states.** Unresolved, then Resolved (decl decoded, children created as Unresolved, outgoing pair made if it has a program), then Started (process running), then back to Resolved when the process exits, as Fuchsia's stop moves Started back to Resolved (`cm/model/component/mod.rs:696-705`). ours marks the instance `exited`, and D23 (a) refuses to start it again; Fuchsia would restart it on its next route. Fuchsia's Shutdown and Destroyed (`instance.rs:101-113`) are not needed: instances are never destroyed.
- **vfs connection.** Holds `{channel, node, rights, readdir cursor, seek offset, generation}`. Open or Clone creates one; Close or PEER_CLOSED frees it.
- **Launcher connection.** Holds a string arena, up to 61 handles with their infos, and an overflow mark. 61 because a processargs message carries at most 64 handles (`system/public/ours/process_args.hpp:214-216`) and the root adds VmaRoot, VmoVdso and ProcSelf (62 before ProcSelf was added on 2026-10-07, see D19). An Add* that does not fit sets the mark, and Launch answers InvArgs, as Fuchsia answers INVALID_ARGS for a processargs too large (`src/lib/process_builder/src/process_args.rs:24-28`, `:189-193`, `:227-229`). The state is reset after every Launch, as `ProcessLauncherState` is (`process_launcher.rs:57-63`, `:109-110`).
- **Ownership.** The root owns every instance, connection and slot, in fixed pools (D27), because user programs have no heap (`system/lib/ort/src/cpp_dependencies.cpp:10-26`). An instance owns its children. Instances are never destroyed.
- **Handle charges.** The kernel charges a handle to the process that created it, wherever the handle is now, up to 1024 (`system/public/ours/syscall.hpp:49-60`). The root pays for:
  - fixed: the port, and one slice per BootFS file;
  - per started component with k directory uses: 9 + 2k while it runs (the JobDefault duplicate, the outgoing pair, the /svc pair, k NsDir pairs, the process, VmaRoot, VmoVdso and ProcSelf); the bootstrap pair is gone by then, since ort closes its end at start (`system/lib/ort/src/start.cpp:83`). All of them go once the process is dead and its slots have closed;
  - per Launch: 4 while the process runs (the process, VmaRoot, VmoVdso and ProcSelf), then 1, the process handle, for as long as the client keeps it, which may be long after the process died;
  - per GetBackingMemory and per DebugResource.Get: 1, until the client closes it.

  A client pays for the channels it creates, including the server ends it passes in Open (D18).
- **Locking.** There are no locks: one thread runs everything (D16). ort's startup-handle table is unlocked for the same reason (`system/lib/ort/include/ort/startup_handles.hpp:22-26`). State is protected by the Loop's rule that one callback runs at a time, to completion.

### Events: boot
1. userboot starts `bin/init` (`kernel/products/image/dev/BUILD.gn:144`) with JobDefault, VmoBootfs (read-only), VMEX, VmoVdso, Debug and VmaRoot (`userboot.cpp:296-330`). In Fuchsia, userboot starts `bin/component_manager+--boot` (`main.rs:14`).
2. The root takes those handles, as init does today (`init.cpp:329-364`). It keeps VMEX for life: the code that gives VMEX up (`init.cpp:396-401`) goes.
3. Ingest:
   - The root maps the BootFS read-only for good (`map_bootfs`, `system/lib/launch/include/launch/bootfs.hpp:53-61`) and checks it with `bootfs::View`, which now refuses entries that break the page rule (PLAN step 2).
   - For each entry it splits the flat name on `/`, creates the pseudo-directories along the way, and adds a VmoFile holding a slice of the read-only BootFS VMO at the entry's page-aligned range (`vmo_create_child` with Slice, `system/public/ours/syscalls.gni:833-852`).
   - When the first segment is `bin`, it replaces that slice with `vmo_replace_as_executable` (`syscalls.gni:884-897`): one call per binary, and no handle with Execute covers the whole image (D15).
   - BootfsSvc decides by path the same way, with the root directories `bin`, `lib`, `blob` and `test` executable (`cm/bootfs.rs:53`, `:294-305`), and calls `replace_as_executable` on each executable file's own VMO (`:300-301`). Ours starts with `bin` only (S3).
4. The root registers its built-ins. `boot` is BootServer's root directory with rights rx*, Fuchsia's namespace capability (`bootfs_config.json5:7-14`). The two protocols are servers inside the root, Fuchsia's builtin capabilities (`:25`, `:50`; `cm/builtin_environment.rs:563-573`).
5. The root resolves the fixed URL `fuchsia-boot:///#meta/root.cm` (D26) by calling into its tree directly, decodes it, and creates the children as Unresolved. Fuchsia fixes its root URL too (`cm/startup.rs:10`). Its `root.cml` has children and no program (`src/sys/root/root.cml:5-20`), and so does ours, so the root instance has no outgoing pair.
6. Starting the root means starting its eager children, one after another in manifest order, because there is one thread. A child that fails to start is logged and the next one still starts. Fuchsia starts them concurrently with `join_all` and logs a failure the same way (`cm/model/component/mod.rs:1205-1231`).
7. If `ours.autorun.boot` is set, the root starts it by direct call (D25), after the eager children, as init starts it today once the shell is up (`init.cpp:425-437`).
8. The root enters the Loop and never leaves it.

### Events: starting a component
1. **Resolve.** The root reads `/boot/meta/<name>.cm` by direct call, decodes it (structural checks only, see "The manifest"), and creates the child instances. If the decl has a program, it makes the outgoing-directory pair:
   - It keeps the client end, with a slot for READABLE and PEER_CLOSED, as FS's o_c (FS:322-326). A provider never writes to it, so any message closes it, before a provider can park 64 messages full of handles there; PEER_CLOSED means the provider is gone.
   - The server end waits for the start, as in component_manager's escrow (`cm/model/escrow.rs:114-136`), which Fuchsia makes only for a component with a runner (`instance.rs:538-547`). Opens forwarded before the start queue in the channel.
2. **Directory uses.** For each one, the root routes now (D8) and makes a channel pair. The source is always the built-in `boot` (D3):
   - The root binds the server end directly as a new BootServer connection, with the use's rights, at the joined subdirectory. No message is sent. Fuchsia instead opens its own `/boot` and forwards the request (`NamespaceDirectoryRouter` in `cm/root_input_builder.rs`).
   - If routing fails, the root closes the server end with an epitaph and logs one line.
   - The client end becomes an NsDir handle whose name is the use's path.
3. **Protocol uses.** If there is any, the root makes one /svc pair. It serves the server end as a pseudo-directory with one service node per used protocol, with the flags Fuchsia serves a namespace dictionary with, PROTOCOL_DIRECTORY and PERM_READABLE, less the INHERIT flags (`src/sys/component_manager/lib/serve_processargs/src/namespace.rs:189-196`). PERM_READABLE is r*, which holds CONNECT and TRAVERSE (`sdk/fidl/fuchsia.io/rights-abilities.fidl:47-48`). The client end becomes the NsDir "/svc". Every protocol use lives at `/svc/<name>` (D3); Fuchsia groups protocol objects by their paths and serves the `svc` dictionary as one directory the same way (`cm/model/namespace.rs:114-163`).
4. **Runner.** The root looks up `program.binary` in its tree by direct call and takes the file's VMO, which must be executable. It then calls `launch::start()` directly, never through its own /svc (invariant I2).
5. **Processargs.**
   - argv[0] is `/boot/<binary>`, the counterpart of Fuchsia's `/pkg/<binary>` (`src/sys/lib/runner/src/component.rs:370-382`), followed by `program.args`.
   - The handles are JobDefault (a duplicate of the root job, D6), the NsDir entries, DirectoryRequest (the outgoing server end), VmoVdso, VmaRoot and ProcSelf (D19).
   - Fuchsia's runner sends the same through `configure_launcher` (`component.rs:329`). process_builder adds the vDSO (`src/lib/process_builder/src/process_builder.rs:614-628`), and also ProcessSelf, ThreadSelf, RootVmar (`:227-229`) and StackVmo (`:661-664`); ours's `start()` closes the thread and the stack VMO; since 2026-10-07 every starter passes ProcessSelf as ProcSelf, with full rights, and none passes ThreadSelf (D19).
6. **Started.** The instance becomes Started. The root adds a TERMINATED slot on its process, logs `init: started <moniker>`, and then starts the component's own eager children.

### Events: a component opening a file in /boot
1. At start, fsio binds each NsDir handle at its name, as fdio does when it reads processargs (`sdk/lib/fdio/unistd.cc:627-636`).
2. `fsio::open("/boot/bin/x", R)`:
   - cleans the path (fdio's `CleanPath`, `sdk/lib/fdio/cleanpath.cc:21`);
   - finds the longest bound prefix, `/boot`;
   - makes a channel pair and writes `Directory.Open("bin/x", flags, server)` on the /boot channel.

   Open is one-way, so the client may send Read on its end at once. Binds may not overlap, so the longest prefix is exactly what fdio's tree walk finds (`local-filesystem.cc:56`, `:241`).
3. The root's slot for that /boot connection fires, and vfs decodes the Open. It checks in the Rust vfs's order (`src/storage/lib/vfs/rust/src/directory/connection.rs:391-417`):
   - the path syntax (D21);
   - TRAVERSE, for any path other than ".";
   - that the requested rights are a subset of the connection's rights;
   - CONNECT, when the flags ask for PROTOCOL_SERVICE;
   - GET_ATTRIBUTES, when attributes are requested; unreachable while Open's options table is cut;
   - the node itself: EXECUTE is allowed only on an executable file (`file/common.rs:31-33`).

   On success it adds a connection slot for `server` with the requested rights. On failure it sends an epitaph on `server`.
4. Read copies from the root's BootFS mapping into the reply (D17). GetBackingMemory duplicates the file's VMO with Basic, Map and GetProperty plus Read and, when asked, Execute, as VmoFile always adds BASIC, MAP and GET_PROPERTY (`file/vmo.rs:180-193`). Basic holds Duplicate (`system/public/ours/rights.hpp:57`), which `start()` needs with Read, Map and Execute (`launch.hpp:25-28`). Asking for EXECUTE needs the connection's EXECUTE; PRIVATE_CLONE gets the same duplicate (D21).
5. Close is answered and the slot freed. PEER_CLOSED also frees it.

### Events: a component connecting to a protocol through /svc
1. `fsio::connect("/svc/fuchsia.process.Launcher", server)` writes `Open("fuchsia.process.Launcher", PROTOCOL_SERVICE, server)` on the component's /svc channel, as `fdio_service_connect` does through the namespace (`sdk/lib/fdio/directory.cc:19-26`).
2. The root finds the service node. The node's callback routes the component's use (D9).
3. **Built-in source.** The root binds `server` as a fresh connection of that built-in server, as `LaunchTaskOnReceive` starts one task per connection (`cm/root_input_builder.rs:111-119`). Fuchsia first asks its security policy (`cm/sandbox_util.rs:240-248`); ours has none (non-goal).
4. **Component source P.** The root writes `Open("svc/<name>", PROTOCOL_SERVICE, server)` on P's outgoing client end, as `OutgoingConnector::send` does (`cm/model/component/instance.rs:800-822`), and does not wait. From then on the component and P talk directly.
5. **Routing error.** The root sends an epitaph on `server` and logs one line naming the target, the hop and the capability. The status follows Fuchsia's mapping (`src/sys/lib/routing/src/error.rs:405-441`): NotFound in general, AccessDenied for rights. A source that has exited also answers NotFound, as `RouteSourceShutdown` does (`error.rs:424-429`, D23).
6. Every Open routes again (D11), as `RouterEntry` does (`src/sys/lib/runtime_capabilities/src/fidl/router.rs:72-104`).

### Events: the shell running a program
1. The shell is a component. It uses `boot` (rx*) at /boot, plus the Launcher and DebugResource protocols.
2. For the line `x a b`, it opens `/boot/bin/x` with READ+EXECUTE and calls `GetBackingMemory(READ | EXECUTE | PRIVATE_CLONE)`, the flags `fdio_get_vmo_exec` asks for (`sdk/lib/zxio/get_vmo.cc:142-144`). The VMO comes back with the Execute right because the node is executable and the connection holds EXECUTE.
3. It connects to `/svc/fuchsia.process.Launcher` and sends, in order:
   - AddArgs `["x", "a", "b"]` (`spawn.cc:896-897`);
   - AddHandles with a duplicate of its JobDefault (`:638`);
   - AddNames with a Clone of each namespace entry (`:655`);
   - Launch with the VMO, the job and the name (`:989`).

   This is fdio_spawn's order, which sends AddEnvirons between AddArgs and AddHandles when it has an environment (`:917-918`); ours cuts AddEnvirons.
4. The root's Launcher runs `start()`, which first maps the VMO with PermRead and PermExecute into the root (D14): a VMO without Execute is refused there, before a byte of it is parsed. It plans and binds from that mapping, maps the program into the new process, and unmaps it from the root. It writes the processargs, adding VmaRoot, VmoVdso and ProcSelf, answers `{Ok, process}`, and resets the connection's state.
5. The shell waits for TERMINATED on the process, as it does today (`shell.cpp:345-350`). The Launcher resolved no name: the shell's own namespace decided what `x` is.

### Events: a component exiting
1. The TERMINATED slot fires. The root reads the return code with `object_get_info` and logs `init: <moniker> exited with <code>`, as `report_exit` does for the shell today (`init.cpp:303-322`). Fuchsia's ELF runner does the same and treats code 0 as a clean stop (`src/sys/lib/elf_runner/src/lib.rs:860-898`). The callback closes the process handle, its own slot's handle, and nothing else (I3).
2. The instance goes back to Resolved, marked `exited` (D23).
3. Everything else closes itself. The component's handles close with its process, so the root's /svc server end, each vfs connection it held and the outgoing client end see PEER_CLOSED, and each closes in its own callback. TERMINATED and these PEER_CLOSED packets may come in either order; each callback only marks the instance (FS:325).
4. Later routes to it answer NotFound (D23). Nothing restarts it.

### Events: dev.dbg_1
- userboot starts `bin/shell` directly (`kernel/products/image/dev.dbg_1/BUILD.gn:79`) and hands it the root job, the BootFS and VMEX. There is no root and no namespace.
- The shell keeps its library path (`start_with_library`, `shell.cpp:263-309`), `ls` over its own View, and the fault probes. It tells the images apart by what it is handed: an NsDir "/boot" on dev, VmoBootfs and VMEX on dbg_1 (PLAN step 15).
- Fuchsia has the same kind of image: tests that userboot starts directly, with no component_manager (`zircon/kernel/lib/userabi/userboot/tests/BUILD.gn:38-51`).

### Concurrency: one thread and one Loop
Before this section is discussed, the user writes their own argument for D16: which invariant one thread protects, who races with the root, and the worst interleaving. The section is then the answer it is graded against.

Objects:
- the root's single thread;
- its port;
- one one-shot, level-triggered wait per slot;
- clients and providers, running on other CPUs at the same time.

Invariants:
- **I1. The root waits only in `port_wait`.** Every write to a client or a provider is fire-and-forget. A write to a full channel (64 messages, NoResource; `kernel/object/include/ours/object/channel_dispatcher.hpp:19-27`) fails that write alone: its handles are consumed, the peer later sees PEER_CLOSED with no epitaph, and the root logs and goes on. When the failed write is a reply, the root also closes that connection's slot, from that connection's own callback, since its client would otherwise wait forever (FS:521). Fuchsia's remote node drops the channel the same way (`src/storage/lib/vfs/rust/src/remote.rs:115-123`), and component_manager warns that a component ignoring its outgoing directory can fill the channel and crash it, checking liveness every 500 Opens (`cm/model/escrow.rs:29-35`, `:160-188`).
- **I2. A blocking call to yourself deadlocks, so the root never makes one.** Its runner, resolver and autorun reach the tree and `launch::start()` by direct call, because the Loop has no continuations: a callback that sent a request to the root and waited for the reply would block the very thread that must read it. component_manager avoids this by being async: a call to itself is an awaited future, so its executor keeps serving, with one thread (the default, `src/sys/lib/cm_config/src/lib.rs:606`) as with the two its boot config sets (`bootfs_config.json5:6`). Where it does block, in fdio calls on its own /boot such as the boot resolver's `Path::exists` (`cm/builtin/fuchsia_boot_resolver.rs:60-66`), BootfsSvc serves /boot on a thread of its own "to avoid reentrancy issues" (`cm/bootfs.rs:394-395`).
- **I3. Only a slot's own callback closes its handle.** Packets carry `generation << 32 | index`, so a stale packet is dropped (FS:311-314). This is why the exit path closes nothing that has a slot.
- **I4. One message per callback, then re-arm.** A busy client goes to the back of the queue (FS:315).
- **I5. The direction byte means a reply never reads as a request** (FS:69). So a client that hands the root both ends of one channel cannot make it talk to itself, the loop that `protocol.hpp:44-50` closed in the old launcher.
- **I6. Every VMO a client can hold with Execute has bytes nobody can write.** They are BootFS files the root made executable, and the vDSO. The BootFS VMO has no Write right; `vmo_replace_as_executable` drops Write (`syscalls.gni:893-894`); a snapshot drops Execute (`:851-852`). It holds while the root is the only VMEX holder on dev. D14 (b) rests on it.

Worst interleavings:
1. **A launch stalls everyone.**
   - t0: the shell sends Launch.
   - t1: routing_checks sends Read on /boot.
   - t2: the root runs the whole launch: a map and an unmap, snapshots, and one system call per relocation (`docs/repairs/261002/launch.md:69-78`).
   - t3: only now is the Read served.

   Every /boot and /svc request waits for the longest callback (D16).
2. **A provider dies with Opens queued.**
   - t0: the root writes `Open("svc/x", server)` on P's outgoing channel.
   - t1: P dies before reading it. The kernel closes P's end, with the message and `server` inside it. The client sees PEER_CLOSED with no epitaph.
   - t2: the root sees PEER_CLOSED on the outgoing client end and TERMINATED on P's process, in either order, and marks P exited.
3. **Quota exhaustion.** A client launches programs and keeps their process handles. Each costs the root three handles while the process runs and one for as long as the client keeps the handle. At 1024 (`syscall.hpp:60`), every Launch and every GetBackingMemory in the system answers NoResource (D18).

### The manifest
The source is a GN scope that uses CML's key names (D1):

```gn
# examples/shell/BUILD.gn
component_manifest("shell.cm") {
  program = ":shell"
  manifest = {
    use = [
      {
        directory = "boot"
        path = "/boot"
        rights = [ "rx*" ]
      },
      {
        protocol = [
          "fuchsia.process.Launcher",
          "fuchsia.kernel.DebugResource",
        ]
      },
    ]
  }
}

# kernel/products/image/dev/BUILD.gn
component_manifest("root.cm") {
  children = [
    {
      name = "shell"
      manifest = "//examples/shell:shell.cm"
      startup = "eager"
    },
  ]
  manifest = {
    offer = [
      {
        directory = "boot"
        from = "parent"
        to = "#shell"
      },
      {
        protocol = [
          "fuchsia.process.Launcher",
          "fuchsia.kernel.DebugResource",
        ]
        from = "parent"
        to = "#shell"
      },
    ]
  }
}
```

How the template builds it:
- It writes the scope out as JSON with `generated_file`, the syscalls pattern (`system/public/BUILD.gn:46-51`).
- It adds the user toolchain to the program and child labels and refuses a label that already names one, as `user_bootfs` does for programs (`kernel/products/image/dev/BUILD.gn:102-111`). Without it, `root.cm` in the image's BUILD.gn would load `examples/shell/BUILD.gn` in the kernel toolchain, whose assert fails gen (`examples/shell/BUILD.gn:13-15`).
- It reads the binary's install name from `<program>.install_name` metadata, as `user_program_names()` does (`system/user_program_names.gni:26-28`), so no program name is spelled by hand.
- It writes each child's URL as `fuchsia-boot:///#meta/<name>.cm` (D26), compiles the result to `meta/<name>.cm`, and packages it with `distribution_entries`.
- Children are GN labels, so a dangling child fails at gn gen.

The compiler runs cmc's per-manifest checks at build time:
- child and capability names are unique;
- `from: "self"` names a declared capability, and every `#child` reference exists;
- an offer's target differs from its source, and each (target, target_name) pair appears once;
- use paths are unique and none is nested inside another;
- offers between children form no cycle;
- the rights words are known;
- `startup` is written out as `"eager"` (D4);
- `program.binary` lies under `bin/`;
- a protocol use has no `path`: it is always `/svc/<name>` (D3);
- `capabilities` declares protocols only, and no `expose` names a directory (D3).

The root's decoder checks structure only: bounds, record counts, string-table references, known enum values, and indices that resolve. That is enough to stay memory-safe on any input. It trusts the semantic checks to the build, which alone writes the BootFS. Fuchsia instead re-runs `cm_fidl_validator::validate` at every resolve (`src/sys/lib/routing/src/resolving.rs:133-143`), for example its use-path checks (`src/sys/lib/cm_fidl_validator/src/lib.rs:928-930`); this departure is recorded below.

The compiled form (D2) is a header with a magic and a version, fixed records (Program, Child, Capability, Use, Offer, Expose) that keep decl's fields (source, source_name, target, target_name, target_path, rights, subdir), and one string table.

### The routing walk
`route()` is the function the user writes by hand in PLAN step 8. Its spec:
- **Input.** The target instance and one of its uses: kind (protocol or directory), source name, and for a directory its rights and subdir. A use always comes from the parent (D3).
- **Output.** A Route: the source (a built-in, or a provider instance with its path `svc/<name>`), the joined subdir, and the use's rights. Or a RouteError: the status (NotFound or AccessDenied), the moniker where the walk stopped, the verb (Offer, Expose or Declare, or Use when the use's own subdir is longer than MAX_PATH_LENGTH, which the compiler refuses and the decoder does not) and the capability name.
- **Walk.** Start at the target with `from = parent`, then repeat:
  - **parent:** at the root, look the name up among the built-ins of that kind. Otherwise find the parent's offer of that kind to this instance whose target name is the name, or fail NotFound (parent, Offer); apply the hop; continue at the parent with the offer's source name and source.
  - **child:** resolve the named child if it is not yet, find its expose of that kind whose target name is the name, or fail NotFound (child, Expose); apply the hop; continue at the child.
  - **self:** find the declared capability, or fail NotFound (instance, Declare) when it is missing or the instance has no program; apply the hop; done.
- **Hop rules.** Every hop that fails returns its error at once.
  - Rights: walking from the use toward the source, the rights each hop states must contain the rights stated at the hop before it, the first being the use's own; otherwise the route answers AccessDenied. This is Fuchsia's `validate_next` (`src/sys/lib/routing/src/rights.rs:19-33`), walked by `RightsWalker` in `route_directory` (`lib.rs:953-957` at `9e015f591c4^`). The final connection carries exactly the use's rights.
  - Subdirectories: each hop puts its own subdir in front of the one the request already carries, as `DirectoryState` accumulates it (`lib.rs:978` at `9e015f591c4^`).
- **Termination.** `route()` terminates because `resolve()` fails once the root's fixed instance pool (D27) is full; step 13 must keep that true, so its `resolve()` never grows the pool or evicts a resolved instance to make room. The walk is monotone: offers from a parent only move up, and once the walk turns down the sources can only be self or a child, since an expose from the parent is refused. The steps up are bounded by the target's depth, but the steps down are not bounded by the manifests: a manifest that names itself as a child, which the decoder accepts, and exposes a capability from that child under the same name, makes a tree with no bottom. Each step down resolves an instance one level deeper than the last, so the walk ends, NotFound at that child with Expose, once the pool has no room for its children.

Under D3 (a), rights and subdirs appear only on the way up to the built-in `boot`, and the child and self branches carry protocols.

### The protocol subset
Each member is kept only if a step uses it.

| Message | Selector | Ordinal | Used by |
|---|---|---|---|
| Directory.Open | `fuchsia.io/Directory.Open3` (`sdk/fidl/fuchsia.io/directory.fidl:441`; `openable.fidl:18`) | `0x568ddcb9a9cbb6d9` | every client |
| Node.Close | `fuchsia.io/Node.Close`, declared in `fuchsia.unknown.Closeable` (`sdk/fidl/fuchsia.unknown/unknown.fidl:38`) | `0x5ac5d459ad7f657e` | fsio |
| Cloneable.Clone | `fuchsia.unknown/Cloneable.Clone`, no `@selector` (`unknown.fidl:20-23`) | `0x20d8a7aba2168a79` | fsio's export for AddNames |
| File.Read | `fuchsia.io/File.Read`, declared in `Readable` (`file.fidl:300`) | `0x057e419a298c8ede` | routing_checks |
| File.GetBackingMemory | `fuchsia.io/File.GetBackingMemory` (`file.fidl:191`) | `0x00a6a9e654cbf62b` | fsio's spawn |
| Directory.ReadDirents | `fuchsia.io1/Directory.ReadDirents` (`directory.fidl:475`) | `0x3582806bf27faa0a` | `ls`, routing_checks |
| epitaph | FS's framing (D20) | none | every error |
| Launcher.AddArgs | `fuchsia.process/Launcher.AddArgs` | `0x3be445d3e4fd6512` | fsio's spawn |
| Launcher.AddHandles | `fuchsia.process/Launcher.AddHandles` | `0x51025267a537a615` | fsio's spawn |
| Launcher.AddNames | `fuchsia.process/Launcher.AddNames` | `0x2579ee2c7be28662` | fsio's spawn |
| Launcher.Launch | `fuchsia.process/Launcher.Launch` | `0x11335a9928afbfa4` | fsio's spawn |
| DebugResource.Get | `fuchsia.kernel/DebugResource.Get` | `0x1d79d77ea12a6474` | the shell |

- **Launcher and DebugResource** are closed protocols with no `@selector` (`sdk/fidl/fuchsia.process/launcher.fidl:113-185`, `LaunchInfo` at `:45-54`; `sdk/fidl/fuchsia.kernel/debug-resource.fidl:11-16`), so an unknown ordinal closes the connection.
- **Ordinals** follow fidlc's rule: the first 8 bytes of the selector's SHA-256, little-endian, top bit cleared (`tools/fidl/fidlc/src/compiler.cc:28-45`). The values above were computed that way. The tree has no SHA-256 in C++, so a Python test (hashlib) reads each `{selector, ordinal}` pair from the codec headers and recomputes it.
- **Malformed messages** close the connection without an epitaph, as Fuchsia's `serve` returns an error and drops the channel (`process_launcher.rs:92`) and FS closes on a decode failure (FS:524).
- **Limits.** A message holds at most 4096 bytes and 64 handles (`syscall.hpp:44-47`). MAX_PATH is 1024 and MAX_TRANSFER is 4072 (FS:76-79).

### Where ours departs from Fuchsia
| Fuchsia | ours | Why |
|---|---|---|
| An async component_manager, one executor thread by default and two in the boot config (`cm_config/src/lib.rs:606`; `bootfs_config.json5:6`); its runner reaches the Launcher through a namespace's /svc (`process_launcher.rs:366-388`) | One thread with no continuations; the runner calls `launch::start()` directly | A thread cannot be started in a running process (`syscalls.gni:603-611`); I2; D16 |
| BootfsSvc on a thread of its own, with a VMO per file filled by `transfer_data` (`bootfs.rs:202-219`, `:394-395`) | The same thread; a slice per file | D16, D15 |
| `.cml` in JSON5, compiled to persisted FIDL (`compile.rs:76-82`) | A GN scope, compiled to a flat format | D1, D2 |
| Every resolve re-validates the manifest (`resolving.rs:133-143`) | The decoder checks structure only; cmc's semantic checks run once, at build time | Only the build writes the BootFS |
| Lazy by default (`tools/lib/cml/src/types/child.rs:48-55`) | Eager only | D4 |
| Eager children started concurrently (`mod.rs:1211`) | In sequence | One thread |
| A router object per declaration, built at resolve | The older walk over declarations, at route time | D9 |
| Directory entries routed on their first message (`serve_processargs/src/namespace.rs:145-175`) | Routed at start | D8 |
| A protocol use has a path, `/svc/<name>` by default (`cm/model/namespace.rs:114-163`) | Always `/svc/<name>` | D3 |
| A stopped component goes back to Resolved and starts again on its next route (`mod.rs:696-705`; `escrow.rs:240-273`) | Back to Resolved, never restarted; routes to it answer NotFound | D23 |
| A job per component, with policies (`elf_runner/src/lib.rs:395-432`) | A duplicate of the root job | D6 |
| A security policy gates built-ins (`sandbox_util.rs:240-248`) | Routing is the only gate | Non-goal |
| /pkg in every namespace | No /pkg | D7 |
| The FIDL wire format; 64 KiB messages (`syscall.hpp:42-44`), MAX_PATH_LENGTH 4095 (`sdk/fidl/fuchsia.io/io.fidl:50`), MAX_TRANSFER_SIZE 8192 (`file.fidl:12`) | FS's header with flat bodies and Fuchsia's ordinals; 4 KiB, 1024, 4072 | D20 |
| The Launcher has seven methods; process_builder reads headers with `vmo_read` (`process_builder.rs:619`) and maps nothing into itself; ld.so binds dynamic programs | Four methods; `start()` maps the program with Execute into the root, reads it in place, and binds a static program's vDSO imports itself | No shared libraries; D14 |
| Autorun runs in console-launcher (`src/bringup/bin/console-launcher/main.cc:106-125`) | Autorun runs in the root | D25 |

## Alternatives considered
- **A VFS process with a Linux-style mount table, the BootFS mounted at /boot** (`.agents/tasks/vfs/DESIGN.md:7-40`). Superseded by S1. Fuchsia has no VFS process: each process's namespace plays the mount table's role, and an access ceiling is a connection's rights, not a mount flag.
- **Keep the standalone launcher, which resolves BootFS names in its own view.** Superseded by S4. A launcher that resolves names acts with its own authority on a client's behalf. A VMO carrying the Execute right carries the decision instead, and only a VMEX holder can create that right.
- **A table fixed in the root's code for the first version** (FS's service list, FS:334-357). Rejected by S5.
- **The protocol generator first (G3, G4).** Rejected by S6.
- **Fuchsia's current router graph (D9 b), or routes computed at build time (D9 c).** See D9.
- **/boot on a thread of its own, as BootfsSvc does (D16 b).** It needs a kernel change; one thread is enough until measurements say otherwise.
- **The shell starting components (D12 b).** It needs collections and the Realm protocol, both cut.
- **Routing directory entries on their first message (D8 b).** This is Fuchsia's way. It costs a wait slot per entry, and while `boot` is the only directory source the client sees only an earlier failure.
- **Parsing the client's VMO in place without a rights check (D14 a).** A client-made VMO can change under the parser. Copying headers and tables out (D14 c) is Fuchsia's way and is kept as the fallback.
- **One executable copy of the whole BootFS, sliced per file** (the draft's D15). It left the root holding one Execute handle over configuration and data files too; BootfsSvc makes only executable files executable.
- **Autorun in the shell (D25 b).** It needs the boot option handed to the shell, a new path, while the root reads it already.
- **A dbg_1 image that also boots the root (D22 b, c).** It would lose the one image where userboot starts a program directly, or add a third image to every verification.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| S1. Process structure | (a) keep the split: a VFS process, a standalone launcher, init supervising; (b) merge only /boot into the launcher; (c) Fuchsia's model in full | (a), `.agents/tasks/vfs/DESIGN.md:48` | Not asked | (c): a component_manager-style root, a namespace per process (as fdio_ns), executability as the VMO handle's Execute right (2026-10-04) |
| S2. Which process becomes the root | (a) init grows into it; (b) a new process | (a), `vfs/DESIGN.md:52` | Not asked | (a): userboot already starts `bin/init` on dev (2026-10-04) |
| S3. Who serves /boot and makes programs executable | (a) a VFS without VMEX, with the launcher converting VMOs; (b) the root serves /boot from the BootFS on a server library for a fuchsia.io subset, holds VMEX, and marks files executable at ingest by directory, as BootfsSvc | (a), `vfs/DESIGN.md:47` | Not asked | (b): `bin/` now, `lib/` once shared libraries exist (2026-10-04) |
| S4. The launcher | (a) a standalone process that resolves BootFS names; (b) in the root, working as fuchsia.process.Launcher: it takes an executable VMO and resolves no names | (a), `vfs/DESIGN.md:51` | Not asked | (b) (2026-10-04) |
| S5. How a process gets its capabilities | (a) a table fixed in the root's code first; (b) component manifests and capability routing from the start | (a), `vfs/DESIGN.md:53` | Not asked | (b) (2026-10-04) |
| S6. Protocol codecs | (a) hand-written: the fuchsia.io subset, the Launcher, everything manifest-related; (b) the generator first (G3, G4) | (a), `vfs/DESIGN.md:54` | Not asked | (a) (2026-10-04) |
| D1. Manifest source format | (a) JSON5 `.cml` files read by a hand-written Python reader (the host's Python is 3.9 with no JSON5 module); (b) strict-JSON `.cml` (stdlib `json`, no comments); (c) a GN scope with CML's key names in a `component_manifest()` target, with the program and the children as GN labels | (c): nothing is spelled twice, a dangling child fails at gn gen, and no parser is needed. Cost: no `.cml` files, a departure to record | Not asked | (c), as recommended (2026-10-04) |
| D2. Compiled manifest format | (a) a persisted `fuchsia.component.decl/Component` subset (8-byte header plus V2 tables), as cmc writes (`compile.rs:76-82`); (b) an ours flat format in the BootFS style: header with magic and version, fixed records, string table; (c) no compile step: ship JSON and parse it in the root | (b): the io codec (D20) has no FIDL tables whose decoder it could share; a linear scan suffices at tens of entries; the version field leaves room for change | Not asked | (b), as recommended (2026-10-04) |
| D3. Manifest subset, and where it is checked | (a) program {runner "elf", binary, args}; children {name, url, startup}; protocol capabilities only, the only directory being the built-in `boot`; use from the parent only, protocol uses always at `/svc/<name>`; offer from parent, self or a child, to a child, with `as`, and rights and subdir on directories; expose of protocols from self or a child; cmc's per-manifest checks at build time, structural checks at decode; (b) as (a) plus use from a child, weak dependencies and directories provided by components; (c) as (a) without expose and without declared capabilities, so every source is a built-in, and step 17 waits until a real component provides something | (a): expose is the part of routing a table cannot express, and S5 chose routing. Cost over (c): the svc library, the echo program, forwarding into an outgoing directory and step 17, all for a test, since no real component provides anything yet. The format and the router are the same under (c), so (a)'s extra is only step 17 | Not asked | (a), as recommended (2026-10-04) |
| D4. Startup | (a) eager only: the compiler rejects `lazy` and requires `startup: "eager"` to be written, since CML's default is lazy (`child.rs:48-55`); (b) eager and lazy, starting a provider on its first route (`instance.rs:1591-1612`) | (a). Outgoing channels are made at resolve, so (b) can be added later without changing the format | Not asked | (a), as recommended (2026-10-04) |
| D5. Root manifest | (a) one `component_manifest("root.cm")` per image, in its BUILD.gn, children named by GN label, packaged at the fixed path `meta/root.cm`, as Fuchsia fixes its root URL (`cm/startup.rs:10`); (b) children contributed as shards through GN metadata and merged by the compiler, as Fuchsia's assembly merges root shards; (c) the root manifest named by a boot option | (a) | Not asked | (a), as recommended (2026-10-04) |
| D6. Component jobs | (a) every component gets a duplicate of the root job as JobDefault, as the shell does today (`init.cpp:264-266`), and the gap is a repair item; (b) add `job_create`, `task_kill` and job policy first, and give each component a child job as the ELF runner does (`elf_runner/src/lib.rs:395-432`) | (a) now; (b) as a task of its own. Cost of (a): a component holding a job and an executable VMO can start processes itself, and nothing can stop a component | Not asked | (a), as recommended (2026-10-04) |
| D7. What a component sees as /pkg | (a) no /pkg: the runner takes the binary from /boot, and a component sees files only through `use directory`; (b) /pkg is all of /boot, rx | (a) | Not asked | (a), as recommended (2026-10-04) |
| D8. When a directory entry is routed | (a) at start; on failure the root closes that entry's server end with an epitaph, so the client sees what Fuchsia's client sees, only before it sends anything; (b) on the entry's first message, as Fuchsia (`serve_processargs/src/namespace.rs:145-175`) | (a): no wait slot per entry. If components ever provide directories, (a) costs more: a provider sees an Open for every entry, used or not, where Fuchsia waits for the entry's first message (`namespace.rs:151-161`) | Not asked | (a), as recommended (2026-10-04) |
| D9. Routing engine | (a) a walk over declarations at route time, Fuchsia's directory router until 2025-07-24 (`route_directory`, `lib.rs:952-975` at `9e015f591c4^`; protocols left it on 2024-08-14 in `76fd2bef1ee`, and the code was deleted in `0cca16f3e98`); (b) a router object per declaration, built at resolve (Fuchsia's current design); (c) routes computed at build time and shipped as a table | (a): (b) pays for dictionaries and dynamic offers, which ours cuts; (c) is the table fixed in code that S5 rejected | Not asked | (a), as recommended (2026-10-04) |
| D10. /svc | (a) the root serves one /svc directory per component, PROTOCOL_DIRECTORY and PERM_READABLE without the INHERIT flags; each entry routes on every Open, then forwards the channel (`router.rs:72-104`); (b) one NsDir per used protocol; (c) hand the component the provider's outgoing directory | (a) | Not asked | (a), as recommended (2026-10-04) |
| D11. Route caching (caps performance) | (a) walk again on every Open, as Fuchsia; (b) cache per (instance, use) while the tree is static | (a); measure before choosing (b) | Not asked | (a), as recommended (2026-10-04) |
| D12. How the shell runs a program | (a) fdio_spawn's way: open `/boot/bin/<cmd>` with READ+EXECUTE, call GetBackingMemory with READ, EXECUTE and PRIVATE_CLONE (`get_vmo.cc:142-144`), then the Launcher with AddArgs, AddHandles, AddNames and Launch (`spawn.cc:897`, `:638`, `:655`, `:989`); (b) start a component, which needs collections and the Realm protocol | (a) | Not asked | (a), as recommended (2026-10-04) |
| D13. Launcher wire shape (caps how many clients at once) | (a) Fuchsia's stateful Add* calls, then Launch (`launcher.fidl:113-185`), costing about 4.6 KiB of fixed state per connection slot; (b) one Launch message carrying everything, as today's protocol does | (a) | Not asked | (a), as recommended (2026-10-04) |
| D14. How the Launcher reads a client's VMO (caps per-launch cost; safety) | (a) map it read-only, plan and bind in place, unmap: a client that made the VMO itself keeps a writable handle and can change the headers while the root parses them, a double fetch in the most privileged process; (b) add `vma_unmap` and `vmo_get_size`; `start()` first maps the VMO with PermRead and PermExecute into the root, which `vma_map` refuses without Execute (`syscalls.gni:779-781`), so only an immutable VMO is parsed (I6); then plan and bind in place, and unmap; (c) `vmo_read` the headers and dynamic tables into private buffers, as Fuchsia reads headers (`process_builder.rs:619`; `src/lib/elf_parse/src/lib.rs:254-257`); needs `vmo_get_size` and a range reader in elfload, whose Binder takes the whole file as a span (`system/lib/elfload/include/elfload/binder.hpp:66`); no `vma_unmap` | (b): one check the kernel enforces and no change to elfload. Switch to (c) the day anything but the BootFS can be executable (a second VMEX holder, a JIT). (b) and (c) both close launch repair item 2 (`docs/repairs/261002/launch.md:23-41`). Cost of (b): a map and an unmap, with its TLB shootdown, per launch; a departure from FS §4.6, "the launcher maps nothing into itself" (FS:400), and from Fuchsia | Not asked | (b), as recommended (2026-10-04) |
| D15. One VMO per BootFS file (caps launch cost and root handles) | (a) a slice of the read-only BootFS per file at ingest, replaced with `vmo_replace_as_executable` under `bin/`: one call per binary, no handle with Execute over the whole image, N handles with the root; a launch's snapshot through a slice still costs in proportion to the whole BootFS (`launch.md:43-67`); (b) a snapshot per file, made executable under `bin/`. Fuchsia instead creates a VMO per file and moves the pages into it with `transfer_data`, sharing one VMO between entries with the same range and dropping WRITE from the rest (`bootfs.rs:202-219`, `:262-305`); ours has no such call | (a) now; choose (b) after the measurement repair item 3 asks for | Not asked | (a), as recommended (2026-10-04) |
| D16. Root threading (caps latency) | (a) one thread: /boot, /svc, routing, launches and supervision share the Loop, and the root reaches its own /boot and Launcher by direct call; (b) /boot on a thread of its own, as BootfsSvc (`bootfs.rs:394-395`), which needs a way to start a thread in a running process (`syscalls.gni:603-611`) | (a). The user writes the invariant argument before the Concurrency section is discussed | Not asked | (a), as recommended (2026-10-04) |
| D17. Copies on the read path (caps throughput) | (a) Read copies from the root's BootFS mapping into the reply (one copy in the root, one in the kernel); programs and bulk readers use GetBackingMemory; (b) fsio maps every file it reads through GetBackingMemory: no copy, but one root handle per open file | (a) | Not asked | (a), as recommended (2026-10-04) |
| D18. Root handle quota (caps live processes and VMOs) | (a) no per-client cap; the costs are the sums under "Handle charges"; NoResource shows up as a failed request; a repair item records it; (b) per-connection lifetime caps (FS's GRANT_MAX_BACKING_VMOS, FS:319), which break repeated spawns; (c) a kernel change that charges a transferred handle to its holder | (a) | Not asked | (a), as recommended (2026-10-04) |
| D19. Startup handles besides the namespace | (a) Fuchsia's set less ProcessSelf, ThreadSelf and StackVmo, which `start()` closes or never passes (`launch.hpp:76-80`): JobDefault, NsDir, DirectoryRequest, VmaRoot and VmoVdso (`process_builder.rs:614-628`), with argv[0] `/boot/<binary>`; the Debug resource only through the built-in `fuchsia.kernel.DebugResource`; init's ping channel removed; (b) keep the ping as a channel the root owns in a fixed slot, like Fuchsia's lifecycle channel; (c) the Debug resource as a startup handle enabled by a program key | (a) | Not asked | (a), as recommended (2026-10-04); amended 2026-10-07: every starter (the kernel, userboot, the root's runner, autorun and Launcher, and the shell's library starts) now also passes ProcessSelf, as ProcSelf with full rights, which the runtime keeps as `ort::process_self()`, so that a program can name itself, for its koid first of all |
| D20. Wire encoding of the hand-written codecs | (a) FS's 16-byte header with a direction byte and flat fixed-offset bodies (FS:57-79), with Fuchsia's protocol names and ordinals (`compiler.cc:28-45`); (b) FIDL's V2 wire format with tables and envelopes | (a) | Not asked | (a), as recommended (2026-10-04); revised by the user 2026-10-04: ours's own protocol and library names (ours.io, ours.process, ours.resource), ordinals computed from them |
| D21. fuchsia.io rules where FS's plan and Fuchsia differ | (a) follow Fuchsia: shrink rights to what the protocol uses instead of refusing them, "." first in ReadDirents, OutOfRange above the transfer limit (Rust vfs); paths by fuchsia.io's Path invariants: not empty, no leading or trailing '/', and either "." or valid names joined by '/' (`io.fidl:31-47`), stricter than the Rust vfs, which accepts a leading and a trailing '/' (`src/storage/lib/vfs/rust/src/path.rs:47-60`); PRIVATE_CLONE with EXECUTE answered by a duplicate, because /boot never changes and ours's snapshot drops Execute (`syscalls.gni:851-852`), where Fuchsia makes a no-write snapshot (`file/vmo.rs:212-224`); (b) keep FS §2.2-§2.4 as written | (a) | Not asked | (a), as recommended (2026-10-04) |
| D22. dev.dbg_1 | (a) unchanged: userboot starts the shell, which keeps its library path, `ls` over its View, and the probes, like Fuchsia's tests that userboot starts directly (`userboot/tests/BUILD.gn:38-51`); (b) dbg_1 also boots the root, with a root manifest of its own, and the direct-start image is gone; (c) dbg_1 boots the root, and a third image with the dbg.1 kernel keeps the direct start, at one more boot per verification | (a) for this task, with the gap a repair item. Its cost: dev runs the dbg.0 kernel (`kernel/products/image/dev/BUILD.gn:85`) and dbg_1 the dbg.1 kernel with DEBUG_ASSERT (`dev.dbg_1/BUILD.gn:27-31`), so the root, the busiest user of ports, full channels, handle transfer and `vma_unmap`, never runs with assertions on, although boot_check keeps two images for exactly that (`.agents/skills/ours-verify/scripts/boot_check.py:34-41`). (c) touches `products/bringup.gni:34`, boot_check and `scripts/kshell_run.py:72`, so it is a task of its own | Not asked | (a), as recommended (2026-10-04) |
| D23. A component that exits or crashes | (a) log its exit code; back to Resolved, marked `exited`; never started again; routes to it answer NotFound, as Fuchsia's `RouteSourceShutdown` maps (`error.rs:424-429`); no reboot; (b) start it again on its next route, which is what Fuchsia does with a stopped component (`escrow.rs:240-273`); (c) halt the system when a component marked critical dies, the counterpart of `on_terminate: reboot` | (a) | Not asked | (a), as recommended (2026-10-04) |
| D24. On-target test components | (a) `routing_checks` in dev's root manifest from the first root step; the shell spawning `routing_checks child` in its self-checks from step 15; an `echo` provider in step 17 that exits on request, after which a route to it answers NotFound; (b) test routing on the host only | (a) | Not asked | (a), as recommended (2026-10-04) |
| D25. Autorun (`ours.autorun.boot`) | (a) stays in the root: the name is resolved in its own /boot tree, for itself rather than on a client's behalf, and started through `launch::start()` with argv only, as today (`init.cpp:208-231`); (b) move it to the shell, as Fuchsia's console-launcher spawns it with `fdio_spawn_etc` on an absolute path (`console-launcher/main.cc:122-125`, `:194-196`); the option reaches init's environment from userboot (`kernel/lib/userabi/userboot/userboot.cpp:20-24`), so the root would have to hand it on, for instance as a built-in like Fuchsia's `fuchsia.boot.Arguments` (`bootfs_config.json5:20`) | (a) | Not asked | (a), as recommended (2026-10-04) |
| D26. A child's URL in the compiled form | (a) `fuchsia-boot:///#meta/<name>.cm`, with /boot as the package directory; the boot resolver's doc gives `fuchsia-boot:///path/within/bootfs#meta/component.cm` (`fuchsia_boot_resolver.rs:45-46`), but current Fuchsia resolves boot URLs only through a bootfs package index (`:84-110`) and refuses one without a path (`:349-352`); (b) a scheme of ours's own, such as `boot:meta/<name>.cm`; (c) no URL: the child record holds the BootFS path | (a): Fuchsia's spelling, and an empty path is the one reading that needs no package index. Closed by step 7 | Not asked | (a), as recommended (2026-10-04) |
| D27. Pool sizes (caps components, connections and launches) | (a) fixed pools, with static_asserts against PORT_MAX_OBSERVERS, 1024 (`system/public/ours/port.hpp:100`), and the 1024 handles (`syscall.hpp:60`) using the sums under "Handle charges": 8 components, 128 vfs connections, 4 Launcher connections (about 4.6 KiB each, D13), and Loop slots of 3 per component plus one per connection; a request past a pool answers NoResource on its server end; (b) pools sized by the manifest compiler from the image's root.cm | (a). Closed by step 13 | Not asked | (a), as recommended (2026-10-04) |
| D28. The root cannot start (no BootFS, VMEX, vDSO or VMA; no `meta/root.cm`; a manifest that fails to decode) | (a) log one line and halt, as init halts today (`init.cpp:74-87`, `:365-370`); (b) fall back to starting `bin/shell` directly, as dbg_1 does | (a) | Not asked | (a), as recommended (2026-10-04) |
| D29. The shell's names on dev | (a) `ls [dir]` lists one namespace directory through ReadDirents, `/boot` by default; a command containing '/' is an absolute namespace path, as console-launcher requires of autorun (`main.cc:122-125`); any other runs `/boot/bin/<cmd>`; no cwd; (b) `ls` walks /boot to keep today's flat output, and a path may be relative to a cwd fsio keeps (FS:426) | (a) | Not asked | (a), as recommended (2026-10-04) |

## Testing and verification
- **Host tests, one set per library:**
  - bootfs: the page rule (unaligned offset refused; overlapping rounded ranges refused; identical ranges and zero-size entries accepted) and a Builder round trip.
  - wire and fuchsia-io: a round trip for every message, every malformed form refused.
  - Ordinals: a Python test that recomputes every ordinal in the codec headers from its selector with hashlib.
  - vfs core: the Open check order (CONNECT included), rights shrinking, the EXECUTE gate, ReadDirents with "." and BufferTooSmall, OutOfRange, the rights GetBackingMemory hands out (PRIVATE_CLONE included), Clone keeping rights.
  - fsio: CleanPath; bind refusals (relative, overlapping, shadowing); the longest-prefix lookup.
  - Launcher codec and state: 62 handles accepted and 63 answered InvArgs at Launch, too many bytes answered the same, reserved handle types refused, the reset after Launch.
  - Manifest compiler (Python unittest): each per-manifest rule; `lazy`, a protocol use with `path`, a declared or exposed directory, and a label that names a toolchain all refused.
  - Decoder: a gtest that decodes `.cm` files the GN template built, so the two hand-written codecs cannot drift apart unnoticed, and refuses truncated and out-of-range inputs.
  - Router: a built-in source, a sibling reached through expose, an offer that narrows rights answering AccessDenied, subdir joining, NotFound naming the verb and the moniker, and a provider with no program.
- **On the target, both images through `ours-verify`:**
  - shell checks for `vma_unmap` (unmapping all or part of the sealed vDSO refused, and a range straddling a sealed mapping refused), `vmo_get_size`, the Loop, and `start()` refusing a VMO without Execute before parsing it;
  - dev's `routing_checks` component, which must print `routing_checks: passed`; `boot_check.py` requires that line and waits for it;
  - the shell spawning `routing_checks child`, and its rewritten Launcher checks;
  - after step 17, echo exiting on request and the next route to it answering NotFound.
- **Not verifiable yet:**
  - stopping a component, job isolation, lazy start, restart, the root's own death and more than one thread: none of these exist yet;
  - a child or the root failing to start on a real image (D28): the build refuses the manifests that would cause it;
  - the root under DEBUG_ASSERT, under D22 (a);
  - root handle-quota exhaustion and performance numbers: possible, but not measured yet.

## Open questions
- How long a manifest name may be. `fuchsia.component.decl` says 100 (`sdk/fidl/fuchsia.component.decl/types.fidl:6`), while cm_types takes its limit from its name module (`src/sys/lib/cm_types/src/lib.rs:108`). Step 7 must close it, since the compiled format fixes it.
- The root's own death: nothing notices it (`init.cpp:41-42`). Handling it needs a critical-process mechanism in the kernel; step 13 records it as a repair item.
- Library names (`wire`, `fuchsia-io`, `fuchsia-process`, `fuchsia-kernel`), closed by steps 3 and 6, and the echo protocol's name, closed by step 17. Closed again by the user on 2026-10-04 with ours's own names: the libraries `system/lib/ours.io`, `system/lib/ours.process` and `system/lib/ours.resource`, each directory named for its protocol library, `ours.io`, `ours.process` and `ours.resource` (D20); the kernel-resource library was first named `ours.kernel`, with the protocol `ours.kernel.DebugResource`, and is `ours.resource` with the protocol `ours.resource.Debug`.
- Per-connection revocation under the root needs `port_cancel` (FS:314). No step needs it yet.
