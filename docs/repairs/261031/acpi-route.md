# acpi-route outstanding repairs

## 1. No automated check compares the kernel's ISA-to-GSI translation with the firmware's tables

**Severity**
Medium: right on the machine the images run on, and nothing would say so on a machine where it is not. The translation feeds the timer's line and the kernel's own tests, so a wrong answer there is a timer that never ticks.

**Symptom**
Latent, cannot be triggered on QEMU q35 with edk2, the only machine tested. On hardware whose MADT has overrides the kernel reads differently from the route service (a rule that diverges, as the duplicate-override rule once did), the kernel would put the timer on one GSI and the route service would send a driver to another, and no test fails.

**Cause**
`interrupt_map(Isa)` was an independent oracle: a program could ask the kernel for the ISA line and compare it with its own reading of the MADT. Since the change "interrupt_map names the line by the platform's number" (`docs/changes/261031/syscall.md`) that call is gone, and the two readings of the MADT (the kernel's `apic_io_isa_to_global()` and the route service's `isa_irq_to_gsi()`) are never put side by side. `pinlines.test` (`kernel/commands/pin_lines_test.cpp`) compares `platform::isa_irq_line()` with the kernel's own table, not with the MADT, and is a kernel-shell command that a person runs, which `boot_check.py` does not. `routing_checks` checks the route service's answers against QEMU's table written out by hand, which holds for QEMU only.

**Change**
A host test in `kernel/lib/acpi/test` or `system/service/acpi-route/test` that feeds one MADT blob, with overrides, to both readings and compares them for every ISA number 0 to 15. That needs the kernel's translation to be callable on a `TableSet` and not only on the platform's global state; the smaller change is to move the override lookup both sides do into one function of `//kernel/lib/acpi` and have the route service call it. Ruled out: a second oracle syscall, which is what removing `Isa` was for.

**Reference**
None, this project's own design.

**Acceptance**
The new test fails when either side's duplicate-override rule, polarity decoding or trigger decoding is changed alone (mutate each, one at a time), and passes with the tree as it is.

## 2. The startup-handle arrays are exactly full, with nothing that stops the next handle from writing past them

**Severity**
Medium: not reached today, because every array is as large as the most the code writes; the next handle added writes past the end of a stack array in the kernel, or in the program that starts the first process.

**Symptom**
Latent, cannot be triggered today. The kernel's `write_bootstrap()` (`kernel/lib/userabi/src/userboot.cpp`) holds `handles[14]` and `infos[14]`, and writes at most 14 (the root VMA, the process, the root job, the VMEX resource, the vDSO, the debug, sampling and IOMMU resources, the IRQ, I/O port and MMIO resources, the ACPI tables, the crash record, and the boot image; fourteen with every optional one present). The user-mode userboot (`kernel/lib/userabi/userboot/userboot.cpp`) holds `handles[13]` and `infos[13]` for what the kernel hands it. A fifteenth handle in the first, or a fourteenth in the second, is a store past the array.

**Cause**
Each `handles[count] = ...; infos[count++] = ...` pair has no bound of its own. The arrays were sized by counting the pairs, which is right on the day it is done and wrong on the next one. The ACPI handle that took the kernel's array from 13 to 14 and the user-mode one from 12 to 13 is how this was seen: the count was done by hand each time.

**Change**
Put the bound in one place: a small helper that owns the arrays and the count and whose `add()` answers `NoResource` when full (the kernel side), and the same check in the lambda `add` of the user-mode side. Ruled out: sizing the arrays larger, which only moves the day on which they are full.

**Reference**
None, this project's own design.

**Acceptance**
A host test of the helper that fills it and checks the next `add()` is refused; and, in the kernel, a handle count at the limit that still boots with both images.

## 3. `EcamBase` answers the first window of a segment, and a segment may have several

**Severity**
Medium: right on QEMU, whose MCFG has one allocation. On a machine whose MCFG splits a segment across several allocations, the second window's buses are unreachable through the service.

**Symptom**
Latent on the machine tested. The reply holds one window, and `ecam_window()` (`system/service/acpi-route/src/routes.cpp`) returns at the first allocation whose `segment_group` matches. Buses outside that allocation's `start_bus` to `end_bus` get no answer.

**Cause**
The protocol's shape: `EcamBase(segment)` has a single `EcamWindow` in its reply (`system/lib/ours.acpi`). The MCFG allows several allocations for one segment group, each with its own bus range, and the request has no bus to pick one by.

**Change**
Either add the bus to the request (`EcamBase(segment, bus)`, and the reply is the window that holds it, with `NotFound` for a bus none holds), or return every window of the segment (a variable-length reply, which the flat codec does not have yet). The first is the smaller change and answers what a PCI driver asks, which is where one function's configuration space is. Whoever takes this changes the codec, its test and the client in one commit.

**Reference**
None, this project's own design. The MCFG layout is ACPI-adjacent (PCI Firmware Specification, "MCFG table"); the bus range of an allocation is `start_bus` to `end_bus`.

**Acceptance**
A route test with an MCFG of two allocations in one segment, asking for a bus in each, and for one in neither.

## 4. The client has no timeout, and four stuck clients lock the service

**Severity**
Low: only a program the system started can reach the service, and each of them is trusted not to hang. It is a fault only if a client does.

**Symptom**
Latent, cannot be triggered by the programs that exist. `ours_acpi::isa_irq()` and `ecam_base()` write a request and wait for the reply with no deadline. The server holds `MAX_CLIENTS = 4` client slots (`system/service/acpi-route/src/acpi_route.cpp`) and refuses a fifth with `NoResource`; four clients that connect and never close their channel keep every later one out.

**Cause**
The service is small on purpose: a fixed table, no heap. A slot is freed when the client's channel closes, and a client that never closes it keeps the slot for good. The client's wait is `object_wait_one`, which takes a deadline, and the call passes `INSTANT_INFINITE`.

**Change**
A deadline on the client's wait, with `TimeOut` as the call's error; and on the server, either a larger `MAX_CLIENTS` or the closing of the oldest idle client when the table is full. The first is the smaller change and matters first, since a driver that hangs at start is worse than one that fails. Whoever takes it should decide the deadline with the Driver Manager's design (`.agents/tasks/driver-manager/DESIGN.md`), which is meant to be the only client that connects once it exists.

**Reference**
None, this project's own design.

**Acceptance**
A client test against a server that never answers: the call returns `TimeOut` within the deadline.

## 5. The route service's request paths are covered by the codec's tests and by reading, not by a test of the server

**Severity**
Low: a missing test. The paths are short and read straight, but a change to one is not caught.

**Symptom**
Latent, cannot be triggered by the programs that exist. Not covered by any test: a message of the wrong size, an ordinal the protocol does not know, a message that brings a handle, a table of clients that is full, and a wait that cannot be armed again (`rearm_failed()`). `routing_checks` exercises the two methods with good requests only.

**Cause**
The server's class lives in the program's file (`system/service/acpi-route/src/acpi_route.cpp`) and calls the kernel's syscalls directly, so a host test cannot link it. The route logic and the table reader are in their own files and are tested; the loop around them is not.

**Change**
Move the server's class to a library file that takes the channel calls as a parameter (a template or a small interface), and run it on the fake kernel the `ioservice` host tests already use. Ruled out: a kernel-side test of the service, which cannot make the failures above on demand.

**Reference**
None, this project's own design.

**Acceptance**
A host test that sends each of the five cases and sees the connection closed (or `NoResource` for the full table), and that does so with the service still answering a good request afterwards.
