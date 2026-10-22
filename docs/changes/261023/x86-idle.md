# x86-idle changes

## A CPU without ARAT keeps C1 alone, so no idle state stops its own timer (2026-10-23)

**Files**
- `kernel/arch/x86/lib/idle_states/include/idle_states/idle_states.hpp`, `src/idle_states.cpp` — `Processor`, what CPUID says about a part, `arat` among it; `IdleStates::for_cpu(Processor const &)` gives a part without ARAT C1 alone
- `kernel/arch/x86/lib/idle_states/test/idle_states_test.cpp`, `test/pick_test.cpp` — `WithoutAratOnlyC1IsLeft`; the calls follow
- `kernel/arch/x86/kernel/task/idle_thread.cpp` — `find_states()` reads CPUID leaf 6
- `docs/repairs/261014/x86-idle.md` — item 1 fixed, the file removed with it

**Symptom**
Latent, cannot be triggered today. Every CPU enables C1 alone. Once a deeper state was enabled on a part without ARAT, a CPU waiting there would sleep through its own deadline: its local APIC timer stops in any state deeper than C1, so its timers, preemption deadline and lockup heartbeat would wait for some other interrupt.

**Cause**
`for_cpu()` built the states from the model's table and leaf 5 alone. Leaf 5 says which hints MWAIT takes, not what a state does to the timer; that is leaf 6, EAX bit 2, ARAT ("APIC timer always running"), which nothing read.

**Change**
`for_cpu()` takes a `Processor`, the four facts from CPUID it uses, rather than three loose arguments to which a second `bool` would have been added beside the first:

```cpp
if (!processor.intel || !processor.arat) {
    return result;   // C1 alone
}
```

Of the two ways the repair item gave, this is the smaller: a part without ARAT never has a state that stops its timer. The other, handing the CPU's deadline to the HPET's `TickBroadcast` group around a deep state and taking it back on waking, would let such a part sleep deeper. It is not built: nothing here can test it, QEMU's Skylake-Client reporting ARAT and its leaf 5 no deep states.

**Reference**
Zircon defines the bit (`arat` in `zircon/kernel/lib/arch/include/lib/arch/x86/cpuid.h`) and leaves deep states disabled by default instead of consulting it. Linux marks such states `CPUIDLE_FLAG_TIMER_STOP` and hands the deadline to the broadcast device around them (`tick_broadcast_enter()`).

**Verification**
- Built in `.worktrees/wip-proc-self` at be90653e plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too.
- Host tests: 22 in `idle_states`. `boot_check.py`: dbg and rel PASS, host tests 146 passed.
- `idle.test`, `preempt.test` and `hotplug.test` ok under `CPU=Nehalem,+monitor` (leaf 6 EAX 0, no ARAT) and `Skylake-Client,+monitor` (EAX 0x4, ARAT).
- Mutation: ARAT ignored fails `WithoutAratOnlyC1IsLeft`.

**Deliberately not done**
The broadcast hand-off above.

**How it was found**
Recorded while building idle-power step 6 (`docs/repairs/261014/x86-idle.md`).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Which CPUID leaf says whether MWAIT takes a hint, and which says whether the timer survives the state it names?
2. What would a CPU without ARAT miss, waiting in C6, and what would eventually wake it?
3. Why is "C1 alone" safe here when it gives up every saving a deeper state would bring?
