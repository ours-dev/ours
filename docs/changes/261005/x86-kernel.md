# x86-kernel changes

## Title: ioports_request and ioports_release, with a per-process I/O bitmap kept as runs (2026-10-05)

**Files**
- `system/public/ours/syscalls.gni` — `ioports_request` and `ioports_release`, appended after the sampler's calls
- `kernel/lib/syscall/src/ioport.cpp` (new), `kernel/lib/syscall/BUILD.gn` — the two handlers: `validate_ranged_resource()` with the IoPort kind, then the calling process's `open_ioports()` or `close_ioports()`
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — `ProcessDispatcher::open_ioports()` and `close_ioports()`, which hand the ports to the process's address space
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`, `kernel/arch/x86/mm/arch_vm_aspace.cpp` — `ArchVmAspace` holds an `IoBitmap`; `open_ioports()` and `close_ioports()` pass it the address space's mask of running CPUs; `switch_context()` ends with `IoBitmap::load()` where it used to end with a comment asking for it
- `kernel/arch/x86/kernel/include/ours/arch/x86/io_bitmap.hpp`, `kernel/arch/x86/kernel/io_bitmap.cpp` (new), `kernel/arch/x86/kernel/BUILD.gn` — `IoBitmap`: the runs, the version, `load()` for the switch, the reload sent through `IpiService::sync_exec()`, and two test hooks, `open_here()` and `stale_here()`
- `kernel/include/ours/ipi_service.hpp` — at review, `sync_exec()` states which locks its task may take
- `kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp` — `X86Tss` opens and closes ranges of ports: `open_ports()`, `close_ports()`, `close_all_ports()`, `port_is_open()` and `NUM_IO_PORTS`; `init()` closes every port through `close_all_ports()`
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — the `gp_io_rtc` probe; `gp_io`'s comment says the port is closed because the probe never asked for it
- `examples/shell/shell_tests.cpp` — `check_ioports()`, run from `run()` before the checks that close the job
- `kernel/commands/ioport_test.cpp` (new), `kernel/commands/BUILD.gn` — `ioport.test`
- `.agents/tasks/microkernel-interrupts/PLAN.md` — step 10's verification revised for what the shell's probe can show; `ioport.test` added to the files and the test plan
- `docs/repairs/261005/x86-kernel.md` (new) — the cost the switch now pays, to be measured

**Symptom**
None, a planned change: step 10 of `.agents/tasks/microkernel-interrupts/PLAN.md`. Before it every I/O port was closed to ring 3 on every CPU and nothing could open one: `X86Tss::init()` sets every bit of each CPU's bitmap, and `ArchVmAspace::switch_context()` ended with the comment `// Finally, exchange the IO-Bitmap in TSS` and no code. Since step 9 the I/O port root reaches the root on dev and the shell on dev.dbg_1, but no system call took it. Step 12's PS/2 driver needs the i8042's 0x60 and 0x64.

**Cause**
The objects:
- The TSS I/O permission bitmap (`X86Tss::io_bitmap`): 8 KiB in each CPU's TSS, one bit per port 0 to 0xFFFF. Ring 3 runs at IOPL 0, so every `in`, `out`, `ins` and `outs` it executes is checked against the bitmap of the CPU it runs on: a set bit is a #GP, a clear bit runs the access on the device with no trap. The kernel never sees an access it allowed.
- `IoBitmap`: the ports one address space's threads may use, as sorted runs `[base, end)` that never overlap or touch. It is the truth; the TSS bits are a copy of it on each CPU. One per `ArchVmAspace`, so one per process.
- The version: a counter in each `IoBitmap`, bumped by every change under the bitmap's lock.
- The per-CPU record (`Loaded` in `io_bitmap.cpp`): which `IoBitmap` this CPU's TSS reflects, and at which version. Only its own CPU touches it, with interrupts off.
- `ArchVmAspace::m_active_cpus`: the CPUs whose CR3 holds this address space, already kept for the TLB shootdown. Each CPU sets its own bit when it switches in, and clears it when it switches out.
- `IpiService::sync_exec(targets, task, context)`: runs `task` on every CPU in `targets`, the caller's own with interrupts off and the others in their IPI handler, and returns once all have.

