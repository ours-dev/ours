# ioservice outstanding repairs

## 1. A connection stays held in the Server when the Loop frees its slot after a failed re-arm

**Severity**
Medium: wrong, but not reached today. It needs `object_wait_async` to refuse the re-arm of a handle it accepted when the connection was added.

**Symptom**
Latent, cannot be triggered today. Were it to happen, the connection's channel is closed and its Loop slot freed, but the `Server` still holds the connection: its index is never handed out again, so one of the program's connections (128 in the root's /boot, 4 in each `Outgoing`) is lost for good, and `Server::channel()` would still answer the closed handle for it.

**Cause**
`Loop::run()` step 4 closes a slot's handle and frees the slot not only for the answer `Close` but also when step 3's `arm()` fails after an answer of `Rearm` (`system/lib/loop/src/loop.cpp`). `LoopServer::handle()` releases its connection only on the paths where it answers `Close` itself (`Server::release()` is the only way a connection is freed). Its comment in `handle()` states the invariant the other way round: "Nothing releases a connection without closing its slot in the same callback". The converse, a slot closed without the connection released, has no owner: the Loop never tells a `Waiter` that its slot was freed, and a `Waiter` cannot tell a re-arm that worked from one that did not.

It cannot happen today, since the re-arm is the wait the first `add()` armed, on the same handle with the same rights for the same signals, and the port's observer limit is checked at build time (`static_assert(LOOP_SLOTS <= ours::PORT_MAX_OBSERVERS)`). It would take a kernel that revokes a handle's rights or lowers a port's limit while the process runs.

**Change**
Not decided. The options: (a) `Waiter` gets a second virtual function, called when the Loop frees the slot for any reason but a `Close` the Waiter answered, which a `Binding` answers by releasing its connection; (b) `Loop::run()` treats a failed re-arm as an answer of `Close` to the Waiter before it frees the slot; (c) leave it, and keep the comment in `handle()` honest. (a) and (b) change the Loop's spec, which is the user's to decide, and (a) adds a second virtual function and so a larger vtable to every Waiter.

**Reference**
None, this project's own design.

**Acceptance**
A host test in `loop_server_test.cpp`, with a fake kernel that refuses the second `object_wait_async` for a handle, shows the connection released: a following `serve()` of `CONNECTIONS` connections succeeds.
