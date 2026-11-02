# acpi changes

## `TableSet::dsdt()` finds the DSDT through the FADT, and `export_tables()` lays the tables out for user space (2026-10-31)

**Files**
- `system/public/ours/acpi_tables.hpp` — new, the ABI: a directory (`AcpiDirectory`, then one `AcpiDirectoryEntry` per table), then each table's bytes at an offset that is a multiple of 8. Listed in `system/public/public_headers.gni`.
- `kernel/lib/acpi/include/acpi/structures/fadt.hpp` — new, the FADT as far as `x_dsdt` (148 bytes), with the offsets of the fields read pinned by `static_assert`.
- `kernel/lib/acpi/include/acpi/table_set.hpp`, `kernel/lib/acpi/src/table_set.cpp` — `TableSet::dsdt()`.
- `kernel/lib/acpi/include/acpi/table_export.hpp`, `kernel/lib/acpi/src/table_export.cpp` — new, `plan_export()` and `export_tables()`.
- `kernel/lib/acpi/BUILD.gn` — the new source and headers, and the host test `table-export-test`.
- `kernel/lib/acpi/test/acpi_test_blob.hpp` — the test machine can hold a table the root table does not list (`add_unlisted_table()`, `unlisted_phys()`, `unlisted_bytes()`).
- `kernel/lib/acpi/test/table_set_test.cpp`, `kernel/lib/acpi/test/table_export_test.cpp` — the cases below.

**Symptom**
Latent, cannot be triggered today: nothing asks for the DSDT, and nothing hands the tables to user space. The route service of `.agents/tasks/interrupt-system/PLAN.md` step 8 needs both: its AML interpreter starts from the DSDT and the SSDTs, and `docs/repairs/261031/syscall.md` item 1 (`Isa` and `kind` leaving `interrupt_map`) waits for it.

**Cause**
Not a mistake: a missing input, with one trap in it. `TableSet::find<T>()` walks the root table (XSDT, or RSDT), and the DSDT is not in it. ACPI lists the FADT there, and the FADT carries the DSDT's address twice: `dsdt`, 32 bits, and from revision 2 on `x_dsdt`, 64 bits. A `find<>` for "DSDT" would answer `nullptr` on every real machine, and the AML would be out of reach with nothing saying why.

Three details decide how `dsdt()` reads the FADT.

- **Which pointer.** `x_dsdt` when the table is long enough to have one and it is not zero, otherwise `dsdt`. A revision 1 FADT is 116 bytes: `x_dsdt` is not in it, and the bytes after the table are some other table's. So the length is checked against the field before the field is read, which is what `find_checked()` does for every typed table.
- **Which is right when they differ.** A firmware may write both and disagree. The 64 bit one wins, as the ACPI specification and ACPICA do.
- **What the pointer leads to.** Its signature must be "DSDT" and its checksum must hold, as for any table; a FADT that points at an SSDT, or at a table whose checksum is off, gives no DSDT.

The directory in the VMO is the answer to a second question: why not copy the XSDT and the tables behind it, which user space could then walk as the kernel does. Every pointer in such a copy is a firmware physical address that means nothing in another address space, and rewriting them would change the bytes the checksums cover. So the tables are copied untouched, and the directory is the only index.

Two properties of the copy are there for safety, not tidiness. The padding between tables is zero, because the buffer is kernel heap and anything left in it would reach a user program. And a table whose checksum does not hold is left out, so that a reader never has to decide what to do with a table the firmware itself got wrong.

**Change**
The pointer choice:

```cpp
auto phys_addr = PhysAddr(fadt->dsdt);
if (fadt->header.length >= sizeof(Fadt) && fadt->x_dsdt != 0) {
    phys_addr = PhysAddr(fadt->x_dsdt);
}
```

The layout `export_tables()` writes, the DSDT first and then the root table's order:

```text
[AcpiDirectory 16][AcpiDirectoryEntry 16 x count][pad to 8][table][pad][table]...
```

`plan_export()` and `export_tables()` walk the same tables through one function, so the size the first answers is the size the second writes. The second still checks every offset against that size before it writes, because the buffer is the caller's. Rejected: exporting only the DSDT and the SSDTs, since the MADT's interrupt source overrides and the MCFG's ECAM base are static tables the route service also needs and no AML (`.agents/tasks/driver-manager/DESIGN.md`, D3); and mapping the firmware's own pages into the route service, which needs the MmIo root, the very power that service must not hold.

**Reference**
ACPICA `acpi_tb_parse_fadt()` and `acpi_tb_select_address()` in `drivers/acpi/acpica/tbfadt.c` (Linux master, read for this entry): the first installs the DSDT from the FADT's `Xdsdt`, the second picks the 64 bit address unless it is zero. The directory is this project's own design.

**Verification**
- `table-export-test` (new, 6 cases) and `table-set-test` (4 new cases for `dsdt()`): the layout with every offset, length and the total written out by hand (480 bytes), the firmware's bytes unchanged, the padding zero and the bytes past the plan untouched; `BufferTooSmall` with nothing written; a bad checksum, a bad DSDT, a DSDT the root table lists as well, and a machine with no FADT. For `dsdt()`: `x_dsdt` over `dsdt`, `x_dsdt` of zero, a revision 1 FADT, and six ways to have none (no FADT, a zero pointer, a FADT whose declared length ends before the pointer, a pointer to an SSDT, a bad checksum, an address the mapper cannot follow).
- Seven mutations, each caught by a test and each restored byte for byte: `x_dsdt` ignored, the signature unchecked, the FADT's minimum length lowered to 1, the padding not zeroed, a root-table DSDT not skipped, no alignment, a bad checksum exported.
- `boot_check.py`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host tests 152 passed (151 before: the new executable). clang-tidy on the two changed sources: no findings.
- Not verified: nothing in the kernel calls `export_tables()` yet (the next change), and arm and riscv were not built.
- A first run of `table-set-test` aborted with `free(): invalid next size`: a test helper wrote the FADT's `dsdt` at offset 40 of a 40-byte allocation. The case is now built the way the thing it stands for looks: a full table whose header declares 40 bytes, so the pointer is in memory and not in the table.

