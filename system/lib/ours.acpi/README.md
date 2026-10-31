# ours.acpi

A hand-written codec for ours.acpi, the protocol of the ACPI route service: `Route`, a closed protocol of two two-way methods, `IsaIrq` (the GSI an ISA interrupt arrives on, and how it signals) and `EcamBase` (where one PCI segment's configuration space is). The service answers from the firmware's static tables, so that no driver has to know a GSI or an ECAM base (`system/service/acpi-route`, `.agents/tasks/driver-manager/DESIGN.md`). Bodies are flat at fixed offsets; the layout is in `include/ours_acpi/ours_acpi.hpp`, and the ordinals are the hashes of the selectors, which `system/lib/wire/test/ordinals_test.py` recomputes.

The directory is the library's name. The include prefix and the namespace are `ours_acpi`, the library's name with its dot made an underscore, since a C++ name cannot hold a dot.

## Layout

```text
include/ours_acpi/   public headers, reached as <ours_acpi/...>
src/                 sources
test/                host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ours.acpi`; its headers alone are `//system/lib/ours.acpi:headers`. The gtest executable is `//system/lib/ours.acpi/test:ours_acpi`, built on the host by the default build. It encodes each message, decodes it back, and breaks it one field at a time.
