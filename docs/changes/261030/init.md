# init changes

## The root offers ours.resource.Mmio, and the MmIo root reaches it from the kernel through both userboots (2026-10-30)

**Files**
- `kernel/lib/userabi/src/userboot.cpp` — `write_bootstrap()` mints the MmIo root after the I/O port root and sends it as `HandleType::MmioResource`; the handle and info arrays grow from 11 to 12.
- `kernel/lib/userabi/userboot/userboot.cpp` — the user-mode userboot takes `MmioResource`, keeps it in `HandedHandles` and passes it on to the next program; its arrays grow from 10 to 11; a missing resource gets a line of its own.
- `system/lib/ours.resource/include/ours_resource/ours_resource.hpp`, `test/ours_resource_test.cpp`, `README.md` — the protocol `MMIO_PROTOCOL`, `ours.resource.Mmio`, selector `ours.resource/Mmio.Get`, and its place in the library's tests.
- `system/lib/wire/test/ordinals_test.py` — the scan must find `ours.resource/Mmio.Get`.
- `system/service/init/builtins.json` — the built-in; `BUILTINS` is generated from this file.
- `system/service/init/init.cpp`, `realm.hpp`, `realm.cpp`, `pools.hpp` — a ResourceServer for it over the root's MmIo resource, its field in `BuiltinServers`, the branch of `connect()` that binds a route ending at it, and a sixth server counted in `LOOP_SLOTS`.
- `system/service/init/test/routing_checks/routing_checks.cpp`, `BUILD.gn` and `kernel/products/image/dev/BUILD.gn` — routing_checks uses the protocol, root.cm offers it to routing_checks alone, and the check asks the kernel what the resource it got is.
- `system/lib/component/include/component/route.hpp` — the comment that lists the root's built-ins.
- `.agents/tasks/driver-capabilities/DESIGN.md`, `PLAN.md` — the protocol's name.

**Symptom**
None; this is step 2 of `.agents/tasks/driver-capabilities/PLAN.md`. Before it the MmIo kind existed (step 1) but only inside the kernel's own test: no process was ever handed one, so no component could be routed one, and the syscalls that will take one (`vmo_create_physical`, step 3) would have had nothing to check.

**Cause**
The objects:
- the MmIo root: a resource of kind MmIo with base 0 and size 0, which covers the physical address space except what `validate_ranged_resource()` refuses, the kernel's own RAM and devices (step 1);
- the kernel's userboot: `write_bootstrap()` makes the first process's startup handles, among them the three roots of the ranged kinds;
- the user-mode userboot (`kernel/lib/userabi/userboot`): the first process, which starts the root and passes it everything it does not use itself;
- the root (`system/service/init`): takes the roots as startup handles and keeps them;
- a ResourceServer: one per ours.resource protocol, holding the resource it hands out and a pool of four connections, each on a Loop slot;
- `BUILTINS`: what `from: "parent"` means at the root, generated from `builtins.json`.

The events, from boot to a component holding the resource:
1. `write_bootstrap()` mints the MmIo root with `make_root_resource_handle(RsrcKind::MmIo, "mmio")` and puts it in the first process's processargs under `HandleType::MmioResource` (0x50, a number the ABI already named).
2. The user-mode userboot takes it with `take_startup_handle`, and `start_next()` adds it to the processargs of the next program, the root.
3. The root takes it, logs `init: holding the MMIO resource`, and builds a ResourceServer over it.
4. routing_checks opens `/svc/ours.resource.Mmio`; the root routes the use through root.cm's offer to `BUILTINS`, where the route ends, and binds the channel to the Mmio server.
5. The client writes `Get`; the server answers with a duplicate of the root with the same rights.
6. routing_checks asks `object_get_info(Resource)` and sees kind MmIo, base 0, size 0.

Why each piece is shaped as it is:
- Two arrays had to grow, and each is where a forgotten handle would be lost silently. The kernel's counts the most it can ever send (VmaRoot, ProcSelf, JobDefault, VmoVdso, Debug, Sampling, Irq, Ioport, Mmio, Crashlog and Bootdata is 11, and the array is 12); the message buffer is sized from `sizeof(infos)`, so a bigger array cannot outgrow it. The userboot's was exactly full at 10.
- The protocol is `Mmio`, not `MmIo`: the library names a protocol as it names `Ioport`, in the word's own case, while the resource kind stays `RsrcKind::MmIo`. The ordinal is the first 8 bytes of SHA-256 of `ours.resource/Mmio.Get` read little-endian with the top bit cleared, `0x1f6f'1386'9360'0a40`, which `ordinals_test.py` recomputes.
- Only routing_checks is offered it. pc-ps2 maps no device registers, and routing is the only gate, so a component that is not offered the protocol cannot reach the resource.
- A sixth ResourceServer is four more Loop slots; `LOOP_SLOTS <= PORT_MAX_OBSERVERS` is a `static_assert` and still holds.

**Change**
The root keeps and serves what the kernel sends:

```cpp
// before: the root took two roots
auto const irq = ort::take_startup_handle(ours::HandleType::IrqResource, 0);
auto const ioport = ort::take_startup_handle(ours::HandleType::IoportResource, 0);

