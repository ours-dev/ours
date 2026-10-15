# x86-idle changes

## An idle CPU waits in MWAIT on a word a waker writes, instead of in HLT for an IPI (2026-10-14)

**Files**
- `kernel/arch/x86/lib/arch/include/arch/x86/processor/feature.hpp` — `CpuFeatureType::Monitor`, CPUID.1:ECX bit 3
- `kernel/arch/x86/kernel/feature.cpp`, `include/ours/arch/x86/feature.hpp` — `FEATURE_HAS_MONITOR`, cached at boot
- `kernel/arch/x86/kernel/task/idle_thread.cpp`, `include/ours/arch/idle_thread.hpp` — `ArchIdleThread::idle()` waits in MWAIT on the CPU's monitor word; `wake()`, `waits_in_mwait()`, `num_monitor_wakes()`; the counter `idle.monitor_wakes`; the header guard's closing comment
- `kernel/arch/x86/kernel/mp.cpp` — `arch_mp_reschedule()` and `arch_mp_reschedule_mask()` write a waiting CPU's word instead of sending it an IPI
- `kernel/commands/idle.cpp` — `idle.stats` says how idle CPUs wait and counts the waits a write ended; `idle.test` checks, with MWAIT, that waking an idle CPU writes its word

**Symptom**
None, step 5 of `.agents/tasks/idle-power/PLAN.md`. An idle CPU executed `sti; hlt`, which reaches only C1 and is ended only by an interrupt, so every wakeup of an idle CPU was an IPI. Deeper states are entered only through MWAIT.

**Cause**
HLT has no way to say how deeply to sleep and no way to be woken but an interrupt. MWAIT takes a hint naming a C-state, and also ends when another agent writes a cache line armed with MONITOR. That lets a waker wake an idle CPU with one store to memory it would touch anyway, instead of an interrupt sent through the local APIC, which also has to be delivered, dispatched and acknowledged on the target.

**Change**
The wait, on a CPU with MONITOR:

```cpp
auto &state = mm::CpuLocal::access(MONITOR).state;   // alone on its 64-byte line
state.store(Waiting, std::memory_order_relaxed);
asm volatile("monitor" : : "a"(&state), "c"(0), "d"(0) : "memory");
if (state.load(std::memory_order_acquire) == Waiting) {
    asm volatile("sti; mwait" : : "a"(MWAIT_HINT_C1), "c"(0) : "memory");
} else {
    asm volatile("sti" ::: "memory");
}
if (state.exchange(NotWaiting, std::memory_order_acquire) == NotWaiting) {
    IDLE_MONITOR_WAKES.add(1);   // a waker ended it
}
```

and the waker, before any reschedule IPI:

```cpp
auto ArchIdleThread::wake(CpuNum const cpu) -> bool {
    ...
    return mm::CpuLocal::access(MONITOR, cpu).state.exchange(NotWaiting, std::memory_order_acq_rel) == Waiting;
}
```

A waker that finds Waiting has woken the CPU by its write, and sends no IPI. The order on the idle side closes every window. A waker before the store reads NotWaiting and sends its IPI, which waits pending for the `sti`. A waker between the store and MONITOR has already written NotWaiting, and the check after MONITOR sees it. A waker after MONITOR writes a watched line, and MWAIT does not sleep or wakes at once. The word says Waiting only inside `idle()`, so a waker that finds it so knows the CPU runs its idle thread, whose loop looks again at its run queue and transition: that is all a reschedule IPI to an idle CPU is for.

One wait per call, like `hlt`: whatever ended it, the idle loop looks again. A version that looped inside `idle()` until a waker's write, as a way to sleep through interrupts that brought no work, was not taken. A thread queued on an idle CPU without a reschedule request (the gap `deactivate()` had until c2960d4f) would then never be seen.

The word is alone on its cache line (`alignas(64)`): MONITOR watches the whole line, and another CPU writing anything else on it would end the wait for nothing. The hint is C1, the state HLT enters. Choosing deeper ones from the CPU's own table is the next step.

Without MONITOR, `idle()` is `sti; hlt` as before, and `wake()` answers false.

**Reference**
Zircon `arch_mp_reschedule()` and `ArchIdlePowerThread::EnterIdleState()` (`zircon/kernel/arch/x86/mp.cc`): the idle CPU writes "idle" to its monitor word and waits in MWAIT while the word still says so, and a CPU rescheduling it exchanges the word and sends no IPI if it said "idle". Zircon also keeps the CPU in its MWAIT loop until a write or a pending preemption; this kernel returns on any wakeup, for the reason above.

