# user-pager: a user-space process supplies the pages of a VMO

**Date**: 2026-10-23
**Status**: Accepted

## Problem
Step 1 (`.agents/tasks/page-source/`) made a VMO whose pages come from a PageProvider, but the only provider is a kernel test. Nothing a program can call reaches it: there is no pager object, no syscall, no port packet for a page request. Following "the user-facing interface comes first", step 2 gives user space the real interface, Zircon's user pager, in its smallest form that serves READ faults end to end.

## Goals and non-goals
Goals:

- A pager object and four syscalls: `pager_create`, `pager_create_vmo`, `pager_supply_pages`, `pager_detach_vmo` (decision D1).
- A fault on a pager VMO delivers one READ packet on the pager's port, and the pager answers it by moving pages out of an ordinary VMO it filled (the aux VMO).
- Every teardown order (VMO, pager handle, port, faulting thread) ends without a hang or a leak.
- Nothing a program can reach answers ShouldWait or trips an assertion: `vmo_read`, `vmo_write` and `process_read_memory` on a pager VMO wait for their pages.

Non-goals: DIRTY and writeback, `pager_op_range` (decision D1), resizable or unbounded pager VMOs, job policy, early wake, `SwapAsyncRequest`, COW children of a pager VMO (still refused), evicting pages on detach (D5), a Populate mapping of a pager VMO (`docs/repairs/261023/mm.md` item 2).

Added by the decisions: zero markers in VmPageMap, so `take_pages` follows Zircon (D4); Zircon's overtime warnings and TimeOut for a pager wait, with the timeout dump fixed first (D3); `thread_start` and what a test needs to wait for a thread, so the end-to-end test runs in the shell on both images (D6).

## Design
Objects:

- `PagerDispatcher` (kernel/object): the object behind a pager handle. Keeps the list of its proxies under a leaf lock of its own, and a zero-handles flag. Owns the operations on its VMOs: `create_vmo`, `supply_pages`, `detach_vmo`, so the syscall layer never names an `mm::` type. Zircon: `kernel/object/pager_dispatcher.cc`.
- `PagerProxy` (kernel/object): the PageProvider of one pager VMO. Holds the port and the key, exactly one packet embedded in itself, the request that packet carries, a queue of requests waiting for the packet (`PageRequest::ProviderList`), and a reference to its PageSource. Zircon: `kernel/object/pager_proxy.cc`.
- Port packets owned by a kernel object: an owner interface with `free(packet)`, which the port calls with no lock held once it has handed the packet out or dropped it; `PortDispatcher::queue(packet)` and `cancel_queued(packet)`. Zircon: `PortAllocator`, `PortDispatcher::Queue()`, `CancelQueued()`.
- ABI: `PacketType::PageRequest = 9` and a 32-byte payload `{u16 command, u16 flags, u32 reserved, u64 offset, u64 length, u64 reserved}`, commands `Read = 0` and `Complete = 1`. Zircon: `zx_packet_page_request_t`.
- mm: zero markers in `VmPageMap` (decision D4), `VmObjectPaged::take_pages()` (today Unimplemented), `VmObject::detach_source()`, and an opaque owner id from the provider, for the check that a VMO was made by this pager.

Events, one READ round trip:

1. The pager process calls `pager_create`, `port_create`, then `pager_create_vmo(pager, options, port, key, size)`: a proxy, a PageSource over it, and a VMO over the source; the handle goes to the client.
2. A client thread faults on a page nobody supplied: the cursor calls `PageSource::get_pages()`, which sends the request to the proxy (step 1).
3. `PagerProxy::send_async_request()`: if its packet is free, it fills it in and queues it on the port; if the packet is already out, the request waits on the proxy's queue.
4. The fault drops every lock and sleeps on its request (step 1).
5. The pager's `port_wait` takes the packet. The port, with its lock released, calls `free()`; the proxy marks the packet free and sends the next queued request in it.
6. The pager writes the content into an aux VMO and calls `pager_supply_pages(pager, vmo, offset, length, aux, aux_offset)`: `take_pages()` moves the pages out of the aux VMO (a gap travels as a zero marker, as in Zircon, and the range is unmapped from every mapping of the aux node, slices included), then `supply_pages()` puts them into the pager VMO's empty slots and calls `on_pages_supplied()`. The two trees are locked one after the other, never together.
7. The client thread wakes, takes the fault again, finds the page and maps it.

Events, the end of a pager VMO:

- `pager_detach_vmo` or the VMO's death detaches the source: every outstanding request completes, the waiters' retries fail (a user fault becomes a fatal page fault), and the proxy queues a `Complete` packet as the last one.
- The pager handle closed: each proxy closes its source and withdraws a still-queued `Complete`; nothing more is delivered.
- The port closed first: the port frees its queued packets through `free()`, and later queues answer BadHandle; the proxy must treat a `Complete` it could not queue as delivered, or the proxy and the source leak (a fix over Zircon, found by reading).

Why one packet per proxy: nothing is allocated under the VMO and PageSource locks, queuing cannot fail on the per-port packet limit, and a pager that never reads its port costs the kernel nothing.

Proxy lifetime, as Zircon has it: the proxy and its source hold each other on purpose, and the cycle is cut in one of three places depending on what goes first: `on_close` when no `Complete` is pending, `free()` of the `Complete` packet, or the pager handle's `on_zero_handles`. On top of Zircon: the cycle is also cut when withdrawing a queued `Complete` succeeds and when the port is gone; the proxy drops its reference to the source only after letting go of its own lock (in ours `~PageSource` takes the source's lock); the raw pointer to the pager is cleared wherever the proxy leaves the pager's list.

