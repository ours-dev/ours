# init changes

## The root offers ours.resource.Iommu, and routing_checks runs the DMA calls end to end (2026-10-31)

**Files**
- `kernel/lib/userabi/src/userboot.cpp` — `write_bootstrap()` mints the System resource of the IOMMU slot (`RSRC_SYSTEM_IOMMU_BASE`, 8) after the Sampling one and sends it as `HandleType::SystemResource`; the handle and info arrays grow from 12 to 13.
- `kernel/lib/userabi/userboot/userboot.cpp` — the user-mode userboot takes it, keeps it in `HandedHandles` and passes it on; its arrays grow from 11 to 12; a missing slot is a line and not a failed boot.
- `system/lib/ours.resource/include/ours_resource/ours_resource.hpp`, `test/ours_resource_test.cpp`, `README.md` — the protocol `IOMMU_PROTOCOL`, `ours.resource.Iommu`, selector `ours.resource/Iommu.Get`, ordinal `0x4b24'8818'1716'da78`.
- `system/lib/wire/test/ordinals_test.py` — the scan must find `ours.resource/Iommu.Get`.
- `system/service/init/builtins.json`, `init.cpp`, `realm.hpp`, `realm.cpp`, `pools.hpp` — the built-in, a ResourceServer for it over the root's IOMMU resource (the seventh), its field in `BuiltinServers`, the branch of `connect()` that binds it.
- `system/lib/component/include/component/route.hpp` — the comment that lists the root's built-ins.
- `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn` — root.cm offers the protocol to routing_checks alone, and routing_checks uses it.
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_iommu_resource()` and what it runs.
- `.agents/tasks/driver-capabilities/PLAN.md` — step 6 done, and what its check covers.

**Symptom**
None; this is step 6 of `.agents/tasks/driver-capabilities/PLAN.md`. Before it the five DMA calls existed (step 5b) but nothing could make the first: no process held the IOMMU slot, so the only answers a program could get were the refusals that need no slot, and the success paths, and every refusal that needs a real BTI, had never run.

**Cause**
The path is the one the MmIo root took in step 2, with one difference: a System slot is not a ranged root. The kernel mints System slots with `make_system_resource_handle(base, name)`, a resource of kind System, size 1, at the slot's base, and the receiver asks for it by `HandleType::SystemResource` with the slot as the info's argument, as VMEX, Debug and Sampling are. The events, from boot to a component holding it:
1. `write_bootstrap()` mints the slot, always (nothing is pinned until someone holding it asks), and puts it in the first process's processargs.
2. The user-mode userboot takes it with `take_startup_handle(SystemResource, 8)` and `start_next()` adds it to the next program's processargs, the root's.
3. The root takes it, keeps it, and serves it through a ResourceServer as `ours.resource.Iommu`.
4. root.cm offers the protocol from `parent` to `#routing_checks`; its manifest uses it; at `/svc/ours.resource.Iommu` its `Get` answers a duplicate.

Routing is the only gate, as for every ours.resource protocol: the root answers every Get that reaches it without asking who sent it, so a component holds the slot only if an offer leads to it. Only routing_checks is offered it, because holding it is being trusted to point a device at memory, and no driver of this image does DMA yet. The ordinal is the first 8 bytes of SHA-256 of the selector, little-endian, top bit cleared (`ordinals_test.py` recomputes it).

**Change**
`write_bootstrap()`:

```cpp
handles[count] = make_system_resource_handle(RSRC_SYSTEM_IOMMU_BASE, "iommu");
if (!handles[count]) { return Status::NoResource; }
infos[count++] = handle_info(HandleType::SystemResource, u16(RSRC_SYSTEM_IOMMU_BASE));
```

routing_checks, with the resource `Iommu.Get` gave:
- `object_get_info` says it is the System resource at base 8, size 1.
- `iommu_create` refuses a type nothing implements (NotSupported), a descriptor the stub has none of and one past the limit (InvArgs), and the System resource of another slot (OutOfRange, with the Debug one); none writes a handle.
- `bti_create` refuses an option nothing defines (InvArgs), then makes a BTI.
- `bti_pin` refuses each bad option, an unaligned offset, an empty size and one of part of a page, an address count one too few and one too many, a range past the end and one at the end, and `Contiguous` over more pages than a pin may be (a count of 1 bounds nothing); then the rights: PermWrite over a VMO that cannot be written, a VMO without Map, a BTI without Map, and the order (the BTI before the VMO). None leaves a page pinned, which is read through a snapshot of the VMO, refused while any page is pinned.
- A pin of four pages gives four distinct whole pages, the same again for a second pin, the same under Compress, one address under `Contiguous` that is the first page's exactly when the four are one run (BadState otherwise), and `pmt_unpin` ends each, once.
- A pin whose last handle is closed with no unpin is quarantined: its page stays pinned, the BTI answers BadState, `bti_release_quarantine` without Write is AccessDenied and changes nothing, and with it lets the page go and the BTI pins again.

**Reference**
The path of step 2 (`docs/changes/261030/init.md`), and for the System slot the VMEX, Debug and Sampling ones in the same functions.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required, `routing_checks: passed`), host-tests PASS (151 passed). `ordinals_test.py` OK. The rel log has the kernel's warning from the quarantine check (`bti ... id 0x1234 ... closed without an unpin`), which is that check working and not a fault.
- Mutations, each undone afterwards, each made `routing_checks` fail: Iommu bound to the Mmio server (2 failed lines); no cap on the size of a pin (2); `bti_release_quarantine` asking for no right (3); PermWrite needing no Write on the VMO (6); the address count not checked (6); the last handle of a pin unpinning instead of quarantining (4).
- clang-tidy on the files changed: no diagnostic on them. clang-format: clean. arm and riscv were not built.
- Not verified: a process that dies with a pin outstanding. The check closes the pin's handle in the process; process death reaches the same `on_zero_handles()` through the handle table's `clean()`, which is an argument and not a test. And a pin with a page source, which no program can make yet.

**Deliberately not done**
A component that is a driver: the first real user of the slot is the block driver, and it is offered the slot when it exists. `vmo_create_contiguous` (step 7).

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is the IOMMU slot offered to routing_checks alone, and what keeps another component from holding it?
2. `bti_pin` is refused with a count of 1 and a huge size under `Contiguous`. Which check says so, and why does the count not?
3. The check closes a pin's handle with no unpin and then asks whether the page is pinned. How does it ask, and why does that work?
