# user-pager: the sub-steps of step 2, one commit each

**Date**: 2026-10-23
**Status**: Accepted

## Request
"继续" after step 1 of page-source: give user space the pager, Zircon's shape, READ only. Scope ends at the decisions in DESIGN.md; each sub-step is introduced to the user before it starts.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md D1 to D6 | | | | |

## Steps
Reordered after the decisions of 2026-10-23 (D3 B, D4 B, D6 B). Each step is introduced to the user before it starts; the ones added by the decisions get their own background then.

1. mm: the copy paths wait. `copy_page_by_page()` leaves the locked block on ShouldWait, waits on the request with no lock held, and retries from `done`; covers `vmo_read`, `vmo_write` and `process_read_memory`. First, because once `pager_create_vmo` exists every program can reach it. Verified with step 1's TestProvider in `pagesource.test`. Closes the copy half of `docs/repairs/261023/mm.md` item 1 (nothing in user space can commit a VMO).
2. mm: the timed-out page request no longer dumps the address space without its lock, which D3 makes reachable.
3. mm: zero markers in VmPageMap (D4): a slot can say "zero" without a page, and the cursor, the lookup through parents and the page counts read it.
4. mm: `take_pages()` as Zircon has it, gaps and COW nodes included; unmap through the node's attachment walk (slices included), stop the kernel if an unmap fails. In the same commit, the fault path looks a present page up again under the VMO lock before widening it.
5. mm: `VmObject::detach_source()` and the provider's owner id.
6. object: port packets owned by a kernel object (`free()`, `queue()`, `cancel_queued()` guarded after zero handles); `port.test` cases.
7. ABI: `PacketType::PageRequest` and its payload in `system/public/ours/port.hpp`.
8. task and object: `thread_start` and what a program needs to wait for a thread to end (D6).
9. object: `PagerDispatcher` and `PagerProxy` with Zircon's overtime wait (D3), `VmoDispatcher::pager_koid()`; kshell `pager.test`. The lock order revision goes into `docs/design/260923/userspace-interface.md` 4.8 in this commit.
10. syscalls: `pager_create`, `pager_create_vmo`, `pager_supply_pages`, `pager_detach_vmo`; shell `check_pager` with a second thread that faults while the first serves it, on both images.

## Files
- `kernel/mm/vm_object_paged.cpp`, `kernel/mm/vm_aspace.cpp`, `kernel/mm/vm_page_map.*`, `kernel/mm/vm_cow_pages.cpp`, `kernel/mm/vm_mapping.cpp` and their headers — steps 1 to 5
- `kernel/object/port_dispatcher.cpp` and header — step 6
- `system/public/ours/port.hpp` — step 7
- `kernel/object/pager_dispatcher.cpp` (new), `kernel/object/pager_proxy.cpp` (new) and headers, `kernel/object/vmo_dispatcher.*` — step 9
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/pager.cpp` (new), `system/app/shell/shell_tests.cpp` — step 10
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/task.cpp`, `kernel/object/thread_dispatcher.*` — step 8
- `kernel/commands/page_source_test.cpp`, `kernel/commands/port_test.cpp`, `kernel/commands/pager_test.cpp` (new) — tests

## Test plan
As DESIGN.md, Testing and verification. Each step: `ours-verify` passes, and the kshell tests it touches print `<name>: ok`.
