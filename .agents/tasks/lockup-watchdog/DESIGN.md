# lockup-watchdog: a tick heartbeat that the other CPUs check, and an NMI dump that prints every CPU's registers and backtrace once, then panics

**Date**: 2026-10-07
**Status**: Accepted

## Problem

Paths: ours paths are relative to the repository root. `linux/` is `~/Workspaces/projects/linux` at 62cc90241548, and `zircon/` is `~/Workspaces/projects/fuchsia/zircon` at 51f05104c130.

- **The hang.** About one dbg boot in a dozen stops dead in the shell's fault-probe phase: every CPU goes silent within 16 ms, with no panic and no error line (`docs/repairs/261007/kernel-smp.md:11-15`). cpu3 stops first and the others stop one by one, "each as it reached whatever cpu3 held or was waiting for" (`kernel-smp.md:14`). The log is `out/hangs/2026-10-07-kshell-dbg-fault-probe.log`; its last line is cpu2's `I'm Thread 1` at 10002628000. The cause is unknown; the suspects are a lock held across an IPI wait, a TLB shootdown waiting for a CPU that never answers, and a lock-order cycle (`kernel-smp.md:20`).
- **Why a debugger did not catch it.** A debugger needs the hang to happen while it is attached. Thirty boots under QEMU's gdb stub passed (`kernel-smp.md:32-35`). If the rate were one in twelve, thirty clean boots would still happen about 7% of the time ((11/12)^30), so the hunt proved nothing either way. Meanwhile every session runs `boot_check.py` and `kshell_run.py` many times a day. With a watchdog in the kernel, each of those boots becomes a witness, and the first hang reports itself.
- **Nothing in the kernel reports a hang today.**
  - The NMI vector is handled, and is silent on purpose (`kernel/arch/x86/kernel/faults.cpp:189-198`).
  - The only way to stop another CPU is the Fixed-vector suspend IPI (`kernel/arch/x86/kernel/mp.cpp:61-69`, `:93-107`). A CPU with interrupts off never takes it, and `IpiService::suspend_others()` just waits out its bound (`kernel/kernel/task/ipi_service.cpp:124-145`).
  - A panic prints no backtrace: banner, message, ring tail and crash record (`kernel/top/panic.cpp:207-232`). A kernel-mode fault prints registers only (`faults.cpp:41-69`).
  - The kernel has no symbolizer. It prints pc as an offset from the image start, for `llvm-objdump` (`faults.cpp:36-39`, `:56-57`). The sampler resolves its addresses on the host (`kernel/lib/sampler/README.md:55`).
