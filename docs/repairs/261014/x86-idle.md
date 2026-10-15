# x86-idle outstanding repairs

## 1. A deep idle state that stops the local APIC timer would leave the CPU's deadline unwatched

**Severity**
Medium: latent, cannot be triggered today. Every CPU enables C1 alone, and C1 keeps the timer running.

**Symptom**
Latent. Once a state deeper than C1 is enabled on a CPU whose local APIC timer stops in it, a CPU waiting there misses its own timer interrupt. It wakes only for another interrupt or a waker's write, and its timers, preemption deadline and lockup heartbeat are late by however long that takes.

**Cause**
On parts without ARAT (CPUID leaf 6, EAX bit 2, "APIC timer always running"), the local APIC timer stops in states deeper than C1. `ArchIdleThread::idle()` enters whatever `IdleStates::pick()` returns, and nothing looks at leaf 6 or hands the CPU's deadline to a device that keeps running.

**Change**
Before entering a state deeper than C1 on a CPU without ARAT, hand the CPU's deadline to the existing `TickBroadcast` group of a global device, the HPET, and take it back on waking. Alternatively, mark such states unusable on a CPU without ARAT. Read leaf 6 in `ArchIdleThread::find_states()` either way.

**Reference**
Zircon defines the bit (`arat` in `zircon/kernel/lib/arch/include/lib/arch/x86/cpuid.h`) but its idle code does not consult it; it leaves deep states disabled by default instead (`kX86IdleStateMaskC1Only`, `zircon/kernel/arch/x86/feature.cc`). Linux hands the deadline to the broadcast device around such states (`tick_broadcast_enter()`, `kernel/time/tick-broadcast.c`, called from `cpuidle_enter_state()` for states marked `CPUIDLE_FLAG_TIMER_STOP`).

**Acceptance**
With a deep state enabled on a CPU reported without ARAT, `timer.test` and `preempt.test` pass and `lockup status` shows the CPU beating; QEMU's Skylake-Client reports ARAT, so the test has to clear the bit or fake the state.
