# user-pager: a program can be a pager, READ only, through four system calls

**Date**: 2026-10-25

## Delivered
- `a14b459d`, `01845fea` docs: the design and plan, and decisions D1 to D6.
- `85dd3ac2` fix(mm): `vmo_read`, `vmo_write` and `process_read_memory` wait for a page the source has not supplied, with no lock held, and go on from where they stopped (step 1). Record: `docs/changes/261023/mm.md`.
- `f08e748b` fix(mm): a timed-out page request dumps the address space under its lock (step 2, made reachable by D3). Record: `docs/changes/261023/mm.md`.
- `8eaadca0` feat(mm): zero markers in the page list, read by the cursor, the lookup through parents and the page counts (step 3, D4). Record: `docs/changes/261023/mm.md`.
- `a8ea09a2` fix(mm): a fault that widens a present entry reads it again under the VMO lock (step 4.1). Record: `docs/changes/261024/mm.md`.
- `7809aba3` feat(mm): the walk over every mapping of a node's pages unmaps as well as removing write (step 4.2, after `.agents/tasks/large-pages/`). Record: `docs/changes/261024/mm.md`.
  - `ce5d8f76` fix(mm): a populate maps only what is there, through mm's own `MapOptions`, and `bce77360` refactor(mm): a protection-range walk's result, both raised while reviewing 4.2.
- `fc778789` feat(mm): pages move between VMOs through a splice list; `take_pages()` from a node with no parent, `supply_pages()` from the list (step 4.3). Record: `docs/changes/261025/mm.md`.
  - `85d7eba2` refactor(mm): `VmPageMap` is `VmPageList` (the user's call).
- `5eec63d3` feat(mm): `take_pages()` works on a clone: shared pages are made the node's own first, and a zero marker is left where an ancestor's page would show (step 4.4). Record: `docs/changes/261025/mm.md`.
- `3cae9a58` feat(mm): a VMO detaches from its page source and reports its owner id (step 5). Record: `docs/changes/261025/mm.md`.
- `682de580` feat(object): a port queues packets another kernel object owns, handed back through `free()` with no lock held (step 6). Record: `docs/changes/261025/object.md`.
- `5a77f7da` feat(abi): `PacketType::PageRequest` and `PacketPageRequest` (step 7). Record: `docs/changes/261025/abi.md`.
- Step 8 landed in `.agents/tasks/task-control/`: `c9588747` (a thread raises ThreadRunning and Terminated) and `b6e1ecb8` (`thread_start`, `thread_exit`). Records: `docs/changes/261023/object.md`, `docs/changes/261023/syscall.md`.
- `26173314` feat(object): `PagerProxy` sends a pager VMO's requests to the pager's port, one packet at a time, with the overtime wait of D3 (step 9a). Record: `docs/changes/261025/object.md`.
- `bcc1d2c2` feat(object): `PagerDispatcher` makes, supplies and detaches pager VMOs, and `VmoDispatcher::pager_koid()`; the lock order revised in `docs/design/260923/userspace-interface.md` 4.8 (step 9b, D2). Record: `docs/changes/261025/object.md`.
  - `eddad435` refactor(object): dispatchers check their canary with `m_canary.verify<Self>()` (the user's call).
- `58063fe6` feat(syscall): `pager_create`, `pager_create_vmo`, `pager_supply_pages`, `pager_detach_vmo`, and the shell's `check_pager` (step 10). Record: `docs/changes/261025/syscall.md`.

## Verification
- `ours-verify` at the last step: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Shell `check_pager`, on both images, prints "pager checks passed". In it:
  - A second thread faults on a mapped pager VMO.
  - The shell reads the Read packet off its port and supplies the page from an aux VMO with `pager_supply_pages`.
  - The thread reports the value it read.
  - A detach sends Complete and nothing after it.
  - The check also covers every argument and rights error a program can reach.
- Kernel tests, on the dbg image with the last commit: `pager.test` 19 checks, `pagesource.test` 71, `vmosnapshot.test` 238, `largepage.test` 27, `port.test` ok (7 owned-packet checks and its rounds).
- Control runs at every step, each with one piece taken out and restored after; details in each record. For example:
  - The shell's AccessDenied check fails without the aux VMO's Write requirement.
  - The proxy's lifetime checks fail without the cut in `free()`.
  - `port.test` hangs with `free()` called under the port's lock.
- Not measured: the cost of a pager round trip (fault, packet, supply, wake). There is no benchmark of it yet.
- Not exercised in a boot: the 300 s overtime TimeOut. `pager.test` covers it with a short overtime.

## Deviations from the design
- D6 settled on `thread_start` and a two-thread test in the shell on both images, in place of the `pager_probe` child program the design first proposed.
- Step 9 landed as two commits (9a, 9b), so that the proxy's lifetime could be reviewed apart from the pager's lists.
- Step 10: the options of `pager_create` and `pager_create_vmo` are checked by `PagerDispatcher`, as `PortDispatcher::create()` and `VmoDispatcher::create_user()` check theirs. Zircon checks `pager_create`'s in the system call.
- The shell's `check_pager` makes no supply ahead of any fault, which the design's testing list named. `pagesource.test` covers that case (a page supplied before a fault is read without a request). The shell's own supplies all go to a VMO whose fault is waiting or answered.
- Open question 1 (a failed `pager_create_vmo` delivering Complete for a VMO it never handed out) is settled as planned:
  - The proxy and the source are made in one hold of the pager's list lock, after the zero-handles check.
  - The leftover case, `with_source()` failing after that, still sends Complete. It is documented at `PagerDispatcher::create_vmo()`.

## Found along the way
- Two places where Zircon's `PagerProxy` keeps a reference cycle or a packet longer than it needs to. This kernel fixes both (`docs/changes/261025/object.md`):
  - The cycle is not cut after `CancelQueued` succeeds in `OnDispatcherClose`.
  - A Complete refused with BadHandle stays pending. Here it counts as delivered.
- `ustl`'s alignment helpers assumed a power of two without saying so. They were renamed `ustl::round_up`, `ustl::round_down` and `ustl::is_aligned` (`c2aff0a1`, `01899b58`), and now refuse any other alignment (`dd90ae72`). Record: `docs/changes/261025/ustl.md`.
- Unfixed, each in `docs/repairs/`:
  - The page table takes map options and ignores them: `docs/repairs/261024/arch-paging.md` item 1.
  - A map of a physical mapping maps every region after the first from the wrong address: `docs/repairs/261024/mm.md` item 1.
  - A zero marker put by hand over an ancestor's page leaves its viewer count high (tests only): `docs/repairs/261025/mm.md` item 1.
  - A withdrawn request with waiters is sent to the pager again, for want of `SwapAsyncRequest`: `docs/repairs/261025/mm.md` item 2, recorded with this result, a performance item to go through the user.

## Not done
- `pager_op_range` and its Fail command (D1). DIRTY and writeback, resizable or unbounded pager VMOs, job policy and early wake are also out: non-goals of the design.
- Evicting supplied pages on detach. D5 keeps them.
- COW children of a pager VMO: still refused.
- Committing a VMO with a page source does not wait for its pages, and a copy or a fault asks for one page at a time: `docs/repairs/261023/mm.md` items 1 and 2.
- An AccessDenied check for a pager handle missing AttachVmo or ManageVmo. A pager handle has no Duplicate right and there is no `handle_replace`, so no program can make a weaker one.
