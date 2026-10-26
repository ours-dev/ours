# kernel-task changes

## The hotplug and idle-power code keeps the repo's style: no ternaries, `auto _`, an `enum class` (2026-10-26)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp` — `auto _ =` for the request `take_pending_local()` answers
- `kernel/kernel/task/cpu_hotplug.cpp` — `[[maybe_unused]]` on `take_cpu_offline()`'s `deadline`, for the build without CPU hotplug
- `kernel/arch/x86/kernel/task/idle_thread.cpp` — `enum class MonitorState`, the word an `std::atomic<MonitorState>`; two ternaries
- `kernel/kernel/task/idle_thread.cpp`, `kernel/include/ours/task/sched/sched_entity.hpp`, `kernel/arch/x86/lib/idle_states/src/idle_states.cpp` and its test, `kernel/commands/idle.cpp`, `preempt_test.cpp`, `hotplug_test.cpp`, `benches/kernel/time/preemption_deadline_bench.cpp` — the ternaries, as `if`, `std::min` or `std::max`; the bench's `void` cast as `[[maybe_unused]]`

**Symptom**
None. Review finding "Style" of `.agents/tasks/idle-power/REVIEW.md`: this work's own lines held 19 ternaries, three values discarded with `static_cast<void>`, and an unscoped enum, each against `modern-cpp-development`.

**Cause**
The rules were not applied while the code was written, and clang-tidy enforces none of the three.

**Change**
Each ternary became an `if`, or `std::min` or `std::max` where it picked the smaller or larger of two values. The `[[nodiscard]]` result of `take_pending_local()` is discarded with `auto _ =` and a comment saying why it does not matter. The unused parameter and the write-only local carry `[[maybe_unused]]`. `MonitorState` is an `enum class` over `u32`, held in an `std::atomic<MonitorState>`, so the comparisons name the state.

**Reference**
None, style.

**Verification**
- Built in `.worktrees/wip-proc-self` at f7825b7c plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too.
- `boot_check.py`: dbg and rel PASS, host tests 147 passed.
- Default CPU: `preempt.test`, `hotplug.test`, `idle.test` ok; `preemptdeadline.bench` 1.00 deadline sets per switch. `Skylake-Client,+monitor`: `idle.test` ok, 27 of 100 wakes ended by a write; `hotplug.test` ok. Without CPU hotplug, `hotplug.test` ok.
- Under `Skylake-Client,+monitor`, `preempt.test` failed once, right after `idle.test`, on the probe timer's 2 ms bound. In three runs each with and without this change, the probe was 1.06 to 1.33 ms late both ways. Under TCG with `+monitor` every idle vCPU spins (MWAIT is a PAUSE), so the margin is thin there; the change is not the cause.

**Deliberately not done**
Other files' ternaries, such as `kernel/kernel/task/sched/scheduler.cpp:974`, which this work did not write.

**How it was found**
The architecture review of 2026-10-26.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the style ask for `auto _ =` rather than `static_cast<void>` to discard a `[[nodiscard]]` result?
2. What does `enum class MonitorState` catch that the unscoped enum did not?
3. Why can `std::atomic<MonitorState>` take the place of `std::atomic<u32>` for MONITOR?

## A hotplug transition that misses its deadline is withdrawn, and a CPU that arrives late halts again (2026-10-26)

**Files**
- `kernel/include/ours/task/idle_thread.hpp`, `kernel/kernel/task/idle_thread.cpp` — `IdleThread::State::Leaving`, `IdleThread::claim()`, the withdrawal in `transition()`, the claims in `run()` and `come_back()`, `m_halted` cleared by `transition()`
- `kernel/kernel/task/cpu_hotplug.cpp`, `kernel/include/ours/task/cpu_hotplug.hpp` — `take_cpu_offline()` bounds the transition by its deadline
- `kernel/commands/hotplug_test.cpp` — `check_offline_withdrawn()`, `check_start_withdrawn()`, and `check_runs_there()` factored out of `round_trip()`
- `kernel/commands/cpu.cpp` — `cpu status` names the new state

**Symptom**
Review findings M1 and M3 of `.agents/tasks/idle-power/REVIEW.md`, now reproduced by the two new cases of `hotplug.test`:
- M1: a CPU whose start timed out was left at `{current: Offline, target: Running}`. A retry got `BadState` from `transition()` (current differs from target), and `take_cpu_offline()` got `BadState` too (the CPU is not online), so nothing could ever use the CPU again. If it arrived late instead, it came online behind the caller's back with its Dpc workers stopped, and a Dpc queued there never ran.
- M3: `take_cpu_offline()` waited for the transition with `Deadline::infinite()`. A thread holding the CPU with preemption off kept the idle thread from running, and the caller blocked for ever inside `HOTPLUG_LOCK`, and with it every later hotplug and every sampler session start, stop and release.

**Cause**
`transition()` wrote the target and waited for the CPU to reach it, and on a timeout it simply returned, leaving the request in place. A request left in place after the asker has gone means two parties each act on a different belief: the caller believes the CPU did not move, while the CPU, when it gets there, believes it was asked to. Whichever of them is right depends on timing nobody checks. A bounded wait needs a way to take the request back that the other side cannot race. Bounding the wait alone, as M3 suggested, would have made M3 into M1 for the offline direction: a timed-out caller, and a CPU that goes offline afterwards on its own.