// after: and a third, with a server and a branch in connect()
auto const mmio = ort::take_startup_handle(ours::HandleType::MmioResource, 0);
init::ResourceServer mmio_resource_server(*MMIO_RESOURCE_STORAGE.init(), loop,
                                          ours_resource::MMIO_PROTOCOL, mmio);
...
} else if (routed->name == ours_resource::MMIO_PROTOCOL.name) {
    status = m_builtins.mmio_resource->bind(channel);
```

Rejected: giving the resource to pc-ps2 as well, for symmetry with Irq and Ioport, because it would only widen who can map physical memory; and one protocol per range, which waits for a syscall that narrows a resource.

**Reference**
Fuchsia component_manager `MmioResource::serve()` (`src/sys/component_manager/lib/builtins/src/mmio_resource.rs`), registered in `BuiltinEnvironment::new()` only when the startup handle is there (`src/sys/component_manager/src/builtin_environment.rs:593-630`), the protocol `fuchsia.kernel.MmioResource` (`sdk/fidl/fuchsia.kernel/mmio-resource.fidl`). The kernel side is Zircon `userboot.cc:583`, `push_handle(get_resource_handle(ZX_RSRC_KIND_MMIO))`. Ours follows the same path as `Irq` and `Ioport` (`docs/changes/261005/init.md`).

**Verification**
- `ordinals_test.py`: 5 tests, OK, with the new selector in the list it must find.
- `boot_check.py`: build ok; dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (151 passed, 0 failed). rel's log has `init: holding the MMIO resource` and `routing_checks: passed`.
- A mutation, undone afterwards: `connect()` bound the Mmio route to the Ioport server. routing_checks printed `routing_checks: ours.resource/Mmio.Get = PeerClosed (expected Ok) FAILED`: the Ioport server's decoder refuses Mmio's ordinal and closes the connection, as a closed protocol does. With the real wiring the check passes, so the check is not vacuous.
- clang-tidy on `userabi/src/userboot.cpp`, `userabi/userboot/userboot.cpp`, `init.cpp`, `realm.cpp`, `routing_checks.cpp` and `ours_resource_test.cpp`: no diagnostics. clang-format: clean.
- Not measured: the arm and riscv kernels were not built, and nothing yet maps through the resource, so only its kind, base and size are checked.

**Deliberately not done**
- No syscall takes the resource yet; `vmo_create_physical` is step 3.
- No `HandleType::MmioResource` check in the shell, which takes the Irq and I/O port roots for its own tests.
- Registering the built-in only when the root holds the resource: `BUILTINS` is one constant list, and a root started without it still routes the protocol and answers each Get with the duplicate's failure, as it does for the other resources.

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Which two processes carry the MmIo root before a component can hold it, and what does each do with it?
2. Why does a component that is not offered `ours.resource.Mmio` in root.cm have no way to the resource, even though the root holds it?
3. What did the mutation test show, and why did the failure appear as `PeerClosed` and not as a wrong resource?


## Title: The root forwards a directory use that ends at a provider as one Open of its declared path (2026-10-31)

**Files**
- `system/service/init/realm.cpp` — `Realm::forward_directory()` is new; the branch of `Realm::run()` that answered `NotSupported` for a directory route that ends at a provider is gone; `forward()` is split into the two halves it shares with `forward_directory()`, `provider_serves()` (the exited-provider rule and its log line) and `write_open()` (encode, fire and forget, the "cannot forward" line); `to_flags()` turns a route's rights into the permission bits of an Open
- `system/service/init/realm.hpp` — the three declarations with their contracts, and the comment on `Instance::m_outgoing`

**Symptom**
Step 2 let a manifest declare, expose and offer a directory, and step 3 gave a provider's `Outgoing` a place to put one, but a consumer of such a directory still got nothing. `Realm::run()` routed the use, found a provider at the end, and closed the entry's server end with a `NotSupported` epitaph: every `open()` under that namespace entry answered `NotSupported`, and the root logged nothing, because it was not a routing failure but a branch that had not been written.

**Cause**
The root had one way to serve a directory entry, its own `/boot`: `m_boot->serve(server, subdir, rights)`. That was complete while design D3 allowed one directory, the built-in. A directory that a component provides cannot be served by the root. The entry is a channel pair (N, S): N goes to the consumer as its namespace entry, S is the end whoever serves the directory must hold. For `boot` the root holds S itself. For a provider, S has to get to the provider, and the one way into a provider is its outgoing directory, whose client end only the root holds. So the root writes an `Open` on that end with S as the request's object, which moves S into the provider's process, and from then on the consumer and the provider talk on (N, S) directly. The root keeps nothing for the entry: no connection, no Loop slot, no table entry. That is the property that matters (design invariant I1 and "After the start, the root holds nothing per directory entry"): the root is one thread that must never wait, and a filesystem will wait.

Each field of that Open is a decision with a way to get it wrong:

- The **path** is the declared path without its leading `/`, then `/<subdir>` when the joined subdirectory is not empty. The leading `/` has to go because an Open's path is relative to the connection it is written on (`Server::open()` refuses `/export/docs` as `BadPath`, and the first mutation below shows every directory use failing that way). The subdir is joined by the root and not split off, so that each server in the chain consumes only what it knows: the provider's server walks `export` and `docs` (the declaration) and then `sub` (the subdirectory the hops joined) in one walk, and a part never has to be passed to the next server.
- The **flags** are the use's rights as the `Perm*` bits, and `ProtocolDirectory`. A right is the same bit in a connection's `Operations` and in an Open's `Flags`, so the conversion is a cast, pinned by two `static_assert`s against `PERM_READABLE` and the mask of known permissions. The provider's server makes the connection with exactly these rights, and every later Open through it is checked against them, so rights only shrink from the entry on. `ProtocolDirectory` is what makes a route whose path ends at a file fail: without it the server sees an Open that accepts any node kind and opens the file for the entry, so a consumer asked for a directory and silently got a file. With it the server answers `NotDir`. Nothing before step 4 could observe this bit.
- The **object** is S, which the write consumes whatever it answers.

The states the provider can be in decide what the consumer sees, and `forward()` already had the rule for a protocol: an exited provider, or one whose outgoing client end the Loop has closed, is `NotFound` (D23), logged in the shape of a routing failure with the provider as the place the walk stopped. A directory is the same rule, so the rule became a function both call; copying it would have left two places to change when a third state appears. The same holds for `write_open()`: encode, write, and say `cannot forward <name> to <provider>` when the channel is full or gone. For a directory `<name>` is the directory's name at the provider.

A provider that has not started yet needs nothing: `resolve()` made its outgoing pair when the route walked through it, so the write queues on the server end that the provider takes at its start. That is why `files` can come after `routing_checks` in `root.cm`.

Two checks belong to the root and not to the decoder or `route()`: a declared path that is empty, is only `/`, or does not start with `/` is `InvArgs` on S, with a line naming the consumer's use, the provider's declaration and the path (shown through `printable_path()`, since it is manifest bytes). The compiler never writes one, and the decoder is structural by design, so this is the root's refusal as `serve_svc()`'s is for a protocol use that is not under `/svc`. A joined path longer than the buffer is the same, though `route()` has checked it: the buffer is fixed and the length is cheap to check again.

**Change**
```cpp
// before, Realm::run()
} else if (routed->provider != nullptr || routed->rights.is_none()) {
    // Under D3 a directory comes only from the built-in `boot` ...
    served = Status::NotSupported;
} else {
    served = m_boot->serve(server, routed->subdir.view(), to_operations(*routed->rights));
}