**Verification**
- Built in `.worktrees/wip-proc-self` at c2960d4f plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too. clang-tidy reports nothing on the changed lines.
- `boot_check.py`: dbg and rel PASS, host tests 144 passed.
- With the default `-cpu Nehalem`, no MONITOR: `idle.stats` says "idle CPUs wait in hlt", numbers as before (0.9 timer interrupts and halts a second per idle CPU, 99.9% halted). `idle.test`, `preempt.test`, `hotplug.test`, `sleep.test`, `timer.test`, `tickbcast.test`, `tickreplace.test`, `dpc.test`, `semaphore.test`, `waitqueue.test`, `threadjoin.test`, `samplerclaim.test`, `timershare.test`: all ok.
- With `CPU=Nehalem,+monitor`: "idle CPUs wait in mwait", and the same thirteen tests ok. `idle.test`'s new check: 20, 25, 29, 30 and 38 of 100 wakes ended by a write, in five runs. QEMU's TCG runs MWAIT as a PAUSE with more than one CPU, so an idle CPU spins through its loop there: about 735,000 waits a second, 50% of the time inside them, and its word says Waiting only for a sliver of each turn. That is why only a fraction of wakes find it waiting; on hardware every one does. What MWAIT saves cannot be measured under TCG, and neither can a lost wakeup: a PAUSE never sleeps.
- Mutations, each caught by `idle.test` under `+monitor`:
  - `wake()` never waking: 0 of 100 by a write, the new check fails;
  - `idle()` never saying Waiting: the CPU never waits, the halted share falls below half, and the new check fails on 31,543 "writes", since it comes out to a word it never set. The first version of the check accepted any count above zero, and passed this mutation; it now also requires at most two writes per wake.

**Deliberately not done**
- C-states deeper than C1, from CPUID leaf 5 and a governor: the next step.
- States that stop the local APIC timer, on CPUs without ARAT: after that.
- The argument for the wake protocol is in the user's deferred homework (PLAN.md).

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A waker exchanges the word between the idle CPU's store and its MONITOR. Follow both CPUs: why does the idle CPU not sleep through it?
2. Why must the monitor word have its cache line to itself?
3. Why does a waker that finds Waiting need not send an IPI, when an IPI to a busy CPU also sets a pending preemption there?

## The boot CPU's MWAIT idle states, from its model's table cut to what CPUID reports (2026-10-14)

**Files**
- `kernel/arch/x86/lib/idle_states/` — new library, from the `cpp_library` template: `IdleState`, `IdleStates::for_cpu()`, `decode_signature()`, `mwait_cstate()`, the Skylake table, and host tests
- `kernel/arch/x86/kernel/task/idle_thread.cpp` — `find_idle_states()`, at `InitLevel::Platform`, reads CPUID leaves 0, 1 and 5 and logs the states
- `kernel/arch/x86/kernel/BUILD.gn`, `BUILD.gn` — the library, and its tests in `//:default`

**Symptom**
None. Step 6 of `.agents/tasks/idle-power/PLAN.md`, the part before the governor. An idle CPU had one state to wait in, C1, and no way to know whether it had others.

**Cause**
CPUID leaf 5 says which MWAIT hints a CPU takes, as a count of sub-states per C-state, but not what a state costs to leave, and a governor cannot choose without that. Exit latencies are published per processor model, so they come from a table per model; leaf 5 then removes the states a given part does not have.

**Change**
`IdleStates::for_cpu(intel, signature, mwait)` gives an Intel family 6 part with a table its states, deepest first and C1 last, keeping a state only if leaf 5 reports its sub-state:

```cpp
auto const substates = (mwait.edx >> (4 * mwait_cstate(hint))) & 0xf;
return (hint & 0xf) < substates;
```

C1 is kept whatever leaf 5 says, being the architecture's base and the state HLT enters. Any other vendor, model, or a leaf 5 whose ECX bit 0 says EDX is not valid, gets C1 alone. One table so far, Skylake and the client parts built on it (models 0x4e, 0x5e, 0x8e, 0x9e, 0xa5). Only C1 is enabled; a deeper state is entered only once its C-state's bit is set in the mask.

The default `IdleStates` is constant, C1 alone, so a CPU's table is valid from the first instruction; the kernel's copy is `constinit`.

At `InitLevel::Platform` the boot CPU's states are found and logged; every CPU is taken to share them. `idle()` does not use them yet: which state to enter is the governor's, written next.

**Reference**
Zircon `x86_idle_states_t` (`zircon/kernel/arch/x86/include/arch/x86/idle_states.h`) and the per-model `idle_states` in `x86_microarch_config_t` (`zircon/kernel/arch/x86/feature.cc`), whose Skylake table and model list this copies. Every Zircon table enables C1 alone by default (`kX86IdleStateMaskC1Only`).

**Verification**
- Built in `.worktrees/wip-proc-self` at d3ea77c8 plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too.
- Host tests: 9 in `idle_states`, among them QEMU's real leaf values. QEMU's leaf 5 answers ECX 0x3, EDX 0 for every model, read with a probe in the guest on 2026-10-14, so a Skylake table comes down to C1 there. `boot_check.py`: dbg and rel PASS, host tests 145 passed.
- Boot logs on dbg: default Nehalem `idle: HLT, C1`; `Nehalem,+monitor` `idle: MWAIT, family 0x6 model 0x1a, 1 states` (no table); `Skylake-Client,+monitor` `idle: MWAIT, family 0x6 model 0x5e, 1 states` (the Skylake table, cut to C1 by leaf 5).
- Mutation: the sub-state test as `<=` fails `AStateMwaitDoesNotReportIsLeftOut` and `UnderQemuOnlyC1IsLeft`.

**Deliberately not done**
- The governor, and `idle()` entering anything but C1: the next change, written by the user as this milestone's hand-written function.
- Tables for other models; per-CPU tables for hybrid parts.
- States that stop the local APIC timer (leaf 6 ARAT): recorded for later.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the table come from the processor model, when CPUID leaf 5 already lists the states?
2. Which nibble of leaf 5's EDX says whether hint 0x33 is taken, and what must it hold?
3. Why is C1 kept even when leaf 5 lists nothing?
