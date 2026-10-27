# svc outstanding repairs

## 1. The glue from an ioservice Outcome to channel operations is written twice

**Severity**
Low: duplicated code, correct in both copies today.

**Symptom**
Latent, cannot be triggered today. `svc::Outgoing::handle()` (`system/lib/svc/src/outgoing.cpp`) and init's `BootServer::handle()` (`system/service/init/boot_server.cpp`) are the same function: read one message into a shared buffer, `ioservice::Server::dispatch()` it, and carry out the `Outcome` -- write the reply, close after Node.Close, drop a malformed message and close its handles, watch a bound connection, refuse with an epitaph. They differ only in what `Action::Connect` does and in init's log lines. A fix to one, such as a change to how a failed reply write is handled (I1, init item 3), has to be made in the other by hand, or the two servers start answering the same message differently.

**Cause**
The ioservice library keeps its core free of channels on purpose, so that its tests run on the host (`docs/changes/261004/vfs.md`); the glue to channels and the Loop was then written where it was first needed, in init (step 13), and step 17 needed it a second time for the provider side.

**Change**
The shared piece is written: `ioservice::LoopServer` (`system/lib/ioservice/include/ioservice/loop_server.hpp`, target `//system/lib/ioservice:loop_server`), with its host test and the fake kernel it shares with svc's (`docs/changes/261027/ioservice.md`). It takes the binding table and the message buffers as spans the program lends, and a `Handlers` for `Action::Connect` (the shape `BootServer::Connector` has) and for a failed reply write. What is left: build `svc::Outgoing` on it, then `BootServer`, and delete the two copies. Its host test can reuse `system/lib/svc/test/fake_kernel.cpp`. Ruled out: making `svc::Outgoing` the shared piece, since init's tree also holds /boot's files and every component's /svc, which an outgoing directory never does.

**Reference**
Fuchsia has one connection implementation per protocol in its vfs (`src/storage/lib/vfs/cpp/connection/`), which both component_manager's directories and every `OutgoingDirectory` use.

**Acceptance**
`grep -n "ioservice::Action::Drop" system/service/init/boot_server.cpp system/lib/svc/src/outgoing.cpp` finds nothing: both delegate to the shared piece; the svc and ioservice host tests pass, and `boot_check.py` passes both images with `routing_checks: passed` and `init: echo exited with 0`.
