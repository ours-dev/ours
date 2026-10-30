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
