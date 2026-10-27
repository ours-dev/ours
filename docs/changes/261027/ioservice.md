# ioservice changes

## Title: ioservice::LoopServer, the glue from an Outcome to a Loop's channels written once, and not yet used by anyone (2026-10-27)

**Files**
- `system/lib/ioservice/include/ioservice/loop_server.hpp`, `system/lib/ioservice/src/loop_server.cpp` — new: `ioservice::LoopServer`, `LoopServer::Binding`, `LoopServer::Memory` and `ioservice::Handlers`
- `system/lib/ioservice/BUILD.gn` — the `loop_server` target and its headers, and the `tests` group lists the new test
- `system/lib/ioservice/test/loop_server_test.cpp` — new: twelve tests of `LoopServer` on a real Loop
- `system/lib/ioservice/test/fake_kernel.cpp`, `system/lib/ioservice/test/fake_kernel.hpp` — moved from `system/lib/svc/test/`, with the include guard and the opening comment saying whose they are now
- `system/lib/ioservice/test/BUILD.gn`, `system/lib/svc/test/BUILD.gn` — the fake kernel is a `source_set` both tests link, instead of two files in svc's test executable
- `system/lib/ioservice/README.md`, `docs/architecture/component-framework/libraries.md`, `docs/repairs/261004/svc.md` — say where the shared piece is and what is left

**Symptom**
Latent, cannot be triggered today. The code that reads one message from a connection's channel, hands it to `ioservice::Server::dispatch()` and carries out the `Outcome` was written twice, in `BootServer::handle()` (init) and `svc::Outgoing::handle()` (svc), each with the same six cases. A fix to one, such as what happens when a reply cannot be written, would have to be made in the other by hand, or the root's /boot and a provider's outgoing directory would answer the same message differently. This change is the first of three that remove the copies: it adds the one copy and leaves both programs as they are.

**Cause**
The core of ioservice makes no system call and touches no channel on purpose, so that its tests run on the host. That left the glue to whoever served a tree first. The root needed it in step 13 of the component framework and wrote it into `BootServer`; step 17 needed it for the provider side and copied it into `svc::Outgoing`. Neither place was wrong, but the library whose core they both call had no part of it, so the second program had nowhere to reuse it from. The glue also hides a difference worth naming: the two copies differ in only two ways, what `Action::Connect` does and a log line when a reply write fails, and both differences are a function the program supplies, which is why they fit a small struct of function pointers.

**Change**
`LoopServer` owns a `Server` and the Loop slot of every connection. The program lends it the storage as spans, so one class serves a table of 4 connections (a provider) and one of 128 (the root) without being a template and without a second copy of the code:

```cpp
// what each program has today: a Storage of its own, a private Binding, a handle() of its own
struct Storage { std::array<ioservice::Connection, MAX_CONNECTIONS> connections; /* ... */ };
auto handle(ioservice::ConnectionId connection, Signals observed) -> loop::Action;

// what LoopServer takes instead
auto memory = ioservice::LoopServer::Memory{
    .connections = storage.connections, .bindings = storage.bindings,
    .request_bytes = storage.request_bytes, .request_handles = storage.request_handles,
    .reply = &storage.reply,
};
auto server = ioservice::LoopServer(tree, loop, syscalls, memory, handlers);
```

`Handlers` carries the two differences: `connect(context, service, channel)` for an Open of a service node (closed when absent, as `BootServer` does without a connector), and an optional `write_failed(context, status)` called before a connection whose reply could not be written is ended, which is where init keeps its log line. `serve(channel, directory, rights)` is what `Outgoing::serve()` and `BootServer::serve()` both did: bind, then wait on the channel.

It is its own build target, `//system/lib/ioservice:loop_server`, so `//system/lib/ioservice` keeps making no system call. The fake kernel that svc's test already had moved to `system/lib/ioservice/test`, since `LoopServer` is now the lower library and svc will be built on it. `system/lib/loop/test` still has a smaller fake kernel of its own; folding it in is not part of this.

Rejected: a class template on the number of connections, because every program would carry its own copy of the code, which is the cost this change removes; putting the glue in the `ioservice` target itself, because the core would then call the vDSO and its tests would need a fake kernel.

**Reference**
Fuchsia has one connection implementation per protocol in its vfs (`src/storage/lib/vfs/cpp/connection/`), which both component_manager's directories and every `OutgoingDirectory` use; `LoopServer` plays that part here.

**Verification**
- `host/obj/system/lib/ioservice/test/loop_server`: 12 passed. `.../svc/test/svc`: 9 passed, on the moved fake kernel. `.../ioservice/test/ioservice`: 42 passed.
- Three deliberate breakages of `loop_server.cpp`, each followed by a build and a run of the test, each caught by the test the breakage should fail: the handles of a dropped message left open (`MalformedMessageDropsTheConnection`), a failed reply not reported (`ReplyThatCannotBeWrittenIsReportedAndEndsTheConnection`), a client that went away keeping its connection (`ClientGoneReleasesTheConnection`). The source was put back and the test run again: 12 passed.
- clang-tidy against `out/ioservice/compile_commands.json` on `loop_server.cpp` and `loop_server_test.cpp`: no diagnostic in this change's files, headers included.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change (`.worktrees/refactor-loop-server`, at HEAD): build ok in 30.6 s; dbg PASS, shell finished at 5.8 s, 44 passed, 4 skipped, 0 failed; rel PASS, shell finished at 4.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines; host tests PASS, 148 test programs passed, 0 failed (the line counts programs, not cases).
- Not run in the shared worktree: it holds other sessions' uncommitted kernel edits, which failed both images in a build of the previous change (`docs/changes/261008/ioservice.md`).

**Deliberately not done**
- `svc::Outgoing` and `BootServer` still have their own `handle()`. Moving them, and deleting the copies, are the next two steps, so that each is one change that builds and boots.
- Whether `svc` stays a library or folds into `ioservice` once `Outgoing` is a thin registry: decided with the next step, when the registry's real size is known.
- `system/lib/loop/test`'s fake kernel is not merged with the moved one.

**How it was found**
None. The duplication was filed as item 1 of `docs/repairs/261004/svc.md` when step 17 copied the glue; this change starts working it off.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `LoopServer` take its connection table, its bindings and its message buffers as spans instead of owning arrays, and what would each of the two programs have paid if it were a template on the number of connections?
2. What would go wrong if a connection's Binding were not the one at its index, and what does `watch()` do when the program lent fewer Bindings than connections?
3. Why is the glue a target of its own, and not part of `//system/lib/ioservice`, when every program that uses the core also needs the glue?
