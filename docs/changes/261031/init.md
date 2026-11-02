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

## The root offers ours.boot.Acpi, the machine's ACPI tables as a read-only VMO (2026-10-31)

**Files**
- `kernel/lib/userabi/src/userboot.cpp` — `make_acpi_tables_handle()` lays the tables out with `acpi::export_tables()`, copies them into a paged VMO and makes a handle without Write; `write_bootstrap()` sends it as `HandleType::VmoAcpi` when the platform read ACPI tables. The array of handles grows from 13 to 14.
- `kernel/lib/userabi/BUILD.gn` — the dependencies on `//kernel/lib/acpi` and the platform headers.
- `kernel/platform/arm/acpi.cpp`, `kernel/platform/riscv/acpi.cpp` and their `BUILD.gn` — new, define `ACPI_TABLES` as `nullptr`: neither platform reads ACPI yet, and the code above is not per platform.
- `system/public/ours/process_args.hpp` — `HandleType::VmoAcpi = 0xe1`.
- `kernel/lib/userabi/userboot/userboot.cpp` — the user-mode userboot passes the handle on to the root, as it does the roots.
- `system/lib/ours.resource/include/ours_resource/ours_resource.hpp` — `ACPI_PROTOCOL`, `ours.boot.Acpi`, one `Get` answered with a handle.
- `system/service/init/init.cpp`, `realm.cpp`, `realm.hpp`, `pools.hpp`, `builtins.json` — the root takes the handle, serves it with a `ResourceServer`, and counts one more server.
- `system/lib/component/include/component/route.hpp`, `system/lib/wire/test/ordinals_test.py` — the list of built-ins in a comment, and the new selector in the ordinal test.
- `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn` — `root.cm` offers the protocol to routing_checks alone, which uses it.
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_acpi_tables()`.

**Symptom**
Latent, cannot be triggered today: no program could read the ACPI tables. The route service of `.agents/tasks/interrupt-system/PLAN.md` step 8 starts from them, and removing `Isa` and `kind` from `interrupt_map` (`docs/repairs/261031/syscall.md`) waits for that service.

**Cause**
Not a mistake: a missing input, and the way to it is decided by who must not hold what. A program that reads AML parses bytes the firmware wrote, so it must hold no power over hardware. Mapping the firmware's own pages into it would need the MmIo root, which is every device register in the machine. So the kernel copies the tables, and the copy is what the program gets.

Three choices decide the shape.

- **A copy, made late.** The kernel reads the tables in place at `PlatformInitLevel::StartUp`, before the page allocator exists, and a VMO needs the allocator. So the copy is made when the first process's startup handles are made, in `write_bootstrap()`, by which time the kernel's `TableSet` has been kept for the whole boot (`ACPI_TABLES`, whose mapper never unmaps).
- **Read only, and by rights, not by promise.** The handle is made without Write, and every duplicate the root hands out is a `SameRights` duplicate of it, so no holder can write the tables, and the next holder sees what the first saw. The check asks the kernel for the handle's rights and tries a write.
- **A protocol, not a resource.** The root serves it with the same `ResourceServer` as the roots, which answers each `Get` with a duplicate of the handle it holds. The class name says "resource" and the VMO is not one; renaming it was left out, since the class does nothing a resource needs.

**Change**
The kernel side, before and after in `write_bootstrap()`: nothing between the Mmio root and the crash record, and now

```cpp
if (auto acpi_tables = make_acpi_tables_handle()) {
    handles[count] = ustl::move(acpi_tables);
    infos[count++] = handle_info(HandleType::VmoAcpi, 0);
}
```

Failure is soft on purpose: a platform without ACPI tables, or a firmware that lists none that can be used, gives no handle and a log line, and the boot goes on; the root then answers a `Get` of `ours.boot.Acpi` with why it cannot. The ordinal of `ours.boot/Acpi.Get` is the first 8 bytes of its SHA-256, as the other methods' are, and `ordinals_test.py` recomputes it.

**Reference**
Zircon's boot item `ZBI_TYPE_ACPI_RSDP` carries only the RSDP's physical address (`docs/repairs/261003/x86-phys.md` cites it), and, from memory and not checked here, its user-space ACPI code maps the tables itself through the MMIO resource. This project hands over a copy instead, so that the reader needs no root (`.agents/tasks/driver-manager/DESIGN.md`, the three roles). The handoff of a read-only VMO follows this project's own crash record (`HandleType::VmoCrashlog`).

**Verification**
- `boot_check.py` on the final sources: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required, `routing_checks: passed`), host tests 152 passed. The kernel says `userabi: 7 of 7 ACPI tables, 9384 bytes, handed to userboot` on both, and the root `init: holding the ACPI tables`.
- Four mutations, each caught by `routing_checks` in the rel image and each restored byte for byte, then the final run above: the kernel handing no tables (2 FAILED lines), the VMO made writable (4), the user-mode userboot not passing the handle on (2), the root binding `ours.boot.Acpi` to the Mmio server (2).
- What the first run of those mutations taught: against the dbg image all four passed. The dbg boot log has no user-space line at all (no `init:` line, no `routing_checks: passed`), and `boot_check.py` requires `routing_checks: passed` of rel only, so routing_checks is gated by rel alone. A check added there is tested in one image, and a mutation test of it has to boot that one.
- Not verified: arm and riscv were not built, so the two stubs defining `ACPI_TABLES` are unbuilt. The real hardware's tables are not seen: only QEMU's q35 under edk2, where all 7 tables were exported.

**Deliberately not done**
No route service, and no AML interpreter: this change only gets the bytes to a program that may hold them. No MCFG decoder. `ResourceServer` keeps its name. A platform that reads no ACPI tables answers a `Get` with the root's failure instead of an empty VMO, so that a program cannot mistake "no tables" for "no tables it needs". That path is read from the code (the same as for a root the kernel did not mint) and not exercised: QEMU's machine always has tables.

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is the VMO a copy made in `write_bootstrap()` and not a window onto the firmware's pages, and what would the second choice have cost the program that reads it?
2. What makes the handle read only, and what does a duplicate of it get?
3. Four mutations passed against the dbg image. What did that say about the check, and what did it say about the way the mutations were run?
