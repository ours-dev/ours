# mm outstanding repairs

## 1. NUMA33 fails the first vmalloc mapping during its boot self-test

**Severity**
Medium. Both development images finish booting, but the generated 33-domain
machine reports a failed one-page vmalloc self-check.

**Symptom**
With `mm_numa=true` and `NUMA=33`, both images print `vmalloc: FAILED one-page
allocation` and `vmalloc: self test failed 1 check(s)`. Mapping one page at
`0xffffff9000000000` fails. The default and designed four-node machines pass.

**Cause**
The failure site is confirmed, but the underlying allocation failure remains
unresolved. LLDB stopped in `MappingCoalescer<32>::commit()` at
`kernel/mm/vm_mapping.cpp:97`: `map_bulk()` returned `Status::NoMemory`,
`nr_mapped` was zero, and the data frame in `m_pa[0]` was `0x80e000`. The stack
was `VmMapping::map_paged` -> `VmMapping::map` -> `VmArea::map_with_vmo` ->
`heap::vmalloc` -> `heap::vmalloc_self_test` -> `init_vmm`.

This is reproducible before the PMM naming refactor. A source snapshot of
`f7b22de4`, with only the same explicit `PhysAddr{region.base}` conversion in
SRAT needed to compile NUMA, fails both images at the same address and
self-check. The naming changes and count-helper fix are absent from that
snapshot. Logs are in `/tmp/pmm-naming-checks/parent-numa33-verify.log` and
`numa33-verify.log`; debugger observations are recorded in the task result.

**Change**
Trace the page-table allocation that produces NoMemory on the first mapping,
then fix that path and add coverage for its actual failure condition. The
data-frame allocation already succeeded. Investigate the physical-zero
failure-sentinel contract as a candidate, without treating it as the confirmed
cause. Preserve the mapping error and the boot self-check.

**Reference**
None, this project's own vmalloc boot self-test and page-table allocation
protocol. See `kernel/lib/heap/src/vmalloc.cpp`, `kernel/mm/pmm.cpp`, and
`kernel/arch/x86/lib/page_tables/include/page_tables/page_tables.hpp`.

**Acceptance**
Build with `python3 tools/set.py bringup.x86 --out out/numa --args
'mm_numa=true'`. Run the repository boot checker for both `NUMA=1` and
`NUMA=33`, with the test disk where required. Both dbg and rel must pass with
no failed vmalloc self-check or mapping error, and all host tests must pass.
