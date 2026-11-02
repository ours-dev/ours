# ioservice outstanding repairs

## 1. A Subtree handler that serves a tree of its own forwards the Open through a connection it must make first

**Severity**
Low: a cost, not a wrong result. Every Open through `examples/files` is correct; it spends one extra connection, one channel pair and one Loop turn, and the program carries a second server's buffers.

**Symptom**
`examples/files` serves `/export/docs` through a Subtree handler that opens a tree of its own. The handler cannot run the core's Open checks (valid path, TRAVERSE, rights a subset of the connection's, no creation flag, the walk) because they are private to `Server::open`. So it keeps a root connection of its own tree and forwards each Open to it. Measured from the `files_server` ELF by the step 6 note: `.bss` grew from 18,496 to 34,496 bytes, of which the second tree's `DOCS_STORAGE` is 15,176 (32 connections and bindings, its message buffers and 4,096 bytes for the forwarded Open); and the Loop table from 32 to 64 slots. The step 6 note counts the forwarding as one connection, one channel pair, one Loop slot and a 4,096-byte buffer, plus one Loop turn on each Open.

**Cause**
The Subtree node (directory-capability design, decision 7 A) hands the Open and the rest of the path to the program, and the design left the order of the core's checks inside `Server::open` rather than a function the program can call: "a refactor for the filesystem task to ask for". The first provider is the first program that wants it, so the cost is now visible.

**Change**
Not decided, and not to be done before measuring. Measure first: the bytes and the Loop slots above from the real build, and the time of an Open through `/files` with and without the extra hop on the target. The candidate is to split `Server::open` into a check step a handler can call and a walk step, so that the handler checks the outer connection's rights once and opens its tree directly on S. Ruled out for now: keeping the forwarding (correct, but it is what costs), and copying the checks into the example (two copies of one order of checks drift apart, which the design warned about).

**Reference**
None, this project's own design: `.agents/tasks/directory-capability/DESIGN.md`, "What a provider serves", option A.

**Acceptance**
A report in `docs/perf/` with the baseline and the stage; `examples/files` has no root connection of its own tree; the routing_checks cases through `/files` pass on both images; the `ioservice` tests cover the check step on its own.