The events, for a driver P with one thread on CPU0 and another on CPU1, and a process Q:
1. P's thread on CPU0 calls `ioports_request(the I/O port root, 0x70, 2)`. The kernel asks `validate_ranged_resource(root, IoPort, 0x70, 2)`: the kind matches, the root covers every port, and neither port is the kernel's, so Ok.
2. `ProcessDispatcher::open_ioports()` hands the ports to `ArchVmAspace::open_ioports()`, which calls `IoBitmap::open()`. It allocates the one run the edit may need, takes the lock, inserts `[0x70, 0x72)`, bumps the version from 0 to 1, and drops the lock.
3. It reads P's mask of running CPUs, `{CPU0, CPU1}`, and has `sync_exec()` run `reload()` on both. Each finds its record naming P's bitmap at version 0, so it closes every port in its TSS, opens P's runs, and records version 1.
4. `ioports_request` returns. From here an `outb` to 0x70 on either CPU goes to the RTC without the kernel seeing it.
5. CPU1's scheduler switches from P's thread to Q's. `switch_context()` sets CPU1 in Q's mask, loads Q's CR3, clears CPU1 from P's mask, then calls `IoBitmap::load(Q's bitmap)`: under P's lock it closes P's runs, since its record's version is P's current one; under Q's lock it opens Q's runs, none; it records Q's bitmap and version.
6. Q's thread runs `inb 0x70`. The bit is set, so the CPU raises #GP, and Q is killed unless its exception handler takes it.

Why the ports belong to the address space: the TSS has to change exactly when a CPU starts running another process's threads, and the address space switch is where the kernel already does per-process work on a CPU. The address space also already knows which CPUs run it, `m_active_cpus`, which is the set a change has to reach. A process has one address space, so the bitmap is still the process's.

Why runs, D10 (b): a switch closes the outgoing process's runs and opens the incoming one's, a few bytes each for a driver, and nothing for the processes that never asked, which is nearly all of them. Copying the whole 8 KiB at every switch, D10 (a), would put that copy on every switch to a driver's thread.

Why a change must reach the other CPUs before it returns: a CPU running another thread of P keeps using its own TSS until something rewrites it. Without the reload, a port P released would stay usable by P's thread on CPU1 for as long as that thread kept the CPU, so a release would promise nothing; and a request would leave P's thread on CPU1 faulting on a port it was told is open.

Why the mask is enough to find them: a CPU that loaded P's old version set its bit in P's mask before `load()` took P's lock. The change makes its edit under that lock and reads the mask after dropping it. The lock's release and acquire order the CPU's bit before the read, so the read sees the bit. A CPU that loads P after the edit loads the new version itself. A CPU that switched away in between needs no reload, and the next paragraph is why it is still safe.

Why the version, the window it closes: between step 2's edit and step 3's reload reaching CPU1, CPU1 may switch from P to Q. A switch closes the outgoing process's ports by its runs, and the runs are by then the new ones. For an open that is harmless. For a release it is not: the port just released is no longer in the runs, so the switch does not close it, Q runs with it open, and the reload that then arrives finds CPU1 running Q, not P, and does nothing. Nothing would ever set that bit again, so Q and every process after it on CPU1 would hold a device P gave up. With the version, CPU1's switch sees that its record's version is older than P's, which means its TSS matches runs that are gone, and closes every port instead, 8 KiB, in a window that a switch rarely lands in.

Why the reload rebuilds the whole bitmap rather than applying the difference: the TSS matches an older version whose runs are gone, so "every port closed" is the only state known to be right to lay the new runs over. Changes are rare, a driver asking once at start, and the reload runs only on the CPUs running that address space.