// after
} else if (routed->rights.is_none()) {
    served = Status::NotSupported;           // a directory use states rights; the decoder does not check
} else if (routed->provider != nullptr) {
    forward_directory(instance, use, *routed, server);   // consumes |server|, whatever the provider's state
} else {
    served = m_boot->serve(server, routed->subdir.view(), to_operations(*routed->rights));
}
```

```cpp
// forward_directory(), the part that builds the Open
auto const declared = routed.path.substr(1);          // export/docs
...                                                   // + "/" + subdir when there is one
write_open(provider, routed.name,
           to_flags(*routed.rights) | ours_io::Flags::ProtocolDirectory,
           {path_bytes.data(), length}, server);
```

Rejected: keeping `forward()` whole and writing `forward_directory()` beside it as a copy, which is shorter to write and leaves the exited-provider rule in two places; and a state per directory entry in the root (a Loop slot on the entry's first message, Fuchsia's way), which is decision 5 (b) of the design, kept as the answer if the 64-message bound of a provider's outgoing queue or the window below ever matters.

**Reference**
The design's `.agents/tasks/directory-capability/DESIGN.md`, "The Open on its way" and "One Open crossing mount points", decision 5 (a). The path join is the one the design cites for Fuchsia: `DirConnectorOutgoingRouter` joins `subdir` to `source_path` (`src/sys/component_manager/src/model/component/instance.rs:1682`). Fuchsia connects a provider on the entry's first message (`serve_processargs/src/namespace.rs:145-175`); ours does it when the consumer starts (D8 a), which is the departure the maintainer accepted.

**Verification**
- Build, `boot_check.py --out out/dc-step4` on both images: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required; host tests 151 passed. The same counts as before the change. The cases are in `routing_checks`, which prints `routing_checks: passed` only on rel, so what is new is that line meaning more: 26 more cases.
- The rel log shows the whole life of the new route: `init: started files` after `init: started echo`, and, from the provider's console, `ioservice: the outgoing directory refused an Open of "export/ghost": NotFound` and `... of "export/docs/hello.txt": NotDir`, the two Opens the root forwarded that the provider's server refuses.
- The root has no host test (`system/service/init/test/` holds `autorun_test.cpp`), so `forward_directory()` is checked on the target, by mutation. Each mutation was applied to the source, the rel image built and booted, and the source restored; all are caught by `routing_checks`, whose own mutations are in `routing-checks.md`. The counts are `boot_check.py`'s FAILED lines, the closing `n check(s) FAILED` line among them:
  - the declared path's leading `/` kept: 19, `BadPath` on every directory;
  - the joined subdir dropped: 9, `/files-sub` lists the whole tree;
  - the subdir joined with another separator than `/`: 5, `NotFound` for `/files-sub`;
  - `ProtocolDirectory` dropped: 3, and only the two cases of a path that ends at a file notice it (a directory behaves the same with and without the bit);
  - the rights dropped: 28, `AccessDenied`; every right instead of the use's, and the use's plus `ModifyDirectory`: 2 each, the one case that asks `/files` for `MODIFY_DIRECTORY`, since a read-only `VmoFile` refuses write and execute whatever the connection holds;
  - the old branch put back (`NotSupported`): 19.
- The `InvArgs` path is reached by no manifest the compiler accepts, so it was checked by inverting its condition: every directory route to a provider then logs `init: routing_checks's use of "files" cannot be forwarded: files's declaration of "docs" states the path "/export/docs", which is not absolute below /` and closes the entry with `InvArgs` (19). The condition was restored.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step4`) on `realm.cpp`: no diagnostic. `format_cpp.py --check` clean.
- Not measured: the exited-provider row and the failed-to-start row of the design's table. Nothing in the dev image makes a provider exit (that is step 5's `stop` node) or fail to start, and the window of the fifth row cannot be made on purpose. Both go through `provider_serves()`, which `forward()` has used for a protocol since 2026-10-30 and `routing_checks` exercises for echo.

**Deliberately not done**
- A state per entry, a timeout, or a retry in the root. The write is the whole of the root's part.
- A change to `log_route_error()`. A directory denied at the declaration reads `routes nowhere: AccessDenied at files (declaration of "docs")`, which is true and does not say the provider allows less than the use asked for; step 1 noted it. The warning of the build says it in full.
- A host test for the root. It would need the Loop, the fake kernel and a Realm; the target run covers each branch that can be reached.
- `Realm::connect()` is untouched: a directory is routed once, at the consumer's start, and never per Open.

**How it was found**
None. A new feature, from the design. The one thing the design had not seen is in the entry for `routing_checks` (the first Open on an entry that the provider later refuses reads `PeerClosed`).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The root writes the Open and keeps nothing. What would it have had to keep if it had answered the consumer itself, and why would that make a filesystem that waits on a disk unusable?
2. Drop `ProtocolDirectory` from the flags and the five uses that work still work. Which route would notice, what does the provider answer with the bit, and what did the consumer get without it?
3. A declared path is `/export/docs` and the joined subdir is `sub`. Which server consumes which part of `export/docs/sub`, and why did the root join them instead of sending two Opens?
