# loop outstanding repairs

## 1. The Loop's header and README say `ioservice::Outgoing` will use it "later", and it already does

**Severity**
Low: a stale word in two comments. It makes a reader think the provider side of a component is not yet on the Loop.

**Symptom**
`system/lib/loop/include/loop/loop.hpp:16` reads "the root's every wait (...), and later ioservice::Outgoing's", and the first paragraph of `system/lib/loop/README.md` says the same. `ioservice::Outgoing` is on the Loop today (`system/lib/ioservice/include/ioservice/outgoing.hpp` takes a `loop::Loop &`, and `examples/echo/echo_server.cpp` runs it), found on 2026-10-29 while writing `docs/guide/01-concepts.md`.

**Cause**
The sentence was written before `Outgoing` existed (it was the `svc` library until 2026-10-28) and was not revisited when it landed.

**Change**
Say "and `ioservice::Outgoing`'s", or name what else waits on a Loop (the root's `LoopServer` for `/boot` and `/svc`, `ResourceServer`, `LauncherServer`). Do it in the same commit as the next change to either file; the loop-waiter plan (`.agents/tasks/loop-waiter/PLAN.md`) touches both.

**Reference**
None, this project's own comment.

**Acceptance**
`grep -n "later" system/lib/loop/include/loop/loop.hpp system/lib/loop/README.md` no longer finds a "later" that refers to `Outgoing`.
