# echo outstanding repairs

## 1. The echo server's refusal paths are run by no test, and the developer's guide sends readers to it as the model

**Severity**
Low: a missing test. The paths are short and read correctly, but they are the ones the guide's checklist (`docs/guide/06-checklist.md`, items W13, W14 and W15) points a new server's author at.

**Symptom**
Nothing in the tree runs these paths of `examples/echo/echo_server.cpp`: a message with an ordinal the server does not know or that its decoder refuses (lines 189-208), a client past `MAX_CLIENTS` (line 158), a client whose slot cannot be added (lines 149-155), a reply that cannot be written (lines 223-227), and a request that carries handles. `system/service/init/test/routing_checks/routing_checks.cpp` reaches echo through one connection and sends `EchoString` and then `Stop` (lines 793-826); no host test links the server.

**Cause**
`EchoServer` lives in an anonymous namespace of the program and calls `ours::channel_read`, `ours::channel_write` and `ours::process_exit` directly, so it cannot be run on the host the way `ioservice::LoopServer` is, against the fake kernel of `system/lib/ioservice/test`. The codec's refusals are tested (`examples/echo/protocol/test/ours_examples_echo_test.cpp:79-125`); what the server does with a refusal is not.

**Change**
Not decided. The options: (a) extend `routing_checks` to send echo an unknown method and a malformed request on a second connection and check the connection closes with no message, and to hold `MAX_CLIENTS + 1` connections and check the last reads the epitaph `NoResource`; the cost is a longer boot check and a second place that knows echo's limit; (b) move the serving logic out of the program into a library that takes its system calls as `ioservice::Syscalls` does, and test it on the host; the cost is a library for 100 lines, and the example stops being a single file a reader can follow. The loop-waiter plan (`.agents/tasks/loop-waiter/PLAN.md`, step 4) moves `EchoServer` onto the Loop's new form, so either is best done with or after it.

**Reference**
None, this project's own tests.

**Acceptance**
A test that fails when the unknown-method branch of `EchoServer::serve()` answers instead of closing, and one that fails when the full-table branch of `EchoServer::connect()` closes without an epitaph.

## 2. An echo client's entry stays held when the Loop frees its slot after a failed re-arm

**Severity**
Low: wrong, but not reached today, and in an example program. It needs `object_wait_async` to refuse the re-arm of a handle it accepted when the client was added.

**Symptom**
Latent, cannot be triggered today. Were it to happen, the client's channel is closed and its Loop slot freed, but its entry in `EchoServer::Storage::clients` still has `channel` set, so `connect()` never takes it again. After `MAX_CLIENTS` (4) such events every Open of the protocol is closed with an epitaph of `NoResource`.

**Cause**
The same as item 1 of `docs/repairs/261029/ioservice.md`, in another program. `Loop::run()` step 4 closes a slot's handle and frees the slot not only for the answer `Close` but also when step 3's `arm()` fails after an answer of `Rearm`. `EchoServer::release()` frees an entry only on the paths where `serve()` answers `Close`, and the Loop never tells a `Waiter` that its slot was freed. `echo_string()` answers `Rearm` and so is the path this would take. It cannot happen today for the reasons that item gives: the re-arm is the wait the first `add()` armed, on the same handle with the same rights for the same signals, and `SLOTS` is `static_assert`ed not to exceed `ours::PORT_MAX_OBSERVERS`. The root's `ResourceServer` and `LauncherServer` entries have the same property; they are not listed here because they are not part of this change.

**Change**
Not decided, and not independent in what it needs: it follows the user's choice among the options of `docs/repairs/261029/ioservice.md` item 1 (a second virtual function of `Waiter`, a failed re-arm treated as `Close`, or leaving it). The fix in this file is then to use whichever mechanism is chosen in `Client`. Taken alone, the item can be closed by deciding to leave it, with the comment of `EchoServer::Client` kept honest.

**Reference**
None, this project's own design.

**Acceptance**
Once the Loop tells a `Waiter` about a slot freed without its `Close`, a read of `Client` and `EchoServer::release()` shows that every freed slot releases its entry. The echo program has no host test; the boot check (`routing_checks: passed`) shows the normal path still works.

## 3. The only client of an example server is a 900-line boot test, so a tutorial has the reader edit that

**Severity**
Low: nothing misbehaves. It is a gap in what the tree offers a reader who wants to see their own server answer.

**Symptom**
`examples/echo` is reached by one client, the check `check_echo()` inside `system/service/init/test/routing_checks/routing_checks.cpp`, a 900-line program that tests the root. A reader who copies echo into a server of their own has no small client to copy beside it. The first-server chapter (`docs/guide/02-first-server.md`) has the reader add their check to `routing_checks` and its `use` to `routing_checks.cm`, so a tutorial edits the root's own boot test, and a reader's mistake there can fail checks that are not theirs.

**Cause**
`echo` was written to put the route through a sibling's `expose` on the target, and `routing_checks` already had the client helpers (`Incoming`, `call()`, `connect_status()`). A client of its own was never needed for that.

**Change**
Not decided. The options: (a) leave it, and have the guide say why the client is `routing_checks`; (b) add a small client component beside the example (`examples/echo_client`, a child of the dev image's root that sends `EchoString` and prints a line), with the helpers it needs in a library that `routing_checks` also uses; the cost is a third component in the image and a refactor of the helpers out of a 900-line file.

**Reference**
None, this project's own design.

**Acceptance**
A reader can follow the first-server chapter without editing `routing_checks.cpp`, and the dev image still boots with `routing_checks: passed`.