**Change**
One compare-and-swap on the state decides between the two sides:
- The CPU claims a transition before it touches anything, moving the state from the one asked for to an in-between state: `{Running, Offline}` to `{Leaving, Offline}` in `run()` before `go_offline_and_halt()`, and `{Offline, Running}` to `{Booting, Running}` first thing in `come_back()`. `Leaving` is new; `Booting` already meant "on the way to Running, not yet in the idle loop", which a CPU coming back is.
- At its deadline, `transition()` tries to move the state from the one asked for back to where it was. If that succeeds, the CPU had not claimed, and `TimeOut` means the CPU is as it was. If it fails, the CPU is already on its way, past turning back, and the caller waits for it without a deadline: what remains is the CPU's own work with interrupts off, and if that hangs, the lockup detector is the right tool.
- A CPU that loses, one that arrives after its start was withdrawn, halts again at once in `come_back()`, taking no lock on the way, and raises `m_halted`. The next start resets it with an INIT. `m_halted` is now cleared by `transition()` before it starts the CPU, not by `come_back()`, so that it means "halted since it was last started" and the test can see the late CPU halt.
- `take_cpu_offline()` passes its deadline to the transition. On a `TimeOut` it starts the Dpc workers again, as it already did for a refusal.

This departs from the review's proposed fix in two places. The review proposed resetting a timed-out CPU with an INIT at once. That INIT could catch a late CPU in its bring-up holding a lock, so here the late CPU halts itself and the INIT waits for the next start. The review also proposed starting the Dpc workers from the CPU's side; with the claim, a late CPU can no longer come online unseen, so `bring_cpu_online()` keeps starting them after a successful wait.

The spin on `has_halted()` after a successful transition stays unbounded: after the CPU has signalled, all that is left is a cache write-back and a store.

What is left over:
- A Dpc workers' stop that times out still leaves the workers that did stop stopped on an online CPU. That is the next change.
- The retry's INIT can still catch a CPU that is more than a second late and still in the few microseconds of its arch bring-up. Linux accepts the same window for a CPU in `SYNC_STATE_KICKED`.

**Reference**
Zircon's `x86_bringup_aps()` (`zircon/kernel/arch/x86/smp.cc`) and `finish_secondary_entry()` (`zircon/kernel/arch/x86/arch.cc`) make the same race with one atomic: the controller withdraws every AP that has not reported with `aps_still_booting.exchange(0)`, an AP reports with `fetch_and(~mask)`, and one that finds the word already zero has timed out and halts (`goto fail`). Zircon also sends the failed AP an INIT at once, which this change leaves to the next start. Linux's hotplug core does it with a state word per CPU (`cpuhp_bp_sync_alive()`, `cpuhp_can_boot_ap()`, `kernel/cpu.c`): a CPU still `KICKED` or `ALIVE` after a timeout may be booted again, while one in any other state is in limbo and is refused. On the offline side this departs from Zircon. `mp_unplug_cpu_mask_single_locked()` (`zircon/kernel/kernel/mp.cc`) does bound the wait, passing its deadline to `TransitionActiveToOffline()`, but `IdlePowerThread::TransitionFromTo()` (`zircon/kernel/kernel/idle_power_thread.cc`) returns on a timeout with the transitional state `{current, target}` left in place: the shape this change removes, where the CPU still goes offline after the caller has given up. Linux's `takedown_cpu()` waits for the dying CPU without a deadline.

**Verification**
- Built in `.worktrees/wip-proc-self` at 5376645c plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too. `boot_check.py`: dbg and rel PASS, host tests 147 passed.
- `hotplug.test` ok, 47 checks, in three boots. `preempt.test`, `idle.test`, `timer.test` ok; `tickreplace.test` ok six times after `hotplug.test`, and once not, which is a flake in its own sixth case, fixed in the next change.
- The start-withdrawn case is not made under QEMU as built: `arch_activate_cpu()` spins 200 us between its two startup IPIs, so the CPU has always claimed its return before the withdrawal is tried, and the test says "that case is not made". It was made by a temporary 20 ms spin before `come_back()` in `x86_start_nonboot_cpu()`, not committed: 53 checks pass, among them "and, arriving late, halts again", "and stays offline" and "a second start brings it back".
- Mutations, with that spin in place: without the claim in `come_back()`, the late CPU comes online and the three checks above fail; without the withdrawal in `transition()`, "the transition is withdrawn" fails, the CPU goes offline once the blocker lets go, with the blocker pinned to it, and the kernel panics.
- clang-tidy: nothing new; the two naming warnings at `idle_thread.cpp:156` and `:161` are older.

**Deliberately not done**
- A `bring_cpu_online()` deadline parameter, which would let the test make the start-withdrawn case without a temporary spin: an API change for a test.
- An INIT at the timeout, as above.

**How it was found**
The architecture review of 2026-10-26, findings M1 and M3.

**The user's prediction**
None, homework paused.

**Three questions you should be able to answer**
1. Why is a timed-out wait that leaves its request in place worse than an unbounded one, and what has to happen at the deadline instead?
2. Why must the CPU's claim come before it touches anything, and what goes wrong if `come_back()` claims after `run_online_hooks()`?
3. Why does the caller that loses the race wait without a deadline, and why is that not M3 again?
