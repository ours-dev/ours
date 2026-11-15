# mm outstanding repairs

## 1. A waiting request put off by the gate forgets the node it asked for

**Severity**
Low: latent. No caller combines `Gaf::CanWait` with `Gaf::ThisNode`, an explicit home or a node set; every `ALLOC_USER_VMO` caller names no node.

**Symptom**
`alloc_frames(ALLOC_USER_VMO | ThisNode, 0, N)` from a CPU on node M, with the free-memory gate shut, answers `ShouldWait`. The caller's `AnonymousPageRequest::allocate()` waits in `wait_for_frame_allocation(flags, ...)`, which retries `alloc_frames(flags)` with no home: `ThisNode` then means the waking CPU's node, M, and the frame comes from M, or the retry answers `OutOfMem` because M is short while N had frames.

**Cause**
The wait carries only the flags (`AnonymousPageRequest::make_active()`), not the home or the node set the first call named. It was harmless while every waiting request named no node.

**Change**
Carry the home with the waiting request: `make_active(flags, home)` and `wait_for_frame_allocation(flags, home, deadline, interruptible)`, retrying `alloc_frames(flags, 0, home)`. The wait itself is bounded: a confined request is not put off at a zone's Low (`AllocRequest::is_confined()`), so only the machine-wide gate can make it wait, and the gate opens or refuses for every waiter.

**Reference**
None: Zircon has no node-confined allocation, and Linux does not wait for memory on a gate.

**Acceptance**
A kernel test on the four-node machine: shut the gate by hand, ask with `ALLOC_USER_VMO | ThisNode` and a home other than the calling CPU's node, open the gate, and see the retried frame come from the named home.

## 2. A node set narrowed without Gaf::ThisNode is not treated as confined

**Severity**
Low: nothing can make such a request with `Gaf::CanWait` today. `apply_thread_policy()` is a no-op, and the only caller of `mm::priv::alloc_frames()` outside tests (`alloc_page_table()`) does not wait.

**Symptom**
A future placement policy that binds a thread to node N would narrow its requests' node set without setting `ThisNode`. Its VMO pages are then put off at N's zones' Low, and if N has nothing to reclaim while the machine's count stays above the gate's delay level, only a release on N can end the wait: the hang the confinement rule closed for `ThisNode`.

**Cause**
`AllocRequest::is_confined()` looks at the flag and the zone range only, not at the node set, because the node set is also how a test (`PmNode.WaitingRequestsStopAtLowAndWakeOnRelief`) stands in for a smaller machine through `mm::priv::alloc_frames()`.

**Change**
When a policy that narrows the node set lands, have it narrow the request rather than the set the pipeline treats as the machine, or make `is_confined()` compare the request's nodes with the nodes that have memory and give the test another way to stand in for a one-node machine.

**Reference**
None, this project's own rule.

**Acceptance**
A test with a bound thread and a drained node: its waiting request goes on to Min and answers `OutOfMem` below it, and is never answered `ShouldWait` at Low.
