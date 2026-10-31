# kernel-object changes

## Five system calls reach the IOMMU, BTI and PMT, and no program can yet make the first one (2026-10-31)

**Files**
- `system/public/ours/syscalls.gni` — `iommu_create`, `bti_create`, `bti_pin`, `pmt_unpin` and `bti_release_quarantine`, appended at the end of the table.
- `system/public/ours/bti.hpp`, `system/public/ours/syscall/resource.hpp`, `system/public/ours/vdso.hpp`, `kernel/lib/syscall/include/syscall/handlers.hpp` — the limits `BTI_PIN_MAX_PAGES` and `IOMMU_DESC_MAX_SIZE`, the System slot `RSRC_SYSTEM_IOMMU_BASE = 8`, and the includes the generated declarations need.
- `kernel/lib/syscall/src/bti.cpp`, `kernel/lib/syscall/BUILD.gn` — the handlers.
- `kernel/object/bti_dispatcher.{hpp,cpp}`, `kernel/object/pmt_dispatcher.{hpp,cpp}`, `kernel/object/iommu_dispatcher.{hpp,cpp}` — `BtiDispatcher::pieces()` and `check_options()`, `PmtDispatcher::query_pieces()`, and the descriptor size `IommuDispatcher::create()` takes.
- `kernel/commands/bti_test.cpp` — checks of the three additions.
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_device_dma_refusals()`.
- `.agents/tasks/driver-capabilities/PLAN.md` — what 5b verified and what step 6 has to add.

**Symptom**
Latent, cannot be triggered today. The calls exist, and the only way to start is `iommu_create` with a System resource of slot 8, which nothing mints yet (step 6). So no program can get past the first call, and the rest answer only the refusals a program can reach without it: a handle that names nothing, a handle of another type, a resource of another kind.

**Cause**
The objects of the previous entry are inert until a program can ask for them, and the way a program asks is where the traps are, because a system call is where a user's choices (a count, a size, a handle that may be any object) meet the kernel's rules. Four of them shaped this change.

- A handle that is dropped is a last handle closed. The BTI reads the last handle of a pin closing without an unpin as "the driver went away with its device running", and quarantines the pin. `Handle::make(KernelHandle)` takes its argument by value and, when it refuses (the process is at its quota), drops it, which closes the last handle of a pin the program never saw: a quota error would quarantine the pin and put the BTI in a fault state for every later pin. So `bti_pin` takes the token out of its `KernelHandle` with `release()`, makes the handle from the plain reference (`Handle::make(Arc)`, which on refusal only drops a reference), and unpins on every failure after the pin succeeded, before the handle is dropped.
- `pmt_unpin` takes its handle out of the table before it looks at it, and closes it whatever the answer, as `vmo_replace_as_executable` does for the VMO it is given. A handle that is only looked up cannot be unpinned and then closed as one step: another thread could unpin through the same handle in between. The cost is that a call with a handle of the wrong type (BadType) still consumes it; the table says so (`release = true`).
- Everything a program chooses has to be bounded before it costs anything. `bti_pin` works out how many addresses the memory is cut into (`BtiDispatcher::pieces()`) and refuses a wrong `addrs_count` before it pins a page, so the commonest mistake is free. The count alone bounds nothing with `Contiguous`, which is one address for any size, so the size is capped at `BTI_PIN_MAX_PAGES` pages for every option. And the address of each piece is looked up for its own bytes only: a first version asked about the whole rest of the pin for every piece and shared the answer, which is quadratic when the pin is not one run (a gap near the end of a 128 MiB pin made every piece scan to the end, with the VMO's lock held). It was found by the review below, and is linear now.
- The call can wait. `pin_range` waits for a page a pager has not supplied, and a thread suspended during the wait gets `InternalIrqRetry`, which a program must never see. A call marked `blocking` in the table is made again by the vDSO stub, and `bti_pin` can be: it is all or nothing, so a wait that ended undid what it had pinned, and doing it again repeats only the commit of pages that are there already.

The order of the checks is the rule of the other calls: the handles first, each for BadHandle, BadType and AccessDenied, then the rights the options need on the VMO, then the arguments. `bti` is looked up before `vmo`, so a BTI without Map and a bad VMO handle answers AccessDenied: one handle at a time, each fully, as `HandleTable::attach_port_observer()` does for its port.

**Change**
The core of `bti_pin`, after the handles and rights:

```cpp
bti_object->pieces(options, size, pieces);          // InvArgs for options or a size pin() refuses
if (addrs_count != pieces.count) return InvArgs;    // before anything is pinned
if (size / PAGE_SIZE > BTI_PIN_MAX_PAGES) return InvArgs;
bti_object->pin(vmo_object->vmo(), offset, size, options, pinned, rights);
auto token = pinned.release();                      // not a last handle closed
auto handle = Handle::make(Arc<Dispatcher>(token), rights);
if (!handle) { token->unpin(); return NoResource; }
// addresses in chunks of 32 from the stack; a failed copy: token->unpin()
// then out, then table.add(handle)
```

`iommu_create` and `bti_create` keep the full argument lists of the design (`desc`, `desc_size`, `options`), though the stub has no use for them: a descriptor of size 0 and options of 0, InvArgs otherwise. Leaving them out would have been a smaller ABI that a real IOMMU would have to change.

Rejected: a version of `bti_pin` that checks the count after pinning, as the reference does, since it pays for a pin to find a mistake that arithmetic finds; a run cache across the pieces of one call (the quadratic case above); and an address array written all at once from a kernel buffer, which makes the kernel allocate what the program chose (at most 256 KiB here, and not worth it).

**Reference**
Zircon `sys_iommu_create` (`kernel/lib/syscalls/iommu.rs`), `sys_bti_create` and `sys_bti_pin` and `sys_bti_release_quarantine` (`kernel/lib/syscalls/bti.rs`), `sys_pmt_unpin` (`pmt.rs`). The slot number is `ZX_RSRC_SYSTEM_IOMMU_BASE` (8). Two differences, both on purpose: the pin is counted before it is made, not after, and the address of a piece is asked for only for the piece.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS (151 passed). `routing_checks` asks the five calls with no handle, a VMO where a resource, an IOMMU or a BTI is wanted, and the MmIo root as the wrong kind of resource, and that `pmt_unpin` consumed the VMO it was given.
- `kshell_run.py`: `bti.test: ok -- 79 checks` (the 64 of the objects, and `pieces()` for each option and each refused option, `query_pieces()` for both frame layouts, a later piece alone, a piece past the pin, a piece that is not whole pages, and a token asked after its unpin), `btistress.test` ok, and `vmopin.test`, `resource.test`, `physvmo.test` unchanged.
- Mutations, each undone afterwards. Against `routing_checks`: `iommu_create` taking any resource (5 checks failed); `pmt_unpin` not consuming its handle (2). Against `bti.test`: the check that a piece is inside the pin taken out (4 failed, two layouts); every piece answered by the first run (3); a piece longer than its run accepted (3); Compress counted as Contiguous (3).
- An independent read of the handlers, the table and the objects they call, by a reviewer given the code and not the conclusions, found no memory-safety bug, leak, double free or assertion a program can reach, and that every failure after the pin unpins first. It found five things, all taken: the quadratic address lookup; `bti_pin` missing `blocking`; a size not bounded for `Contiguous`; the handle made after the addresses were copied, so a process at its quota paid for the pin and left addresses of unpinned pages in its memory; and the table naming `AccessDenied` for `iommu_create` (a System resource never answers it) and leaving out several answers of `bti_pin`.
- clang-tidy on `bti.cpp`, the three dispatchers and `routing_checks.cpp`: no diagnostic. clang-format: clean.
- Not verified: any success path of the five calls, and every refusal that needs a BTI (the rights, the count, the size, the options, a failure that has to unpin). They need a program that holds the System slot, which is step 6. A pin whose process dies with it outstanding is the same.

**Deliberately not done**
The slot reaching a program (step 6) and `vmo_create_contiguous` (step 7). `object_get_info` for the three objects. The descriptor of a real IOMMU: the call carries `desc` and its size, and the stub reads neither. A limit on the memory a program may pin in all (`docs/repairs/261030/kernel-object.md`, item 5); the per-call cap bounds one call and not many.

**How it was found**
The handle trap came from the reviewer of the previous step, who read `Handle::make()` and saw what its refusal does to a fresh pin; the quadratic lookup and the missing `blocking` from the reviewer of this one. Neither is visible in a test that has a slot to ask with, which is why the reading mattered.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `bti_pin` has pinned the pages and then finds the process is at its handle quota. What happens if it makes the handle with `Handle::make(KernelHandle)` and drops it, and what does it do instead?
2. Why does `bti_pin` cap the size of the pin and not only `addrs_count`?
3. `pmt_unpin` is given a handle to a VMO. What is returned, and what is left of the handle?