Why the lock is a spin lock always taken with interrupts off, and nothing is allocated under it: `load()` runs in the context switch, under the run queue's lock with interrupts off, and `reload()` runs in an IPI handler. A thread that held the lock with interrupts on could be interrupted by its own CPU's reload, which would spin on the lock for ever. The heap may not be entered with interrupts off, so `change()` allocates first: an edit adds at most one run, either a new run that touches no other or the second half of a run split by a release, so one spare is allocated before the lock is taken, and the runs an edit removes are freed after it is dropped. The edit itself cannot fail.

Why `load()` holds interrupts off across both halves, though it drops each lock between them: a reload arriving between the halves would find the record still naming the outgoing bitmap and open its ports again just after they were closed. The scheduler switches with interrupts off already; the kernel tests that attach themselves to an address space hold off only preemption.

Why the request checks the resource and no right: an I/O port has no later point of use that the kernel sees, so the request is the only moment the kernel can refuse one, and the resource, with the kernel's own ports refused by `validate_ranged_resource()` (step 9), is what it refuses by. The handle needs no right, as for every resource check here. The release is checked the same way so that the two calls mirror each other, though closing a port takes nothing from anyone. Both act on the calling process: a driver opens ports for itself.

Found while finishing the earlier attempt's work:
- `ioport.test`'s reload check used port 0x61, which lies in a run the runs check leaves open, so "open on the other CPU when the open returns" held before the open was made. The port is now 0x80, outside both runs, and a new check first sees it closed there.
- The shell's probe does not show that a switch closes ports. The probe may run on any CPU, and a CPU the shell never ran on with the ports open has them closed whatever the switch does: with the switch's close removed (mutation M1 below), the shell still printed `shell: ioport checks passed`. The probe's comments now claim only what it shows, that a process that did not ask for a port does not have it, and `ioport.test` checks the switch on one CPU, from the kernel. Booted with a single CPU (`SMP=1`), the probe shares the shell's CPU, and then the same mutation does fail the shell.
- The shell skipped the check where it had no I/O port root, which on dev.dbg_1 would have hidden a missing handle, the lesson of step 9's review. It now skips nowhere: on dev it checks the refusal of no handle, BadHandle, and passes as `shell: ioport checks passed (the root keeps the I/O port root)`; on dev.dbg_1 a missing root, job or launcher fails it.
- The window the version closes had no test. `ioport.test` now makes it on purpose: the observer's CPU holds interrupts off, so the close's reload waits, sees through `IoBitmap::stale_here()` that the runs moved on, switches to the kernel's address space, and checks that the port is closed.

Found at review of the step:
- `~IoBitmap()` said no CPU has the bitmap loaded because an address space is destroyed only once its mask of running CPUs is empty. The mask does not show it: `switch_context()` clears its CPU's bit before `IoBitmap::load()` takes the outgoing bitmap's lock, so a free timed by the mask could land while `load()` still reads that bitmap. What holds is when the address space can be freed: a process keeps it to the end, and each of its threads keeps the process until `Thread::destroy()`, a Dpc that runs on the thread's CPU after its final switch. The comment says that now, and so does the line on a dying process's ports under Deliberately not done.
- `IpiService::sync_exec()` said its task must not "take a lock that is held outside interrupt context", and `IoBitmap::reload()`, its task here, takes the bitmap's lock, which `change()` and `load()` hold in thread context and in the context switch. The sampler's timer tasks already did the same with the timer queue's lock. The rule the deadlock argument needs is narrower: every holder keeps interrupts off while it holds the lock and never waits for another CPU meanwhile, so the task cannot spin on a lock held by the code it interrupted, or by a CPU waiting for its own. `ipi_service.hpp` states that rule now; both locks keep it.
- `ioports_release`'s documentation said it answers OutOfMem when the change cannot be recorded, "closing the middle of an open range splits it in two", which reads as though only a split can fail. `IoBitmap::change()` sets the one spare run aside before every edit of a port or more, whatever the edit turns out to need, so releasing a whole run can answer OutOfMem as well, as the reference's `SetIoBitmap()` can. The documentation says so now, and that such a release has closed nothing.

