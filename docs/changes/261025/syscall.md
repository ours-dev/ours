# syscall changes

## The four pager calls: pager_create, pager_create_vmo, pager_supply_pages, pager_detach_vmo (2026-10-25)

**Files**
- `system/public/ours/syscalls.gni` — four entries appended at the end of the table (their positions are their call numbers), each with the doc a program reads in `<ours/syscalls>`
- `kernel/lib/syscall/src/pager.cpp` — new: the four handlers, and `publish()`, the handle-out sequence `vmo_create` uses
- `kernel/lib/syscall/BUILD.gn` — `src/pager.cpp` added to the sources
- `kernel/object/include/ours/object/pager_dispatcher.hpp`, `kernel/object/pager_dispatcher.cpp` — `create()` takes `u32 options` and `create_vmo()` takes `VmoOptions`, each checked by the object
- `kernel/commands/pager_test.cpp` — the callers pass the new arguments
- `system/app/shell/shell_tests.cpp` — `check_pager()` and its second thread `pager_reader()`, called from `run()` after `check_task_kill()`

**Symptom**
None, a new feature: until now a pager existed only inside the kernel (`pager.test`), and no program could make one.

**Cause**
None, a new feature. Step 10 of `.agents/tasks/user-pager/`: steps 1–9 built everything beneath (the page source and its waits, zero markers, `take_pages`/`supply_pages`, the page-request packet, PagerProxy, PagerDispatcher); this step is the user-facing surface those exist for.

**Change**
Each handler resolves every handle, in argument order, before it looks at any other argument, so BadHandle > BadType > AccessDenied outranks every argument error (design 4.5). It then calls one PagerDispatcher method; the syscall layer names no mm type.

The rights, one line each with the reason:

- `pager_create_vmo`: pager AttachVmo (it ties a VMO to itself), port Write (requests are queued on it).
- `pager_supply_pages`: pager ManageVmo, pager VMO Write (its contents change), aux VMO Read and Write (its pages are read out, and it reads zero after).
- `pager_detach_vmo`: pager AttachVmo and ManageVmo (detaching undoes the one and ends the other), VMO Write.

The options checks moved into the object, as `port_create` and `vmo_create` already have them ("the port checks |options|"):

```cpp
// before
static auto create(KernelHandle<Self> &handle_out, Rights &rights_out) -> Status;
auto create_vmo(ustl::Arc<PortDispatcher> port, u64 key, u64 size, ...) -> Status;
// after
static auto create(u32 options, KernelHandle<Self> &handle_out, Rights &rights_out) -> Status;
auto create_vmo(VmoOptions options, ustl::Arc<PortDispatcher> port, u64 key, u64 size, ...)
    -> Status;
```

`create_vmo` checks the options before anything else, the way `VmoDispatcher::create_user()` does: an unknown bit is InvArgs, and Resizable, a bit this kernel knows but defers, is NotSupported. The alternative, checking options in the handler, was rejected: it would split one call's argument rules across two layers, and the kernel test, which calls the dispatcher directly, would not see them.

The handle goes out as `vmo_create` sends it (design 4.1): `Handle::make()` (which refuses past the quota, NoResource), then the value copied to the caller, and only then the handle added to the table, so a faulting `out` leaves nothing behind. `pager_create` and `pager_create_vmo` share this as `publish()`.

**Reference**
Zircon `sys_pager_create()`, `sys_pager_create_vmo()`, `sys_pager_supply_pages()` and `sys_pager_detach_vmo()` in `zircon/kernel/lib/syscalls/pager.cc`: the same rights on each handle, and the same order (handles first). Zircon checks `pager_create`'s options in the syscall itself; here the object checks them, matching this tree's `port_create`. Zircon's `pager_op_range`, `query_dirty_ranges` and `query_vmo_stats` are not added (decision D1).

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); both print "shell: pager checks passed". Host tests 146 passed.
- `kshell_run.py`: `pager.test` ok (19 checks), `pagesource.test` ok (71 checks).
- clang-tidy: nothing in the changed sources.
- Control: with the aux VMO's Write requirement dropped from `pager_supply_pages`, dbg reports `pager_supply_pages(a VMO without Write) = Ok (expected AccessDenied)` and "pager checks FAILED"; restored.

The shell check, on both images:

- A second thread reads the first word of the mapped pager VMO.
- The shell's `port_wait` gets a Read packet with the key, offset 0 and one page.
- The shell writes a value into an aux VMO, and `pager_supply_pages` moves it in.
- The thread wakes and reports that value over a channel, and the aux VMO reads zero after.
- A second supply of that page keeps the first.
- After `pager_detach_vmo`, the port gets Complete and nothing more, a supply is BadState, a read of a page never supplied is BadState, and the supplied page still reads.

**Deliberately not done**
- No AccessDenied check for a pager handle missing AttachVmo or ManageVmo: a pager handle has no Duplicate right (as in Zircon), and this kernel has no `handle_replace`, so no program can make a weaker one. The ports and VMOs are checked.
- No check that a page request's wait times out (300 s): a boot cannot wait that long. `pager.test` covers it with a short overtime.
- `pager_op_range` and its Fail command stay a later step (decision D1).

**How it was found**
None

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why does every pager handler resolve all its handles before it checks an offset or an option, and what would a program see differently if it did not?
- `pager_supply_pages` takes the pages out of the aux VMO before it supplies them; what happens to those pages when the supply then fails with OutOfRange or BadState?
- Why does `pager_create_vmo` refuse Resizable with NotSupported but an unknown bit with InvArgs?
