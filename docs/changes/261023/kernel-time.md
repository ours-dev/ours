# kernel-time changes

## A timer pass arms the device once, at its end, and a restarted timer waits in the queue's list until then (2026-10-23)

**Files**
- `kernel/include/ours/time/timer.hpp` — `Timer::m_parked`, the aliases `ActiveList`, `RestartHook` and `RestartList`, and the class comment on the parked state
- `kernel/include/ours/time/timer_queue.hpp` — `m_restart` (the list of timers waiting to be put back), `unpark_locked()`, and `ClockBase::active` spelled with the new alias
- `kernel/kernel/time/timer_queue.cpp` — a restarting timer is parked, not enqueued; the end of the pass puts them all back before its one `reprogram_locked()`; `init()` builds the list again in each CPU's copy of the queue
- `kernel/kernel/time/timer.cpp` — `cancel()` takes a parked timer out of the list instead of the set
- `kernel/commands/timer_test.cpp` — a case in `timer.test`: a handler cancels a timer that restarted earlier in the same pass

**Symptom**
Not wrong, only wasteful. With the device one-shot, a periodic timer whose handler returns `Restart` made the pass arm the device twice: once when `enqueue_locked()` put the timer back (it became the head, so `reprogram_locked()` ran), and again at the end of the pass, now with the hold-off. The first arming is overwritten at once. In `irqcost.test`'s storm (a 10 µs timer with a 20 µs callback) that was 5 HPET reads and 5 APIC register writes per interrupt, against 3 and 3 for a timer that does not restart (`docs/architecture/timer/oneshot-interrupt-cost.md`).

**Cause**
Two things made the first arming useless. First, the pass is the one writer of the device while it runs: nothing else can arm this CPU's queue from another CPU, and the interrupt for a timer cannot be taken before the pass returns, so a deadline programmed in the middle of it cannot be served earlier than one programmed at the end. Second, the end of the pass always programs the device (`reprogram_locked()` after the hold-off is set), so whatever the middle did is replaced.

The first arming also did a little harm, which I only saw in the measurement. A one-shot timer that has expired while the CPU is in its handler leaves an interrupt pending, and writing the initial count again does not take it back. The in-pass arming was for a deadline about 10 µs ahead, so on some passes it ran out before the end of the pass, and the interrupt was taken the moment interrupts came back on, with the hold-off of the second arming not in force. In the "before" runs the delivery latency of the storm's interrupts was 0 µs at the 10th percentile and 1250 µs at the 90th, with a mean of 840 to 865 µs, which for two values is about a third of them delivered with no delay beyond what was asked; in the "after" runs the 10th percentile is 1237 to 1240 µs and the smallest value 1001 to 1015 µs (0 once, in one run). That is consistent with the explanation and does not prove it: I did not count the interrupts that ran nothing.

The way it was done also had a hazard of its own, found by reading before building. The first draft kept the restarting timers in a list local to the pass, pushed with `to_restart.push_back()` and put back after the loop. A timer in that list is `is_linked()` (the hook is linked, into the list), its `m_active_cpu` is cleared as soon as the lock is dropped, and `m_queue` still names the queue. `Timer::cancel()` asks only `is_linked()` and then `remove_locked()`, which erases the node from the **set**: a list node taken out of a tree, from another CPU's `cancel()` or from a later handler in the same pass. That corrupts both structures. And even if it did not, `cancel()` would return and the caller, whose `Timer` lives on its stack, would free a node the pass still points at.

**Change**
The list belongs to the queue and the timer says which container it is in.

- `TimerQueue::m_restart`, `Timer::RestartList` guarded by `m_lock`, and `Timer::m_parked`, a plain `bool` that is read and written only under the queue's lock. It is the one fact `is_linked()` cannot give, now that the same hook can be in a set or a list.
- Before, the end of the restart branch: `enqueue_locked(*timer);`. After:

```cpp
ASSERT(!timer->is_linked(), "Restarted timer re-armed itself as well");
m_restart.push_back(*timer);
timer->m_parked = true;
```

- The end of the pass, in the lock hold that already sets the hold-off and reprograms:

```cpp
while (!m_restart.empty()) {
    auto &timer = m_restart.front().value();
    m_restart.pop_front();
    timer.m_parked = false;
    auto const [iter, inserted] = base_of(timer.clockid()).active.insert(timer);
    DEBUG_ASSERT(inserted);
}
reprogram_locked();
```

