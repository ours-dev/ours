# echo outstanding repairs

## 1. The echo server's refusal paths are run by no test, and the developer's guide sends readers to it as the model

**Severity**
Low: a missing test. The paths are short and read correctly, but they are the ones the guide's checklist (`docs/guide/06-checklist.md`, items W13, W14 and W15) points a new server's author at.

**Symptom**
Nothing in the tree runs these paths of `examples/echo/echo_server.cpp`: a message with an ordinal the server does not know or that its decoder refuses (lines 172-191), a client past `MAX_CLIENTS` (line 141), a client whose slot cannot be added (lines 132-138), a reply that cannot be written (lines 206-210), and a request that carries handles. `system/service/init/test/routing_checks/routing_checks.cpp` reaches echo through one connection and sends `EchoString` and then `Stop` (lines 793-826); no host test links the server.

**Cause**
`EchoServer` lives in an anonymous namespace of the program and calls `ours::channel_read`, `ours::channel_write` and `ours::process_exit` directly, so it cannot be run on the host the way `ioservice::LoopServer` is, against the fake kernel of `system/lib/ioservice/test`. The codec's refusals are tested (`examples/echo/protocol/test/ours_examples_echo_test.cpp:79-125`); what the server does with a refusal is not.

**Change**
Not decided. The options: (a) extend `routing_checks` to send echo an unknown method and a malformed request on a second connection and check the connection closes with no message, and to hold `MAX_CLIENTS + 1` connections and check the last reads the epitaph `NoResource`; the cost is a longer boot check and a second place that knows echo's limit; (b) move the serving logic out of the program into a library that takes its system calls as `ioservice::Syscalls` does, and test it on the host; the cost is a library for 100 lines, and the example stops being a single file a reader can follow. The loop-waiter plan (`.agents/tasks/loop-waiter/PLAN.md`, step 4) moves `EchoServer` onto the Loop's new form, so either is best done with or after it.

**Reference**
None, this project's own tests.

**Acceptance**
A test that fails when the unknown-method branch of `EchoServer::serve()` answers instead of closing, and one that fails when the full-table branch of `EchoServer::connect()` closes without an epitaph.
