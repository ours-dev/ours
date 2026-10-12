# init changes

## init counts its four resource servers, not three, when it sizes its Loop (2026-10-12)

**Files**
- `system/service/init/pools.hpp` — `RESOURCE_SERVERS` is 4, and its comment names the four.

**Symptom**
`init.cpp` builds a `ResourceServer` for each of Debug, Irq, Ioport and Sampling, but `RESOURCE_SERVERS` said 3, so `LOOP_SLOTS` fell short by one server's connections (`MAX_RESOURCE_CONNECTIONS`, 4 slots). With every connection in use at once, the Loop would refuse a wait it was sized to take. Not seen in a boot: the shell and routing_checks open one resource connection at a time.

**Cause**
The Sampling server was added to `init.cpp` (the SAMPLING resource served from `/svc`, 2026-10-07) without the count beside the Loop's size following it; nothing ties the two together, and the comment still listed three protocols.

**Change**
`RESOURCE_SERVERS` 3 -> 4, and the comment names Debug, Irq, Ioport and Sampling and says that a server added in `init.cpp` must be counted here. `LOOP_SLOTS` grows by 4 and stays within `PORT_MAX_OBSERVERS` (the `static_assert` beside it holds).

**Reference**
None, this project's own sizing (D27 of the component framework's design).

**Verification**
In `.worktrees/wip-proc-self` at baseline `93c802f5` plus only this change: build ok, dbg PASS (35 passed), rel PASS (33 passed, 6/6 required), host tests 144 passed.

**Deliberately not done**
Deriving the count from the servers `init.cpp` builds, which would need them in one table; the next server (the root job protocol) is about to be added and will be counted with it.

**How it was found**
A read-only survey of init's protocol serving, for the root job protocol the system-wide profiler needs (2026-10-12).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What does a Loop slot hold, and why does each resource connection cost one?
2. What would a client see if the Loop ran out of slots?
3. Why did adding the Sampling server not fail the build?