Lock order (decision D2): VmAspace → VmCowPages → PageSource → PagerProxy → {PagerDispatcher list lock, port lock}, the last two leaves. The port lock is already a leaf in today's code. Lockdep does not link today, so the order rests on the written argument and the teardown tests.

`cancel_queued()` answers false once the port has lost its last handle: `on_zero_handles` moves the whole queue to a local list and frees it with the lock released, and a packet on that list still looks linked. Zircon guards the same window with a sentinel (`PortDispatcher::CancelQueued()`).

Syscall error order follows this project's rule, handles first (BadHandle > BadType > AccessDenied > InvArgs, `docs/design/260923/userspace-interface.md` 4.5), not Zircon's alignment-first order. A supply of length 0 answers Ok, as in Zircon.

Found while designing, fixed inside this step: the fault path reads a present page table entry before it takes the VMO's lock, and `widen_present_locked()`'s non-write branch remaps that frame without looking it up again (`kernel/mm/vm_mapping.cpp:645`, `:769-776`). Harmless while no frame leaves a node while mapped; `take_pages()` is the first operation that does, so the branch must look the page up again under the lock.

Reference: Zircon `kernel/object/pager_dispatcher.cc`, `pager_proxy.cc`, `kernel/lib/syscalls/pager.cc`, `vdso/pager.fidl`, `kernel/object/port_dispatcher.cc` (`Queue`, `CancelQueued`, `Dequeue`, `on_zero_handles`), `kernel/vm/vm_object_paged.cc` (`TakePages`, `SupplyPages`), `kernel/vm/vm_cow_pages.cc` (`TakePagesLocked`, `SupplyPagesLocked`, `DetachSource`), `system/public/zircon/syscalls/port.h`.

## Alternatives considered
- One owner interface for every kind of port packet: moves the two existing kinds for nothing in return.
- Pager packets on a spin-locked side queue, as interrupts have: the proxy would learn of nothing until a request completes, so the next request could only go out then.
- Queuing packets from a worker thread, keeping the documented lock order: a thread switch per fault and a new teardown race.
- The proxy holding a reference to itself while its packet is out, instead of the proxy–source cycle: a new lifetime scheme with no reference implementation to check it against.
- Operations on `VmoDispatcher` instead of `PagerDispatcher`: Zircon puts them on the pager, and the composition take-then-supply stays out of the syscall layer either way.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Which syscalls step 2 ships | create, create_vmo, supply_pages, detach_vmo; the same plus `op_range(Fail)`, which needs new Io statuses in the ABI | the four, Fail as its own step | None (homework paused) | the four, 2026-10-23 |
| D2. Lock order: a page packet is queued under the VMO lock, after the port in the documented order | revise the documented order (port lock a leaf after PagerProxy); queue from a worker thread | revise, as Zircon does | None | revise, 2026-10-23 |
| D3. How a faulting thread waits on a pager | no deadline, interruptible, Zircon's retry loop for a wait that is not suspendable; Zircon's warning every 20 s and TimeOut after 300 s | no deadline | None | Zircon's overtime handling, 2026-10-23; repair 3 of `docs/repairs/261023/mm.md` becomes reachable and is fixed first |
| D4. take_pages where ours lacks Zircon's zero markers | commit gaps to zero pages and refuse an aux VMO in a COW tree (BadState); add zero markers to VmPageMap first | commit and refuse | None | zero markers first, then take_pages as Zircon does, 2026-10-23 |
| D5. What detach does to pages already supplied | keep them (the contract allows it); unmap and evict them, as Zircon does | keep | None | keep, 2026-10-23 |
| D6. How the user-space path is tested, given no program can start a second thread | kernel `pager.test` plus a `pager_probe` child program on dbg_1 that faults while the shell serves it; add `thread_start` first | pager.test plus pager_probe | None | `thread_start` first, then a two-thread test in the shell on both images, 2026-10-23 |

## Testing and verification
- `pagesource.test`: `vmo_read`/`vmo_write` on a sourced VMO wait and return the supplied bytes; zero markers read as zero and shadow a parent; `take_pages` from a mapped aux VMO and from one with a mapped slice (both refault and read zero), a gap arriving as a zero marker, an aux VMO in a COW tree, every refusal; `detach_source` failing a waiting fault.
- `port.test`: an owned packet is queued, dequeued and freed once with no lock held; `cancel_queued` before and after dequeue, and racing `on_zero_handles`; owned packets do not count against the user packet limit.
- `pager.test` (new, kshell): the READ round trip through `PagerDispatcher`; a second request waiting for the packet; withdrawal; `Complete` last; every teardown order, with a count of live proxies that must return to where it was.
- Shell `check_pager`, both images: argument and rights errors that a single thread can reach, supply before any fault, detach then `Complete` then BadState.
- Shell `check_pager` with a second thread (D6), both images: the second thread faults on the pager VMO while the first serves the READ through real syscalls.
- `ours-verify` on every sub-step.

Cannot be verified: AccessDenied for a pager handle lacking AttachVmo or ManageVmo, since a pager handle has no Duplicate right and there is no `handle_replace`. A pager test that never gets its READ hangs the shell's `port_wait` rather than failing a check, because `port_wait` takes only 0 or infinite; the overtime TimeOut (D3) bounds the faulting thread, not the shell.

## Open questions
- Whether a failed `pager_create_vmo` may still deliver a `Complete` packet for the VMO it never handed out (closing the source detaches it). The plan is to make the proxy and source in one hold of the pager's lock after the zero-handles check, and to document what is left.
- `SwapAsyncRequest`: without it a cancelled primary with waiters can cost the pager a duplicate READ packet. Correct, since a second supply is given back; a performance item for `docs/repairs/`, to go through the user.
