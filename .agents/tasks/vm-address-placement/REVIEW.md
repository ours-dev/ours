# vm-address-placement: review of strong addresses and direct virtual placement

**Date**: 2026-11-03

## Scope and method

Self-review of both increments: the address-domain migration (d52954d0) and
removing VmPlacement from creation interfaces and all production callers.
Read modern-cpp-development, kernel-design, the affected address and paging
interfaces, and the local Zircon vm_address_region.cc creation paths.
The sentinel-based placement interface is the user's requested departure from
Zircon's flags-and-offset interface. Existing locking and permission checks
are retained.

Ran repository clang-format and clang-tidy on 138 C++ files in the first
increment and 26 in the second. Three initial header checks were repeated
through actual including translation units because standalone inference lacked
their include context. New naming and override diagnostics were corrected;
the second increment has no changed-line diagnostics. Existing unrelated
warnings were not suppressed.

Both development images and all 163 host tests passed. The 3 GiB QEMU
configuration also passed both boots and the host tests, exercising physical
addresses above 4 GiB. Seventeen relevant kernel suites passed, including
vmaddress.test (42 checks), usercopy, physical mappings, snapshots, large
pages, BTI, resources, page queues, page sources, pager, reclamation and
memory pressure. These findings are verified through the public interfaces.

The address-literal follow-up adds global constexpr `_va` and `_pa`
operators to types.hpp. Integer literal operators use unsigned long long as
required by C++; direct list initialization preserves the address bits without
an unchecked narrowing cast. Compile-time checks cover suffix result types,
zero, hexadecimal and decimal constants, bit 63 and the all-ones value.
Both touched C++ files passed clang-format and clang-tidy with no new
changed-line diagnostics. The follow-up build, both development boots and
all 163 host tests passed.

## What is done well

- system/public/ours/types.hpp:14 preserves distinct scoped address types;
  offsets, address differences and machine encodings retain their own types.
- kernel/mm/tests/address_smoke.cpp:30 rejects cross-domain operations and
  implicit integer conversions at compile time, and distinguishes physical
  zero from failed physmap translation.
- kernel/mm/vm_area.cpp:224 identifies special requests before alignment.
  kernel/mm/vm_aspace.cpp:116 excludes reserved values from address-space
  ranges without first forming a potentially overflowing end address.
- kernel/commands/vm_address_test.cpp:30 exercises concrete zero, automatic
  placement, overlap refusal, CanMapSpecific and all reserved input values.

## High severity

None outstanding in the changes.

## Medium severity

None outstanding in the changes.

## Minor and style

Resolved new constexpr naming and redundant virtual diagnostics. The legacy
kernel/lib/arch/test/paging_test.cpp is not part of the GN graph and already
uses the retired paging interface; it was left unchanged. Active address
source files are covered by the current compile database.

## Teaching opportunities

None; the user asked to focus on development.

## Suggested follow-up order

None required for this change.