- **What a report must contain to name a cause.**
  - For every CPU: the thread, pc as an image offset, RFLAGS.IF (were interrupts off?), the registers and a backtrace.
  - The order in which the CPUs went quiet, which the repair says to start from (`kernel-smp.md:20`).
  - The number of spin locks each CPU holds. The per-CPU count already exists (`kernel/kernel/sync/spin_lock.cpp:94`, `:131`, `:168`).
  - For a CPU spinning on a lock, which lock and which CPU holds it. The lock word already holds its holder's CPU number plus one (`spin_lock.cpp:6-8`, `:104-106`); the waiter side is D12.
  - Printed once and whole, not interleaved with the dbg image's busy `I'm Thread` loops (`kernel/top/main.cpp:76-100`).
- **What changes the picture: a tick-driven check cannot see a hang in which every CPU has interrupts off.**
  - Every spin lock is held, and waited for, with interrupts off (`kernel/include/ours/sync/spin_lock.hpp:196-200`; the wait loop is `spin_lock.cpp:151-159`). So is the console lock (`kernel/lib/logz4/kernel.cpp:48-50`, `:92-96`).
  - If all four CPUs are spinning like that, no CPU takes a tick, so no checker runs.
  - Zircon's heartbeat checker has the same blind spot, and so does Linux's buddy detector. Linux closes it with a performance-counter NMI on each CPU (`linux/kernel/watchdog_perf.c:105`), and Zircon with a hardware watchdog.
  - QEMU's TCG does not emulate the performance counters, and ours keeps the PMI masked (`faults.cpp:268-276`).
  - So the design needs an NMI source that does not depend on any CPU taking interrupts (D4).

## Goals and non-goals

- **Goals:**
  - Detect a CPU whose tick has stopped (stuck with interrupts off) while at least one other CPU still ticks.
  - Catch the case where every CPU is stuck, through an NMI from outside the guest (D4).
  - On detection, NMI every other online CPU. Collect each CPU's registers and backtrace and print them once, from one CPU, without taking a lock a stuck CPU may hold. Then panic.
  - A kshell command that spins one CPU with interrupts off, as the acceptance test.
  - On a healthy boot it costs one relaxed increment per tick per CPU, and one check per second per CPU.
  - Both images build, boot and stay quiet after every step.
- **Non-goals:**
  - Soft lockup: a CPU that still ticks but never schedules (Linux's `softlockup_fn()` and `is_softlockup()`, `linux/kernel/watchdog.c:785`, `:741`). A CPU spinning with interrupts on in `sync_exec()` (`ipi_service.cpp:218-222`), waiting for a CPU that is stuck, shows up in the dump anyway.
  - Hung tasks: every thread blocked while the CPUs idle and tick.
  - Lock hold-time statistics, and Zircon's timed critical sections (`lockup_timed_begin()`/`lockup_timed_end()`, `zircon/kernel/lib/lockup_detector/lockup_detector.cc:548`, `:569`).
  - Continuing after a report, and rate limiting (D8).
  - Symbolizing inside the kernel.
  - An in-guest NMI timer (HPET through the IO-APIC, or the PMU). Later, if a real machine needs one (D4).

## Design

The checker and the report are arch-neutral and live in `kernel/lib/lockup_detector` (D13). Arch supplies two things: sending an NMI to a set of CPUs, and calling the hook from its NMI vector. This is the same split the sampler has, where arch only supplies pc, sp and fp from a frame (`kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp:199-212`).

### Why a stuck CPU's tick stops

- Every CPU runs the periodic tick: only Legacy mode is ever set (`kernel/kernel/time/tick_sched.cpp:78-83`), so an idle CPU ticks too (`kernel/include/ours/time/tick_sched.hpp:20-28`).
- Once a CPU's local APIC timer is enrolled (`kernel/arch/x86/apic/local_apic.cpp:672-702`), the tick goes: ApicTimer vector (`faults.cpp:255-257`), then `apic_handle_timer_irq()` (`local_apic.cpp:380-392`), then `TickDevice::handle_periodic_event()` (`kernel/kernel/time/tick_device.cpp:329-359`), then `TickSched::on_tick()` (`tick_device.cpp:343`).
- So a CPU's count stops only when interrupts are off on it, or when its tick device has stopped.
- `TickSched` holds `m_last_tick` and `m_next_tick`, which nothing reads (`tick_sched.hpp:79-80`). They are not a heartbeat, and this design leaves them alone.

### Objects

- **Heartbeat**: a per-CPU `std::atomic<u64>`, `CPU_LOCAL`. Its own CPU increments it at the top of `TickSched::on_tick()` (`tick_sched.cpp:45-60`), before the Shutdown return, so ticks in every mode count. Other CPUs read it through `CpuLocal::access(var, cpu)` (`kernel/mm/include/ours/mm/cpu_local.hpp:169-172`). Relaxed on both sides: nothing is published through it, it only says whether it moved.
- **Checker snapshot**: per checking CPU, a private array indexed by target CPU of {last count seen, checks unchanged}. Only its owner writes it, in its own tick with interrupts off, so it needs no lock (D2).
- **Reporter claim**: one global `std::atomic<CpuNum>`, invalid until a CPU wins a compare-exchange. One report per boot.
- **Context slot**: per CPU, written only by that CPU in its NMI. It holds:
  - a copy of the interrupted `IrqFrame` (registers, pc, cs, rflags; `interrupt.hpp:172-197`);
  - the current thread's name (`kernel/include/ours/task/thread.hpp:411`);
  - its heartbeat count;
  - `SpinLock::num_held_cpu()` (`spin_lock.cpp:197-200`);
  - up to 16 return addresses;
  - a `ready` flag, stored last with release.
- **Request mask**: a global `std::atomic<CpuMask>` naming the CPUs the reporter is NMI-ing. It is lock-free, which `kernel/include/ours/cpu.hpp:154-155` asserts.
- **NMI sender**: `arch_mp_send_nmi(CpuMask)`, beside `arch_mp_suspend()` (`mp.cpp:61-69`). It writes an ICR with delivery mode NonMaskable (`kernel/arch/x86/lib/arch/include/arch/x86/apic/common.hpp:16-24`) through the bounded `XApic::try_send_ipi()`, whose comment already reserves it for NMI delivery (`kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp:102-118`).
- **NMI hook**: `lockup::on_nmi(IrqFrame &)`, called from the Nmi case of `x86_dispatch_exception()` (`faults.cpp:189-198`). The NMI reaches it through `paranoid_common` on its own IST stack (`kernel/arch/x86/kernel/exception.S:80-117`; `kernel/arch/x86/kernel/idt.cpp:137`). `x86_handle_paranoid_exception()` has already installed this CPU's %gs, found from TR (`faults.cpp:401-406`; `cpu_local.hpp:373-390`).
- **LINT1**: the boot CPU's local APIC LVT LINT1, set to NMI delivery, so that an NMI from outside reaches the guest (QEMU's monitor `nmi`, or a real NMI button). Ours never writes LINT1 today; the register list has no entry for it (`xapic.hpp:13-29`).
- **Stack walker**: `sampler::walk_frame_pointers()` (`kernel/lib/sampler/include/sampler/frame_pointers.hpp:9-26`), reading only within the current thread's kernel stack (`thread.hpp:344`), as the sampler bounds its own unwinds (`kernel/lib/sampler/src/session.cpp:73-87`). The kernel keeps frame pointers in every function except leaves (`build/config/BUILD.gn:269-275`; `build/switch/kernel_switchset.gni:65`, `:103-107`) (D10).
- **Report printer**: runs on the reporter CPU only, after `logz4::enter_panic_mode()` (`system/lib/logz4/src/init.cpp:50-52`). A console lock held by a stuck or parked CPU then costs one bounded wait (`kernel.cpp:25-37`, `:92-111`), not a deadlock.
- **Boot option**: `kernel.lockup.threshold=<seconds>`, default 10; 0 turns detection off. It is parsed beside the other options (`kernel/include/ours/boot/options.hpp:36-50`; `kernel/top/boot_options.cpp:66-68`) (D3, D11).
- **Test command**: `lockup` in `kernel/commands/lockup.cpp`, with subcommands `lockup spin <cpu> [<ms>]`, `lockup spin all`, `lockup nmi` and `lockup status`.

### Events: one CPU stops ticking

1. An init hook at `InitLevel::Platform` arms the checker. It runs after `init_platform()` has started every AP (`kernel/platform/pc/init.cpp:21-28`; `main.cpp:53-55`; `kernel/lib/gktl/include/gktl/init_hook.hpp:61`). Before that, checks do nothing.
2. Every tick on every CPU increments that CPU's heartbeat with one relaxed `fetch_add`.
3. On every 100th tick of its own (1 s at HZ=100, `kernel/include/ours/time/cfg.hpp:12-14`), each CPU checks the others. It skips the check while a panic is in progress, as Linux's `watchdog_timer_fn()` does (`linux/kernel/watchdog.c:809`). That needs a `panic_in_progress()` that reads `PANIC_STARTED`, which is private to `panic.cpp:24` today. It also skips the check once the claim is taken.
4. For each other online CPU whose heartbeat has moved at least once: if the count is the same as at the last check, unchanged goes up by one; otherwise unchanged goes back to 0 and the new count is stored.
5. When a target has been unchanged for `threshold` checks in a row, the checker tries the reporter claim. A CPU that loses the claim, or finds it taken, stops checking.
6. The reporter records its own context from the frame its tick interrupted: `irq::current_frame()`, which the preamble sets (`kernel/kernel/irq/mod.cpp:100-106`). It sets the request mask to every other online CPU and sends them an NMI.
7. Each target, in its NMI, finds a report under way by another CPU. It writes its frame, thread, held-lock count and frame-pointer walk into its slot, and stores `ready` with release. It clears its own Online bit, as `stop_this_cpu()` does (`panic.cpp:49-54`), and halts with NMIs blocked. It never returns.
8. The reporter waits for every requested slot to be ready. The wait is bounded by time on the TSC (about 2 s), not by a spin count: one spin costs about 640 ns under TCG (`kernel.cpp:33-36`).
9. The reporter enters panic mode and prints, in this order:
   - a header: which CPU, found by which, and every CPU's unchanged count, which shows the order in which they went quiet;
   - one block per CPU: thread, pc offset, IF, the registers in `panic_exception()`'s layout (`faults.cpp:56-64`), the held-lock count, and the return addresses as image offsets;
   - "did not answer" for any slot that never became ready.
10. The reporter calls `panic("lockup: cpu<N> took no tick for <T> s")`, and the panic runs as usual:
    - `vpanic()` finds the parked CPUs already gone from the Online mask, so `suspend_others()` returns at once (`ipi_service.cpp:139-141`);
    - the report is followed by the banner, the message, the ring tail and the crash record (`panic.cpp:207-232`);
    - dbg_1 then resets, because its cmdline sets `kernel.panic.reboot=true` (`kernel/products/image/dev.dbg_1/BUILD.gn:92`).

### Events: every CPU stops (the NMI from outside)

1. The host script sees the serial log stop growing and sends `nmi` to QEMU's monitor. The hunt already treats 15 s of silence as a hang (`kernel-smp.md:34`). `boot_check.py` already opens a monitor of its own for rel's keys (`.agents/skills/ours-verify/scripts/boot_check.py:188-199`).
2. QEMU delivers the NMI through each CPU's LINT1. Only the boot CPU has LINT1 unmasked, which is Linux's choice too (`setup_local_APIC()`, `linux/arch/x86/kernel/apic/apic.c:1629-1641`).
3. The boot CPU's NMI finds no report under way and no request for itself. An NMI that nothing inside the kernel asked for is a request for a dump: the boot CPU takes the claim and runs events 6-10 from NMI context, using the NMI's frame as its own context. Linux sends the same kind of NMI to `unknown_nmi_error()` (`linux/arch/x86/kernel/nmi.c:326`).
4. If two CPUs both get the NMI, the one that loses the claim acts as a target (event 7).
5. One constraint: here the reporter runs on the 16 KiB NMI IST stack, which has no guard page (`kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp:39`; `kernel/arch/x86/kernel/descriptor.cpp:140-146`). Step 1 measures how deep the stack goes.

### What the NMI handler may and may not do

- **May:**
  - read per-CPU data, once %gs is installed;
  - read its own frame and the TSC;
  - use atomics on its own slot, the claim and the request mask;
  - walk its own thread's stack, through a reader bounded to that stack.
  - As the reporter only, and after entering panic mode: send NMIs and print.
- **May not:**
  - Take any `SpinLock`, `Mutex` or IPI mailbox lock: the code it interrupted may hold it (`faults.cpp:190-197`).
  - Log outside panic mode: `SerialLogger::acquire()` waits forever outside panic mode (`kernel.cpp:92-96`).
  - Allocate (the heap takes locks), or touch `CURRENT_FRAME` or the preempt counts (`faults.cpp:380-392`).
  - Fault, or `iretq` before it is done: an `iretq` unmasks NMIs, and the next NMI lands on the same IST frame (`descriptor.cpp:140-146`).
  - Print 256 lines or more. `ring::append()` takes no lock, but it spins on a slot that someone is still writing (`system/lib/logz4/src/ring.cpp:48-60`). If the interrupted code on this CPU was writing a slot, and 256 newer lines wrap round onto it, the NMI spins forever.
- **An accepted hazard:** in xAPIC mode, which ours runs (`local_apic.cpp:28`), the ICR is two registers written in order (`xapic.hpp:124-151`). A reporter in NMI context may have interrupted its own CPU between those two writes. When it sends its NMIs, the interrupted IPI goes to the wrong CPU. The machine panics anyway, so this does not matter.

### Counterparts

| Role | ours (proposed) | Linux | Zircon |
|---|---|---|---|
| Heartbeat | tick count bumped in `on_tick()` | `hrtimer_interrupts`, bumped by `watchdog_hardlockup_kick()` from `watchdog_timer_fn()` (`linux/kernel/watchdog.c:199`, `:795`) | `last_heartbeat`, stamped by `DoHeartbeatAndCheckPeerCpus()` (`zircon/kernel/lib/lockup_detector/lockup_detector.cc:397`) |
| Check | every CPU checks every other; private snapshots; N unchanged checks | `is_hardlockup()`, `watchdog_hardlockup_check()` (`watchdog.c:183`, `:207`); buddy `watchdog_buddy_check_hardlockup()` (`linux/kernel/watchdog_buddy.c:86`) | `HeartbeatLockupChecker::PerformCheck()` with the `current_checker_id` claim (`lockup_detector.cc:225`, `:438-452`) |
| Interrupts-off-everywhere source | NMI from outside, through LINT1 (D4) | perf overflow NMI, `watchdog_overflow_callback()` (`linux/kernel/watchdog_perf.c:105`) | hardware watchdog, `hw_watchdog_pet()` (`lockup_detector.cc:408-410`) |
| Reaching the other CPUs | NMI IPI | `trigger_allbutcpu_cpu_backtrace()`, then `nmi_trigger_cpumask_backtrace()` (`linux/lib/nmi_backtrace.c:39`), then `nmi_raise_cpu_backtrace()` (`linux/arch/x86/kernel/apic/hw_nmi.c:34`) | `CpuContextExchange::RequestContext()` with `SendNmiIpi()` (`zircon/kernel/lib/backtrace/include/lib/backtrace/cpu_context_exchange.h:105`; `global_cpu_context_exchange.h:20-23`), one CPU at a time (`zircon/kernel/lib/lockup_detector/diagnostics.cc:93-125`) |
| Target in its NMI | captures into its slot, parks | `nmi_cpu_backtrace()` prints from the NMI (`nmi_backtrace.c:103`) | `HandleRequest()` captures and returns (`cpu_context_exchange.h:159`), from `x86_nmi_handler()` (`zircon/kernel/arch/x86/exceptions_c.cc:238`) |
| After the report | panic | `nmi_panic()` if `hardlockup_panic` is set, otherwise carry on | `KERNEL_OOPS` below the fatal threshold, `platform_halt(SoftwareWatchdog)` above it (`lockup_detector.cc:276-290`) |
| Test | `lockup spin <cpu>` | lkdtm `HARDLOCKUP` (`linux/drivers/misc/lkdtm/bugs.c:334`) | `k lockup test_spinlock`, `lockup_trigger_spinlock()` (`lockup_detector.cc:668`) |

### Staging

1. **NMI dump on demand.** Add the context slot, the NMI hook, `arch_mp_send_nmi()`, the reporter and printer, parking, LINT1 set to NMI on the boot CPU, and `lockup nmi`.
   - Verify: on dbg_1, `lockup nmi` prints one block per CPU, once, then the panic banner. The monitor's `nmi` gives the same report. `boot_check.py` passes.
   - This step already helps the hunt: a silent boot can be NMI'd.
   - Landed on 2026-10-07 (PLAN.md step 1). Revised, each keeping the decisions as taken:
     - The namespace is `lockup_detector`, the library's directory name, not `lockup`: a library's namespace matches its directory. The hook is `lockup_detector::on_nmi()`.
     - Arch supplies a third thing besides the NMI send and the hook: `lockup_detector::arch_dump_registers()`, a CPU's register lines, because the register names are arch's (x86 shares them with a kernel fault's report, `faults.cpp` `log_registers()`). `arch::IrqFrame` gains `irqs_enabled()` beside `pc()`, `sp()`, `fp()` and `from_user()`, for the IF the report prints.
     - A target clears its Online bit before it publishes its slot, not after (event 7). A reporter that has seen every slot then also sees every answering CPU out of the mask, so its panic's `suspend_others()` waits for none of them.
     - The context slots are a plain array indexed by CPU, not `CPU_LOCAL` data: the reporter reads every slot by number, and a slot needs no per-CPU unit, so an NMI before `CpuLocal::init()` still has one. A slot has no heartbeat count yet; step 2 adds the heartbeat.
     - `arch_mp_send_nmi()` returns the CPUs whose send it gave up on, and the report names them apart from a CPU that was sent an NMI and did not answer.
     - `lockup nmi` sends its own CPU an NMI through `arch_mp_send_nmi()`, so it takes the same path as an NMI from outside.
     - The reporter writes a bare newline before its first line: a target parked half way through a log line leaves the console there, and the report's first line would otherwise be glued to it.
     - A target whose own panic is under way does not park (D7 holds for every other target). A CPU can start a panic after the reporter has looked for one (event 3's stand-down) and before the reporter's NMI reaches it; parked, it took its panic with it, since the reporter's own panic then found the claim taken, logged to the ring only and stopped: no banner, no message, no crash record, no reset (reproduced in review with a temporary command and delay, 2026-10-07). Such a target fills its slot, marks it `Panicking` and returns to its panic, which waits for the reporter to leave the Online mask. `panic.cpp`'s `PANIC_STARTED` flag became `PANIC_CPU`, the claiming CPU, and `this_cpu_in_panic()` sits beside `panic_in_progress()`.
2. **Heartbeat and checker.** Add the heartbeat, the checker, the threshold option, the panic stand-down and `lockup spin <cpu> [<ms>]`.
   - Verify: `lockup spin 2` reports once, within threshold plus 2 s, naming cpu2 with IF=0 and pc in the spin. A spin below the threshold stays quiet. Both images boot.
3. **Scripts.**
   - `kshell_run.py` treats `lockup spin` and `lockup nmi` like `crash`: they end the machine (`scripts/kshell_run.py:329-358`), and the script checks the report.
   - On a silent boot, `boot_check.py` and `kshell_run.py` send `nmi` and keep reading.
   - Verify: `lockup spin all` is caught only through the script's NMI.
4. **Threshold data.** Add `lockup status`: each CPU's largest unchanged count so far. Run under host load to confirm the threshold with data. Concurrent boots need the user's leave (`kernel-smp.md:40`).
5. **Optional: the spinning CPU's lock (D12).** Record which spin lock a CPU waits on, so the report prints a wait-for graph.
6. **Optional: `.eh_frame` unwinding (D10 (b)).**

## Alternatives considered

- **The debugger hunt alone.** It needs the hang to happen during the hunt; thirty boots did not reproduce it (`kernel-smp.md:32-35`). It stays as the way to inspect a hang the report has named.
- **A host-only watchdog** (silence, then `info registers -a`, as the hunt does at `kernel-smp.md:34`). It covers the every-CPU case. But register dumps have no backtrace or thread, and it misses a partial hang in which the CPUs still running keep the log moving. It is kept as one half of D4.
- **A Fixed-vector IPI for the dump** (`sync_exec()` or the suspend IPI). The CPU with interrupts off, the one that matters, never takes it (`mp.cpp:45-59`).
- **Each target prints from its own NMI** (Linux). Panic mode would have to be on before any NMI is sent, and four writers would interleave line by line. Ours has no counterpart of `printk_cpu_sync`. Capturing on the target and printing from the reporter keeps one writer.
- **Report and continue** (Zircon's oops). Targets would have to return from the NMI, and the healthy CPUs would keep printing into the report. The console would need panic mode for good, since `m_abandoned` never clears (`kernel.cpp:113-124`). It also needs rate limiting. For a hang that never ends, carrying on buys nothing.
- **A PMU overflow NMI** (Linux). TCG does not emulate the counters, and the PMI stays masked (`faults.cpp:268-276`; `local_apic.cpp:707`).
- **An HPET comparator routed through the IO-APIC as an NMI** (Linux's IO-APIC `nmi_watchdog=1` before 2.6.38). It needs no host. But it is more machinery, its routing on QEMU is unverified, and the HPET's channel 0 is already the legacy tick route (`kernel/arch/x86/kernel/time/hpet.cpp:307-333`). Left for a real machine (D4 (c)).
- **A timestamp heartbeat** (Zircon). A whole-VM pause, such as a debugger stop or a host sleep, ages every heartbeat at once and fires on resume. Counting the checker's own checks does not (D3).
- **The boot CPU checks alone.** Blind when the boot CPU is the stuck one.
- **Zircon's monitored critical-section names** (`MonitoredSpinLock::Acquire(name)` calls `LOCKUP_BEGIN(name)`, `zircon/kernel/include/kernel/spinlock.h:62-72`). They need lock names, and ours has none: `LockNameStorage` is empty (`kernel/include/ours/sync/priv/lock_name_storage.hpp:8-14`). Recording the waited-on lock's address and holder (D12 (b)) gets more without names. The `Monitored` option (`spin_lock.hpp:22-34`, `:75-77`) stays reserved.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Heartbeat source | (a) a per-CPU count bumped at the top of `TickSched::on_tick()` (Linux `hrtimer_interrupts`); (b) a per-CPU 1 s `time::Timer` that stamps a time (Zircon); (c) PMU cycles (Linux perf) | (a): zero extra interrupts, because every CPU ticks today. Move to (b) when tickless idle arrives | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D2. Who checks whom | (a) buddy ring, each CPU checks the next (`watchdog_buddy.c:86`); (b) all check all, with a shared per-target state and a claim (Zircon); (c) all check all, each checker with private snapshots; (d) the boot CPU alone | (c): one writer per snapshot means no claim and no ring to keep in step as CPUs come online. N reads a second at N <= 16 | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D3. Threshold and its unit | (a) time since the last heartbeat (Zircon, 3 s oops and 10 s fatal, `zircon/kernel/lib/boot-options/include/lib/boot-options/options.inc:185`, `:196`); (b) N consecutive 1 s checks, counted in the checker's own ticks, with the target's count unchanged (Linux `is_hardlockup()`) | (b), N = 10 (Linux's `watchdog_thresh`, `linux/kernel/watchdog.c:50`). It survives whole-VM pauses, and step 4 measures the margin | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D4. Catching a hang where every CPU has interrupts off | (a) accept the gap; (b) an NMI from outside: the scripts send QEMU's `nmi` on silence, LINT1 is set to NMI on the boot CPU, and an NMI nobody asked for means "dump"; (c) an HPET comparator routed through the IO-APIC as an NMI; (d) a PMU NMI, which TCG lacks | (b) now; (c) only for a real machine | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D5. How the other CPUs are reached | (a) an NMI IPI; (b) a Fixed-vector IPI; (c) no interrupt, heartbeat only | (a): the only one a CPU with interrupts off takes | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D6. Who formats a target's state | (a) the target prints from its NMI (Linux); (b) the target captures into its own slot and the reporter prints (Zircon) | (b): one writer, and no lock or formatting in the target's NMI | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D7. What a target does after answering | (a) returns from the NMI (Zircon); (b) parks for good with its Online bit cleared, like `stop_this_cpu()` | (b): the machine stops moving under the report, and `suspend_others()` does not wait for it | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D8. After the report | (a) continue, rate-limited (Zircon below fatal, Linux without `hardlockup_panic`); (b) panic: crash record, and a reset where `kernel.panic.reboot` says so; (c) halt without a reset, keeping QEMU for lldb | (b). Revisit (c) if a hunt wants QEMU kept (Open questions) | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D9. How the output is serialised | (a) the normal logger, whose lock waits forever; (b) one printer, the reporter, after `enter_panic_mode()`, with the targets parked; (c) ring only, replayed by the panic | (b) | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D10. Unwinding | (a) frame pointers through `walk_frame_pointers()`; (b) `.eh_frame` through `KernelUnwinder` (`kernel/lib/sampler/include/sampler/kernel_unwinder.hpp:47-85`), with an FDE index of about 100 KiB built when the checker is armed (`session.cpp:115-160`) | (a) first: no allocation, and it works when the heap is what hangs. It loses a leaf's caller. (b) as step 6 | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D11. Which images, and how it is turned off | (a) both images, on by default, turned off with `kernel.lockup.threshold=0`; (b) dbg only, gated on `KERNEL_DEBUG_LEVEL`; (c) off by default, turned on by dbg_1's cmdline | (a): rel (`kernel/products/image/dev/BUILD.gn:196`, built with dbg.0) is where the hang has not been seen, and a rel hang should report too | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D12. The lock half | (a) only the held-lock count; (b) also record, on the contended path of every `SpinLock` (`spin_lock.cpp:151-159`), the lock this CPU spins on, so the report reads the holder from the lock word; (c) Zircon-style names on `Monitored` locks | (a) now, (b) as step 5: the uncontended path costs nothing | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D13. Placement | (a) `kernel/lib/lockup_detector`, beside Zircon's place; (b) `kernel/kernel/lockup` | (a): arch-neutral checker and report; arch supplies the NMI send and the hook | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |
| D14. The crash record | (a) the 32-line tail any panic keeps (`panic.cpp:102`, `:224`; `kernel/top/crash_record.cpp:107-118`), with the found CPU printed last so its block lands in the tail; (b) the whole report, through a tail length passed down the panic path | (a) now | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |

D2, D6 and D7 are concurrency decisions. Under the working rules, the user writes the argument first: which invariant, who accesses it, and the worst interleaving. Two to start from: whether the heartbeat may be relaxed, and why a target's slot needs `ready` stored with release while the request mask does not carry the slot's data. On 2026-10-07 the user took every decision as recommended ("直接上") and skipped both the predictions and these arguments.

## Testing and verification

- **Host test.** The check is a pure function of (snapshot, current count, threshold) and gets a host test, in the way the sampler's core does.
- **dbg_1, through `kshell_run.py`.** Only dbg_1 has a kernel shell; rel sets `kernel.shell=false` (`kernel/products/image/dev/BUILD.gn:255`).
  - `lockup nmi`: one block per online CPU, once, then one panic banner.
  - `lockup spin 2`: within threshold plus about 2 s, one report naming cpu2. Its block shows IF=0, and its pc resolves to the spin in `lockup.cpp`. Every other online CPU has a block. Then the banner.
  - `lockup spin 2 3000`: no report, and the command returns.
  - `lockup spin all`: no in-kernel report (the documented gap). The script's `nmi` after the silence gives one with every CPU at IF=0.
- **Symbolizing.** Offsets are relative to the image start, and vmours is linked at 0 (`kernel/mm/include/ours/mm/vmm.hpp:11`), so `prebuilt/clang/llvm-23/bin/llvm-objdump -d vmours` resolves them by hand. There is no `llvm-symbolizer` in `prebuilt/clang/llvm-23/bin`.
- **rel.** `boot_check.py` shows it stays quiet. One `nmi` through `boot_check.py`'s monitor, by hand, shows the NMI path works there.
- **Quiet on a normal boot.** `boot_check.py` passes for both images with no `lockup:` line. After a dbg boot, `lockup status` shows each CPU's largest unchanged count (expect 0 or 1). Under host load (concurrent boots plus a build, with the user's leave) it should stay far below 10. Timings vary between boots, 5.7 s to 57 s for one dbg run (`kernel-smp.md:17`) and 16 s to 63 s wall time in the hunt (`kernel-smp.md:35`); that is the margin step 4 measures.
- **What cannot be verified yet:**
  - catching the real hang, which is intermittent; the repair item's own acceptance covers that;
  - an NMI that lands between the two ICR writes;
  - real hardware.

## Open questions

- Does QEMU's `nmi` reach the guest? It goes through LVT LINT1, and ours never writes that register (`xapic.hpp:13-29`). The firmware may have left the boot CPU's LINT1 set to NMI; step 1 reads it at boot before setting it. Answered in step 1 (2026-10-07): yes, and it did before step 1 too. The firmware leaves the boot CPU's LINT1 at 0x400 (NMI, unmasked; logged at boot as `local APIC: the firmware left LINT1 at 0x400`), so the monitor's `nmi` reached the boot CPU, whose NMI vector dropped it. The APs come out of INIT with LINT1 masked, and step 1 writes them NMI and masked. With step 1, the monitor's `nmi` on a booted dbg or rel image gives the full report, from cpu0, the one CPU whose LINT1 is unmasked.
- Is the recorded hang the every-CPU kind? If so, only D4 catches it. Nobody knows until one is caught.
- How deep does the stack go when the report runs in NMI context, on the 16 KiB IST stack with no guard page (`percpu.hpp:39`)? To be measured in step 1. Answered in step 1 (2026-10-07), by painting every CPU's NMI stack with 0xA5 when its TSS is set up and scanning it after `record_panic()` (temporary code, not committed): the reporter used 3896 of 16384 bytes on dbg (`lockup nmi` and the monitor's `nmi` alike) and 3368 on rel; a target used 1124 bytes on dbg and 576 on rel. The reporter's figure includes the whole panic up to the crash record, so a quarter of the stack is the most the report has needed.
- Does this QEMU run all vCPUs on one host thread (round-robin TCG) or one each (MTTCG)? `qemu_boot.py` passes no `-accel` (`scripts/qemu_boot.py:221-232`). The answer decides how a host stall looks to the checker: all vCPUs paused together, or one starved while the others run.
- Should a hunt keep QEMU alive after the report? dbg_1 resets on panic, and `-no-reboot` then ends QEMU (`qemu_boot.py:226`). D8 (c) would keep it for lldb.
- When tickless idle comes (HpTick plus a stopped idle tick, `tick_sched.hpp:24-28`), idle CPUs stop ticking. The checker must then skip them, or the heartbeat must become a timer (D1 (b)).
- Should `walk_frame_pointers()` move from `//kernel/lib/sampler:core` to `kernel/lib/unwind`, beside the CFI unwinder, so that the checker does not depend on the sampler?