- `Timer::cancel()`, in the lambda that runs under the queue's lock: `if (m_parked) { queue.unpark_locked(*this); } else if (is_linked()) { queue.remove_locked(*this); handler_not_running = true; }`. A parked timer's handler has run, so the answer stays "not stopped before it ran".
- Why it holds: which container the timer is in changes only under `m_lock` (park, put back, unpark), and `cancel()` stores `m_cancelled` before it takes that lock. The park tests `!is_cancelled()` under the lock. So a `cancel()` comes before the park (the timer never parks), or between the park and the put-back (it finds the timer parked and takes it out), or after (it is in the set and is removed as before). There is no window where the timer is in a container the canceller does not know.
- **The per-CPU copy.** The first boot with this change took a page fault in the put-back loop. A CPU's `TimerQueue` is a copy of a template the constructor ran on, and a list's header points at itself: in the copy it points at the template's header, so `empty()` is false and the first `front()` reads the template's links. The sets in `m_bases` survive the copy because an empty one has no pointer to itself. `TimerQueue::init(cpu)` now builds `m_restart` again in place (`new (&m_restart) Timer::RestartList()`), without destroying the copied one, which has nothing of its own. `DpcRunner::init()` says the same of its queues.
- Rejected: leaving `m_active_cpu` set until the put-back, so that a canceller waits for the pass. The canceller would then spin until the whole pass ends, and a handler later in the pass that is in `trylock_or_cancel()` on a lock the canceller holds can only see its **own** timer cancelled, so each waits for the other for ever. Rejected: marking a parked timer by setting `m_queue` to null, so that `with_queue_locked()` skips it. That read has no lock, so it would race with the put-back through a store followed by a load on each side (the store buffer lets both miss the other's store); the bit under the lock has no such window.
- Not chosen, and offered: leave the timers going straight into the set and only keep the programming out of the pass (a flag the middle of the pass tests in `reprogram_locked()`). It is about six lines, needs no state in `Timer` and no change to `cancel()`, and saves the same arming. The user chose the list, which also makes a timer that forwards by less than the pass's own `now` wait for the next pass instead of running again in this one.

**Reference**
Linux `__run_hrtimer()` (`kernel/time/hrtimer.c`) puts a restarted timer back with `enqueue_hrtimer(timer, base, HRTIMER_MODE_ABS, false)`, the last argument meaning no reprogramming, and says why: "we do not reprogram the event hardware. Happens either in hrtimer_start_range_ns() or in hrtimer_interrupt()". `hrtimer_interrupt()` programs the device once, after `__hrtimer_run_queues()`, in `hrtimer_interrupt_rearm()`. That is this change's goal. Linux gets there without parking: the restarted timer goes straight into the tree under the base lock, which is the option not chosen above. Zircon's timers are one-shot and have no restart; its `TimerQueue::Tick()` ends with one `UpdatePlatformTimer()` after `TickInternal()` has run the due timers, which is the same single arming at the end of a pass.

**Verification**
- Mutation check: with the `m_parked` branch taken out of `cancel()` (the first draft's behaviour) the new `timer.test` case takes a kernel page fault in `cancel()`; with it, the case passes. The hold-off check printed a failure in that run too, see the flake below.
- `timer.test`, new case, 17 checks ok on the default build and on a `time_oneshot=false` build: B's handler cancels A in the same pass in which A restarted; A ran once and never again over 30 ms; the cancel said A's handler had run; a timer armed afterwards still fired.
- `ours-verify` on the default build: dbg PASS (39 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (37 passed, 8 skipped, 0 failed, 6/6 required), host tests PASS (146). On `time_oneshot=false` both images PASS as well.
- `preempt.test`, `sleep.test`, `semaphore.test`, `waitqueue.test`, `tickreplace.test`, `tickbcast.test`, `tickfallback.test`, `timershare.test`, `timershare.test big` (the thread never kept under 45 %) and `mutex.stress 2000`: ok on both builds (`big` on the default build only).
- Before and after, `irqcost.test` on the instrumented tree (`exp/irq-cost` stamps cherry-picked onto the baseline, then this change on top), three runs of each, the storm on CPU 3:

| Storm, per interrupt | before (3 runs) | after (3 runs) |
|---|---|---|
| HPET reads | 5 | 4 |
| APIC register writes | 5 | 3 |
| whole handler, mean, µs | 35.3 to 35.6 | 31.2 to 33.1 |
| whole handler, median, µs | 34.0 to 34.9 | 30.0 to 30.9 |
| "callbacks" stage, mean, µs | 26.6 to 26.8 | 22.6 to 23.3 |
| delivery latency, mean, µs | 839 to 865 | 1227 to 1245 |
| `single` workload (no restart) | 3 reads, 3 writes, 6 µs | the same |

- The 4 reads and not 3: the storm's callback calls `forward(SteadyClock::now(), ...)`, which is a read of its own. My prediction of 3 and 3 was wrong about the reads; the arithmetic is the first arming's one read and two writes, which is exactly what went.
- The saving is in the "callbacks" stage because the in-pass arming ran inside it (the restart is handled between the callback and the next pop), which is why the earlier document's callbacks stage was 26 µs for a 20 µs callback. The stage "whole handler less callbacks", which I had used for the kernel's own cost, stays at 8.7 µs: the pass's closing clock reads got cheaper (end-clock 1.5 to 0.7, `program_event()`'s 1.3 to 0.4) and the count write got dearer (1.5 to 3.1 µs). Why the write got dearer is not established; with a 1 µs TSC step and three runs it is not a finding. The figure to quote is the whole handler, about 4 µs (11 %) less in the storm.
- `timershare.test big` on the baseline and on this change, two runs each: the thread's median share and the gap between callbacks are the same within the run-to-run spread (for example C = 500 µs: 69.0 % and 69.5 % before, 67.2 % and 69.1 % after; C = 4 ms: 51.1 % and 51.4 % before, 50.8 % and 50.9 % after). The delivery grid of the emulator (1.25 ms) is still what a user of the timers sees.
- `scripts/format_cpp.py --check` on the five C++ files: clean. `clang-tidy -p out/bringup.x86` on `timer.cpp`, `timer_queue.cpp` (which include both headers) and `timer_test.cpp`: no diagnostic located in any of the touched files (the output is other headers' old warnings).
- A flake that is not this change's: `timer.test`'s hold-off check ("and it came no sooner than about the pass's own length after the pass") failed in 1 of 6 runs on the untouched baseline (a gap of 20 µs) and in 4 of 10 runs with this change on the default build (gaps of 21 µs, 21 µs and 1527 µs where the log was read). The sample is too small to tell the two rates apart. A likely cause, not verified, is the end of the thread's slice: the preemption deadline is exempt from the hold-off by design, so an interrupt for it right after A's pass also runs B. It is recorded as item 8 in `docs/repairs/261010/kernel-time.md`.
- Not verified: cancelling a parked timer from **another** CPU. The case in `timer.test` is the same-CPU one, which is the deterministic one; the other CPU takes the same lock and the same branch, and `mutex.stress` and `timershare.test` exercise cancels across CPUs, but no test arranges a cancel to land between a park and the put-back of a pass on another CPU.

**Deliberately not done**
- The other candidates of repair item 4 (handing the pass's closing clock reading to `program_event()`, skipping an unchanged divisor write, a TSC clock source): the user has not chosen them.
- Timers armed by a handler of the same pass still reprogram the device when they become the head. Only restarts are parked; `activate_timer()` from a handler is a different path, and it is not the periodic case.
- Fixing the hold-off check's flake, since it is a separate cause and a separate change.

**How it was found**
The double arming was read off the stamps of `irqcost.test` (5 and 5 against 3 and 3). The hazard of the local list was found by following `cancel()` through `with_queue_locked()` to `remove_locked()` with the draft in front of me, before building: the question was what a canceller sees of a timer that is linked and whose `m_active_cpu` is already clear. The empty-list fault came out at the first boot (`Unhandled page fault ... cr2=0x0`), disassembled at `+0x39f00`, which was the `front()` of the put-back loop, reading a header that pointed at the template.

**The user's prediction**
Not given (homework paused). Mine, against the result: the saving would be one read and two writes, and I wrote it as "3 reads and 3 writes in the storm". The two writes were right; the reads came to 4 because of the callback's own read.

**Three questions you should be able to answer**
1. Why did the draft's local list let `cancel()` corrupt the set, and what makes the queue's list with a bit under the lock safe where a wait for the pass or a null `m_queue` was not?
2. Why does a list member of a per-CPU object have to be built again in `init()`, and why do the sets next to it not?
3. A one-shot timer expires while its CPU is in the handler and the handler then writes a later count: which interrupt does the CPU take when it re-enables interrupts, and what did that do to the hold-off before this change?

## The list of timers to restart is a singly linked list (2026-10-23)

**Files**
- `kernel/include/ours/time/timer.hpp` — `RestartHook` and `RestartList` name `AnyToSlistHook` and `Slist` instead of the list pair
- `kernel/kernel/time/timer_queue.cpp` — a restarting timer is pushed at the front, the put-back pops from the front, and `unpark_locked()` erases through `iterator_to()`, which walks from the front
- `kernel/include/ours/time/timer_queue.hpp` — the comments of `m_restart` and `unpark_locked()`
- `kernel/commands/timer_test.cpp` — a third timer R in the `timer.test` case, parked between A and B, so that the cancelled A is the node at the far end of the list
- `system/lib/ustl/test/set_any_hook_test.cpp` — the same any hook in an `Slist` (see `docs/changes/261023/ustl.md`)

**Symptom**
None: an instruction, "sort with the Set, restart with an Slist". The previous change kept the parked timers in a doubly linked `List`.

**Cause**
The two containers have two jobs, and only one of them is ordered. The set of armed timers keeps the deadline order, because the head must be the next to expire. The parked timers have no order: they are pushed as handlers return, and all of them go into the sets together at the end of the pass, which sorts them. A doubly linked list pays for a way back that nothing on the fast path uses (the pass pushes and pops at one end), and says more than the code needs.

Said without flattery, what it buys is the statement of intent and not space or time on the fast path. The any hook is one node of the same 32 bytes whichever container it sits in, so `sizeof(Timer)` does not change, and push and pop at the front cost the same in both. What it costs is on the cold path: a singly linked list cannot unlink a node from the middle without its predecessor, so the `cancel()` of a parked timer walks the list from the front, under the queue's lock with interrupts off, in time linear in the number of timers that restarted in the pass so far. The doubly linked list did that in constant time, as Linux's timer wheel does on purpose (`detach_timer()` calls `__hlist_del()` on an `hlist_node`, which carries a pointer to the pointer that points at it) and as Zircon's timer lists do (`fbl::DoublyLinkedList<Timer*>`). The number is small (the timers of one pass that returned `Restart`: the tick, the sampler, the heartbeat), the walk happens only for a `cancel()` of a timer in that window, and it was not measured.

**Change**
- `timer.hpp`: `AnyToSlistHook<MemberHook<...>>` and `Slist<Self, RestartHook>`.
- Park: `m_restart.push_front(*timer)`. Put back: `front().value()`, then `pop_front()`, as before but from the front of a list that keeps the last one pushed first; the order does not matter, the sets sort.
- Unpark: `m_restart.erase(m_restart.iterator_to(timer))`, which is `erase_after(previous(position))` and so the walk. The caller has the lock and `m_parked` says the node is in this list, so the walk always finds it.
- **Not `Linear<true>`.** A linear singly linked list ends in a null link and has a header that refers to nothing, so an empty one is all zeros and would have survived the copy of the per-CPU queue with no `init()` rebuild. I tried it, and the `ustl` test failed on the first push: the safe-link any hook tells an unlinked node by a null `next`, and the last node of a linear list has exactly that, so `is_linked()` answers false for it. The assertions that trust `is_linked()` (`activate_timer()`, the destructor, `Slist::insert_after()`'s own `unique()` check) would not see a parked last node. The circular `Slist` keeps every node's `is_linked()` exact and costs the rebuild in `init()`, which stays.

**Reference**
Linux `detach_timer()` and Zircon `TimerQueue::TickInternal()`'s `fbl::DoublyLinkedList<Timer*>` above are the doubly linked counterparts, which this departs from for the reason given. The collection of the expired timers in Linux (`collect_expired_timers()` moving whole buckets with `hlist_move_list()` into a local array of heads) is the nearest thing to a list of timers waiting to be run that has no order, and it is a head-only structure too.

**Verification**
- Mutation check of the new case: with `unpark_locked()` popping the front (R) in place of A, the case passes its first three checks and then the kernel panics with `Assertion failed on m_on_expired`, because A, still parked, went back into its set and ran with the handler `cancel()` had cleared. With the erase it passes: 18 checks ok on the default build.
- `ours-verify` on the default build: dbg PASS (39 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (37 passed, 8 skipped, 0 failed, 6/6 required), host tests PASS (146, `set-any-hook-test` among them). On `time_oneshot=false` both images PASS.
- `timer.test`, `preempt.test`, `sleep.test`, `semaphore.test`, `waitqueue.test`, `tickreplace.test`, `tickbcast.test`, `tickfallback.test`, `timershare.test`, `mutex.stress 2000` ok on both builds; `timershare.test big` ok on the default build.
- `scripts/format_cpp.py --check` clean; `clang-tidy -p out/bringup.x86` on `timer.cpp`, `timer_queue.cpp` and `timer_test.cpp`: no diagnostic located in the touched files.
- Not measured: the cost of the walk, and the interrupts-off time of a `cancel()` of a parked timer. Not verified: a parked timer cancelled from another CPU (the case is the same-CPU one, as before).

**Deliberately not done**
- A `CacheLast` option, which would make `push_back()` constant time: nothing pushes at the back.
- Going back to a doubly linked list to make the cancel constant time. That is the better engineering on the cold path and the user chose the singly linked one; it is one alias to revert.

**How it was found**
None: an instruction. The linear list's last-node problem came out of the new `ustl` test returning at the push, with the three `is_linked()` answers split into separate checks to see which one was false.

**The user's prediction**
Not given (homework paused).

**Three questions you should be able to answer**
1. What does a singly linked list give up compared with a doubly linked one, and which operation of the restart list pays for it?
2. Why is a linear singly linked list a bad match for a safe-link hook, even though its empty state is all zeros?
3. Why does the put-back not care in which order the parked timers come out of the list?
