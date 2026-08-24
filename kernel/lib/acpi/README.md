# libacpi

A small ACPI table decoding library.  Table layouts follow ACPI 6.5.

## Shape

```
acpi/signature.hpp        Signature (POD, little-endian u32)
acpi/structures/*.hpp     Table and entry layouts.  Plain data, no logic.
acpi/table_set.hpp        PhysMapper (the one seam) + TableSet + entries<E>()
acpi/numa.hpp             SRAT/SLIT decoding
acpi/debug.hpp            DBG2 decoding
```

`TableSet` is the way in.  Build one from the RSDP, then:

```cpp
auto tables = acpi::TableSet::from_rsdp(mapper, rsdp_phys);
auto const *madt = tables->find<acpi::madt::Table>();
for (auto const &entry : acpi::entries<acpi::madt::IoApicEntry>(*madt)) {
    ...
}
```

`find<T>()` is the only supported way to reach a typed table.  It checks the
signature, that the table is at least `sizeof(T)` bytes, and the checksum -- in
one place, so a caller cannot forget the length check and read past a truncated
table.  Lookups are memoised per signature; the root table is walked at most
once per signature rather than once per call.

`entries<Entry>(table)` is a range over the sub-entries carrying `Entry`'s type
tag.  It stops at the first malformed entry and reports that through
`EntryRange::malformed()`.

## Constraints

- `PhysMapper` implementations must return mappings that stay valid for the
  lifetime of the program and cover the whole requested length.
- `TableSet` is not thread safe: `find()` and `header_at()` mutate the lookup
  cache.  Callers run during single threaded init.

## Not covered

MADT types 0x0B..0x0F (the GIC family) have type numbers but no entry structs.
The previous definitions for them were fabricated -- the GIC CPU interface
structure is 80 bytes in ACPI 6.4 and 82 in 6.5, and the declaration claimed 10
-- and nothing in this tree consumes them.  Add them against the specification
when ARM ACPI bring-up needs them.

## Tests

`//kernel/lib/acpi:tests` runs on the host toolchain against synthesised table
images (`test/acpi_test_blob.hpp`).  The fixture's mapper is a *translating*
mapper with per-table bounds, so an over-read in the library shows up as a
mapping failure rather than a silently successful read of adjacent memory.