**Change**
The TSS, before and after:

```cpp
// before: init() set every bit, and nothing else wrote the bitmap
auto init() -> void {
    io_port_bitmap_base = static_cast<u16>(ustl::mem::offset_of(&Self::io_bitmap));
    for (auto &byte: io_bitmap) {
        byte = 0xff;
    }
}

// after: the same start, and ranges that IoBitmap opens and closes
auto init() -> void {
    io_port_bitmap_base = static_cast<u16>(ustl::mem::offset_of(&Self::io_bitmap));
    close_all_ports();
}
auto open_ports(u32 first, u32 end) -> void;    // clears the bits of [first, end)
auto close_ports(u32 first, u32 end) -> void;   // sets them
auto close_all_ports() -> void;                 // all 8 KiB, and the byte after
```

The end of the address space switch, before and after:

```cpp
// before
        if (from != nullptr && from != to) {
            from->m_canary.verify<Self>();
            set_cpu(from->m_active_cpus, cpu, false);
        }
    } else { ... }

    // Finally, exchange the IO-Bitmap in TSS
}

// after
        if (from != nullptr && from != to) {
            from->m_canary.verify<Self>();
            set_cpu(from->m_active_cpus, cpu, false);
        }

        // The ports this CPU's TSS opens follow the address space: those of
        // the one it ran before close, |to|'s open.
        IoBitmap::load(to->m_io_bitmap);
    } else { ... }
}
```

The switch's half, new:

```cpp
auto IoBitmap::load(IoBitmap &next) -> void {
    ::arch::AutoInterruptDisabler const no_interrupts;
    auto &loaded = CurrentLoaded::get();
    auto &tss = this_tss();

    if (auto *const previous = loaded.bitmap; previous != nullptr) {
        Guard<sync::SpinLock, IrqSave> guard{previous->m_lock};
        if (loaded.version == previous->m_version) {
            previous->close_in(tss);
        } else {
            tss.close_all_ports();   // the runs no longer say which ports this TSS opens
        }
    }

    Guard<sync::SpinLock, IrqSave> guard{next.m_lock};
    next.open_in(tss);
    loaded = Loaded{.bitmap = &next, .version = next.m_version};
}
```

A change, new (abridged):

```cpp
auto *spare = new (mm::GAF_KERNEL) Run{};       // the one run an edit may add
...
{
    Guard<sync::SpinLock, IrqSave> guard{m_lock};
    if (change == Change::Open) {
        open_runs(base, end, spare, freed);
    } else {
        close_runs(base, end, spare, freed);
    }
    m_version += 1;
}
delete spare;                                    // null if the edit used it
freed.clear_and_dispose([](Run *const run) noexcept { delete run; });
IpiService::sync_exec(running.load(std::memory_order_relaxed), &Self::reload, this);
```

The system call, new:

```cpp
auto ioports_request(ours::Uandle const resource, u16 const io_addr, u32 const len) -> Status {
    if (auto const status = validate_ranged_resource(resource, RsrcKind::IoPort, io_addr, len);
        status != Status::Ok) {
        return status;
    }
    return ProcessDispatcher::current()->open_ioports(io_addr, len);
}
```

Rejected:
- Copying the whole 8 KiB at every switch, D10 (a): see Cause.
- Sending the reload to every CPU with preemption held off around the change, as the reference does: it interrupts every CPU for every change, the plan asks for the CPUs running the process's threads, and it leaves the window the version closes, since a CPU that switched away in between is not the one the reload reaches.
- Closing every port at every switch away from a process that holds any, which needs no version: it costs 8 KiB at every switch away from a driver's thread to close a window a switch rarely lands in.
- Keeping the bitmap in `ProcessDispatcher`: the switch, in the arch layer, would have to reach up into `kernel/object` for it.
- A general run-length bitmap library: there is none in this tree, and the two edits the bitmap needs are about seventy lines with the allocation kept outside the lock.

