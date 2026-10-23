# user-pager: the sub-steps of step 2, one commit each

**Date**: 2026-10-23
**Status**: Proposed

## Request
"继续" after step 1 of page-source: give user space the pager, Zircon's shape, READ only. Scope ends at the decisions in DESIGN.md; each sub-step is introduced to the user before it starts.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md D1 to D6 | | | | |

## Steps
1. mm: the copy paths wait. `copy_page_by_page()` leaves the locked block on ShouldWait, waits on the request with no lock held, and retries from `done`; covers `vmo_read`, `vmo_write` and `process_read_memory`. First, because once `pager_create_vmo` exists every program can reach it. Verified with step 1's TestProvider in `pagesource.test`. Closes `docs/repairs/261023/mm.md` item 1's copy half.
2. mm: `take_pages()` for a parentless, unsourced, unpinned node: commit the gaps, unmap the range through the node's attachment walk (slices included), stop the kernel if an unmap fails, move the pages to a list. In the same commit, the fault path looks a present page up again under the VMO lock before widening it.
3. mm: `VmObject::detach_source()` and the provider's owner id.
4. object: port packets owned by a kernel object (`free()`, `queue()`, `cancel_queued()` guarded after zero handles); `port.test` cases.
5. ABI: `PacketType::PageRequest` and its payload in `system/public/ours/port.hpp`.
6. object: `PagerDispatcher` and `PagerProxy`, `VmoDispatcher::pager_koid()`; kshell `pager.test`. The lock order revision goes into `docs/design/260923/userspace-interface.md` 4.8 in this commit.
7. syscalls: `pager_create`, `pager_create_vmo`, `pager_supply_pages`, `pager_detach_vmo`; shell `check_pager`.
8. test: `pager_probe` child program on dbg_1, one READ fault served end to end.

## Files
- `kernel/mm/vm_object_paged.cpp`, `kernel/mm/vm_cow_pages.cpp`, `kernel/mm/vm_mapping.cpp` and their headers — steps 1 to 3
- `kernel/object/port_dispatcher.cpp` and header — step 4
- `system/public/ours/port.hpp` — step 5
- `kernel/object/pager_dispatcher.cpp` (new), `kernel/object/pager_proxy.cpp` (new) and headers, `kernel/object/vmo_dispatcher.*` — step 6
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/pager.cpp` (new), `system/app/shell/shell_tests.cpp` — step 7
- a `pager_probe` program beside `fault_probe` — step 8
- `kernel/commands/page_source_test.cpp`, `kernel/commands/port_test.cpp`, `kernel/commands/pager_test.cpp` (new) — tests

## Test plan
As DESIGN.md, Testing and verification. Each step: `ours-verify` passes, and the kshell tests it touches print `<name>: ok`.