**Deliberately not done**
The kernel does not call `export_tables()`: handing the VMO to user space is the next change. No MCFG decoder (`EcamBase` of the route service needs one, later). The FACS is not exported: it holds no AML and the firmware writes to it. No compression, since the tables on the machine this runs on are tens of kilobytes.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a lookup of "DSDT" in the root table fail on a real machine, and where is the DSDT's address?
- Why is the padding zeroed and why is the output not a copy of the XSDT with the tables behind it?
- What does a reader do with a revision 1 FADT, and what does it do when `dsdt` and `x_dsdt` differ?

## The export reads each length once and says what it left out; the physmap adapter bounds the whole range (2026-10-31)

**Files**
- `kernel/lib/acpi/src/table_export.cpp`, `include/acpi/table_export.hpp` — one read of each table's length; the write pass is held against the plan; `ExportedLayout` says how many tables were skipped and whether the DSDT is there.
- `kernel/platform/pc/acpi.cpp` — `PhysMapMapper::map()` checks the last byte of the range, not only the first.
- `kernel/lib/userabi/src/userboot.cpp` — the log line says what the layout says.
- `kernel/lib/acpi/test/table_export_test.cpp` — the counts, and a mapper that changes the tables between the plan and the write.
- `kernel/lib/acpi/include/acpi/table_set.hpp`, `structures/fadt.hpp`, `src/table_set.cpp` — two comments that were wrong.

**Symptom**
Latent, cannot be triggered on the machine tested. Found by the independent review of the ACPI work (`docs/changes/261031/acpi-route.md`).

**Cause**
Three small things in the kernel half, each of which assumed firmware memory to be well behaved.

- **The adapter checked the start of a range.** The physmap is one linear window of 32 GiB. `PhysMapMapper::map(addr, size)` asked only whether `addr` was in the window and never used `size`, although the contract of `acpi::PhysMapper` is that the whole range is readable, and the length is the firmware's: a table's header says how long it is, and `TableSet` maps that many bytes. A table at the end of the window with a length that runs past it would have been "mapped", and the checksum would have run off the end into a page fault. The reviewer reproduced it with a mapper that checked only the start. It is now one more comparison on the last byte.
- **A length was read four times.** In the write pass `header.length` is firmware memory read for the guard, the entry, the copy and the stride. If it were two values the guard would hold for one and the copy would use another. It is read once and every use takes that one.
- **The directory could claim more than it held.** The header of the directory says how many tables there are and how long it all is, and it was written before the tables. If the second walk found a table fewer than the first (a checksum that stopped holding), the directory still said N and held N-1, the last entry zero. Firmware memory is not meant to change, but the cost of being wrong is a directory that lies to a program that trusts it, so the write pass is now held against the plan and `export_tables()` answers BadState, and the buffer is not a directory, if they differ.

The log line was the fourth: it compared the exported count with `num_tables() + 1` and so said "3 of 4" for a machine with no FADT where nothing was lost. It now says how many tables, how long, whether the DSDT was found, and how many were left out.

**Change**

```cpp
auto const length = usize(header.length);            // read once
if (written == layout.count || next_table + length > layout.size) { overran = true; return; }
...
if (overran || written != layout.count || next_table != layout.size) {
    return ktl::err(Status::BadState);
}
```

and in the adapter: `size == 0 || addr + (size - 1) < addr || !is_valid_phys_addr(addr + (size - 1))` is an error.

**Reference**
None, this project's own design. The contract that the adapter did not keep is the comment on `acpi::PhysMapper` in `kernel/lib/acpi/include/acpi/table_set.hpp`.

**Verification**
- `table-export-test`: the plan counts the skipped tables and the DSDT; a mapper that changes a table's checksum, and one that makes a table longer, just before the first call of the write pass: both answer BadState, and nothing is written past the buffer. Three mutations caught (the final check removed, the skipped count removed, the DSDT flag removed), each restored.
- `boot_check.py`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host tests 154 passed; the kernel logs `7 ACPI tables, 9384 bytes, handed to userboot; DSDT found, 0 left out`.
- Not verified: the adapter's new check on a table at the end of the window: it is kernel code that no host test reaches, and the reasoning is the comparison itself. The lookup cache of `TableSet` is still not thread safe; the comment now names the second caller, which is one thread and before user space.

**Deliberately not done**
The startup-handle arrays of the kernel's and the user-mode userboot are exactly full (14 and 13), with nothing that stops the next handle from writing past them: `docs/repairs/261031/acpi-route.md`, item 2. The `ExportedLayout` still counts a DSDT the root table lists as ignored, not skipped.

**How it was found**
By the independent review, and reproduced under ASan and UBSan with the repository's own `table_set.cpp` and `table_export.cpp` and a mapper that checked only the start.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a contract that says "the whole range is readable" need the adapter to look at the size, when the physmap is a permanent linear mapping?
- What can a program that trusts the directory do with a header that says five tables and holds four?
- Why is a length read once better than a length read four times when nothing writes the tables?