**Reference**
- Zircon `sys_ioports_request()` and `sys_ioports_release()` in `zircon/kernel/lib/syscalls/driver.cc`: `validate_resource_ioport()` (`zircon/kernel/object/include/object/resource.h`, `validate_ranged_resource()` with `ZX_RSRC_KIND_IOPORT`), then `IoBitmap::GetCurrent()->SetIoBitmap(io_addr, len, enable)` on the caller's own address space. The same here, through `ProcessDispatcher::current()`; on any other architecture Zircon answers `ZX_ERR_NOT_SUPPORTED`, and here no other architecture has an `ArchVmAspace` yet.
- Zircon `IoBitmap::SetIoBitmap()` in `zircon/kernel/arch/x86/ioport.cc`: allocates the run-length bitmap and one free-list element before the lock, holds off preemption, edits under an IrqSave spin lock with `RleBitmapBase::SetNoAlloc()` or `ClearNoAlloc()` (`zircon/system/ulib/bitmap/include/bitmap/rle-bitmap.h`), then `mp_sync_exec(mp_ipi_target::ALL, 0, IoBitmap::UpdateTask, ...)`. `IoBitmap::UpdateTask()` rebuilds the TSS bitmap, `x86_reset_tss_io_bitmap()` then `x86_set_tss_io_bitmap()`, on a CPU whose current address space's bitmap is the one changed, as `IoBitmap::reload()` does here.
- Zircon `X86ArchVmAspace::ContextSwitch()` in `zircon/kernel/arch/x86/mmu.cc` ends with `x86_clear_tss_io_bitmap(old_aspace->io_bitmap())` and `x86_set_tss_io_bitmap(aspace->io_bitmap())`, the two halves of `IoBitmap::load()`; the bitmap is `X86ArchVmAspace::io_bitmap_`, an address space's, as here.
- Three departures. The reload goes to the address space's running CPUs, not to all of them (D10). The change does not hold off preemption: the version and the mask, read after the edit, cover the caller being switched away midway (see the comment on `IoBitmap::change()`). And the version itself: `x86_clear_tss_io_bitmap()` closes the outgoing address space's ports by its current runs, and `UpdateTask()` does nothing on a CPU running another address space, so a CPU that switches away between a release and the IPI appears to keep the released port open for whatever runs there next. That reading of the reference is inferred from its code and was not tested there; `ioport.test` makes that interleaving here, and mutation M4 below shows what this kernel would do without the version.

**Verification**
- `boot_check.py`: dbg PASS (34 passed, 2 skipped), rel PASS (30 passed, 9 skipped, 3/3 required). dev.dbg_1 prints `shell: ioports_request(no handle, 0x70, 2) = BadHandle`, `(the root, 0xFFFF, 2) = OutOfRange`, `(the root, COM1 0x3F8, 1) = AccessDenied`, `(the IRQ root, 0x70, 2) = BadType`, the release of COM1 AccessDenied, the request for the RTC Ok, `shell: RTC through 0x70/0x71: register D 0x80, register B 0x2, seconds 0x6` (the seconds vary from boot to boot), `shell: probe gp_io_rtc killed after exception 0x8, while 0x70 is open in this shell`, the release Ok and `shell: ioport checks passed`. dev prints `shell: ioport checks passed (the root keeps the I/O port root)`.
- `kshell_run.py` on dev.dbg_1: `ioport.test: ok -- 14 checks`, and `resource.test: ok -- 32 checks` and `irq.test: ok -- 30 checks` unchanged. `ioport.test` covers seventeen opens and closes that stand alone, touch, overlap, swallow runs, trim them and split one, each compared port by port with a model over 256 ports; the last port; InvArgs past it; an empty range; the switch to the kernel's address space and to another user one closing the test's ports, and back opening them; the reload of another CPU running the address space, a port that starts closed there seen open when the open returns and closed when the close returns, never opened on the caller's CPU; and the switch made while a close's reload is held off.
- The checks fail when they should. Each mutation was made, built and run, then taken out:
  - M4, a stale switch closes the runs as they now stand, which is the window the version closes: `ioport.test: FAIL  and that switch closes the port, though the runs no longer name it`, 1 of 14.
  - M1, a switch never closes the outgoing process's ports: `ioport.test` fails 3 of 14, `after switching to the kernel's address space: port 0x7 is true, expected false`. The shell still passed, which is the finding about its probe under Cause; with `SMP=1` it fails, `shell: probe gp_io_rtc was not killed by a #GP (exception 0x0) while 0x70 is open in this shell`.
  - M2, a change sends no reload: on dev.dbg_1 the shell's first read of the RTC after its request is a #GP, `terminating process 'bin/shell'`, and boot_check fails with no end marker.
  - M5, `ioports_request` skips its resource check: `ioports_request(the root, 0xFFFF, 2) = InvArgs (expected OutOfRange)`, COM1 `Ok (expected AccessDenied)`, the IRQ root `Ok (expected BadType)`, `shell: ioport checks FAILED`.
- Booted with a single CPU (`SMP=1`): dbg PASS and rel PASS, and `ioport.test: ok -- 7 checks`, its reload checks skipped with a warning, since they need two CPUs.
- Repeated boots of the final code, which later changed only in comments: 54 of both images or of dbg alone, 12 of them four pairs at a time, every one PASS. One earlier boot of the same code, the first after the rebuild that added the dev check, reported dbg FAIL while rel passed; the next run overwrote its log before it was read, so its cause is not known. The lines it printed showed the shell had finished and exited, which rules out a hang and points at a FAILED or error-level line.
- clang-tidy on every changed kernel and user source, filtered to the changed lines: no diagnostics. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean.
- At review, from the committed tree: M4 made again, built into a separate out directory and run, `ioport.test: FAILED -- 1 of 14 checks` on the same check, then taken out; and 25 more boots of dev.dbg_1 alone, every one PASS, the shell finishing between 5.9 s and 55.0 s after start against boot_check's 60 s. The earlier lone FAIL did not recur.
- Not tested: a process with threads on several CPUs using its ports at once, since the shell is one thread and the reload of another CPU is exercised by a kernel thread in the test's address space; hardware other than QEMU's.

**Deliberately not done**
- Taking the i8042's 0x60 and 0x64 off the deny list: step 12, with the kernel's keyboard driver.
- Releasing a dying process's ports: nothing to do. The bitmap goes with the address space, which is freed only after every thread of the process has made its final switch away, and each such switch closed the process's ports on its CPU.
- A faster switch for address spaces with no ports: every address space switch now takes both bitmaps' locks with interrupts saved, which nothing has measured. It is a repair item, `docs/repairs/261005/x86-kernel.md` item 1, to be measured before anything is changed.
- A cheaper reload: each change rewrites all 8 KiB on every CPU running the address space, as the reference's does. A driver asks for its ports once at start.
- `ioports_request` on another architecture: no other architecture has an `ArchVmAspace`. One that has no I/O ports would give its `ArchVmAspace` the two calls answering NotSupported.

**How it was found**
None, a planned step. It was finished from an earlier attempt's uncommitted work, made on an older integration branch, committed as work in progress and rebased onto the branch's Sampler work. The one conflict was the end of `syscalls.gni`, where both sides appended calls: the sampler's keep their numbers and the two I/O port calls follow them. The review of that work found the four corrections at the end of Cause, the last two with the help of the mutations. The review of the step found the corrections listed after them.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `ioports_release` reach the CPUs running the process's other threads before it returns, and why is the address space's mask of running CPUs, read after the edit, enough to find every CPU whose TSS needs the reload?
- A CPU switches from P to Q after P released port 0x70 but before the reload reaches it. What would a switch that closes P's current runs leave behind, why would the reload not repair it, and how does the version make the switch close the port anyway?
- Why is nothing allocated while an `IoBitmap`'s lock is held, and how is an edit then guaranteed never to fail halfway?
