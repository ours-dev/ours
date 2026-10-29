# svc changes

## Title: svc::Outgoing is served by an ioservice::LoopServer, and its own copy of the Outcome-to-channel glue is gone (2026-10-28)

**Files**
- `system/lib/svc/src/outgoing.cpp` — `handle()`, `watch()`, `on_signals()` and the two helpers they used are removed, about 130 lines; `serve()` is one call to `LoopServer::serve()`; `connect()` maps the value of a service node to the protocol's `Handler`
- `system/lib/svc/include/svc/outgoing.hpp` — the private `Binding` is `ioservice::LoopServer::Binding`; the `Server` and the `Loop` pointer are one `LoopServer` member; the private functions are `on_connect()` and `connect()`
- `system/lib/svc/BUILD.gn` — depends on `//system/lib/ioservice:loop_server`
- `system/lib/svc/README.md`, `docs/repairs/261004/svc.md`, `docs/architecture/component-framework/libraries.md` — say that `svc` is on the shared glue and that `BootServer` is the copy left

**Symptom**
Latent, cannot be triggered today. After `ioservice::LoopServer` was added (`docs/changes/261027/ioservice.md`) there were still two copies of the glue that reads a message, dispatches it and carries out the `Outcome`, `BootServer::handle()` and `svc::Outgoing::handle()`, so a fix to the shared piece would still not reach the provider side.

**Cause**
`svc::Outgoing` was written by copying init's function, because the library under both had no part of the glue. With `LoopServer` there, the copy has no reason to stay.

**Change**
`Outgoing` keeps its public interface, so echo and every test compile unchanged. What it keeps is the one part that is its own, the registry: the value of a service node is the index of a `Handler` that `add_protocol()` stored. Before, that mapping sat in the `Connect` case of a 130-line function; now it is a function of its own:

```cpp
// before: one case of Outgoing::handle()
case ioservice::Action::Connect: {
    if (outcome.service >= m_protocols) {
        auto _ = ours::handle_close(outcome.handle);
        return loop::Action::Rearm;
    }
    auto const &handler = storage.handlers[outcome.service];
    handler.connect(handler.context, outcome.handle);
    return loop::Action::Rearm;
}

// after: Outgoing::connect(), called by LoopServer through Handlers
if (service >= m_protocols) {
    auto _ = ours::handle_close(channel);
    return;
}
auto const &handler = m_storage->handlers[service];
handler.connect(handler.context, channel);
```

The `Storage` the program declares keeps its fields and its size; only the type of `bindings` changed, to the one `LoopServer` declares. No behaviour is meant to change: `serve()` refuses with the same statuses, since `LoopServer::serve()` is the `bind()` and the wait it always was.

**Reference**
Fuchsia's `OutgoingDirectory` serves its directory through the same connection implementation as component_manager's directories (`src/storage/lib/vfs/cpp/connection/`).

**Verification**
- `host/obj/system/lib/svc/test/svc`: 9 passed, with `outgoing_test.cpp` untouched. These tests were written against the old `Outgoing` and drive it through a real Loop, so they are the evidence that the behaviour is the one it had. `.../ioservice/test/loop_server`: 12 passed. `.../ioservice/test/ioservice`: 42 passed.
- `grep -c "Action::Drop"` finds 0 in `system/lib/svc/src/outgoing.cpp` and 1 in `system/service/init/boot_server.cpp`: the item's acceptance line holds for the provider side.
- clang-tidy on `outgoing.cpp` with the header filter on `system/lib/svc`: no diagnostic. clang-format and gn format: clean.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change (`.worktrees/refactor-svc-on-loop-server`, at HEAD): build ok in 7.7 s; dbg PASS, shell finished at 5.6 s, 44 passed, 4 skipped, 0 failed; rel PASS, shell finished at 4.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines; host tests PASS, 148 test programs passed, 0 failed. rel.log holds `routing_checks: passed`, which reaches echo through `Outgoing` on `LoopServer`, and `init: echo exited with 0`: the two lines the repair item names as its acceptance.
- Not run in the shared worktree, which holds other sessions' uncommitted kernel edits.

**Deliberately not done**
- `BootServer` still has its copy; moving it is the next change, since it touches init and its routing.
- Folding `svc` into `ioservice` is decided and comes after this, as its own change: it renames a namespace and a directory and edits echo, which should not hide in a change that moves logic.

**How it was found**
None. This is the second of the three steps of item 1 of `docs/repairs/261004/svc.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `Outgoing` passes `this` to `LoopServer` as the context of its connect function while its own constructor is still running. Why is that safe here, and what would make it unsafe?
2. The old test file did not change. What does that tell you about the change, and what would it not tell you?
3. What does `Outgoing` still own after this change, and why can that part not move into `LoopServer`?

## Title: svc::Handler is an abstract class a protocol's server derives from, in place of a struct of function pointers (2026-10-28)

**Files**
- `system/lib/svc/include/svc/outgoing.hpp`, `system/lib/svc/src/outgoing.cpp` — `Handler` is a class with one pure virtual `connect(channel)` and a protected, non-virtual destructor; `add_protocol()` takes a `Handler &`, which `Storage::handlers` keeps by address; the null-function check is gone, since a reference cannot be null
- `examples/echo/echo_server.cpp` — `EchoServer` derives from `svc::Handler` and overrides `connect()`; its `handler()` and its static `on_connect()` that only forwarded to `connect()` are gone
- `system/lib/svc/test/outgoing_test.cpp` — the recorder is a `Handler` subclass in place of a struct and a free function; the case that passed an empty `Handler` is removed with the check it tested
- `system/lib/svc/README.md` — names the class

**Symptom**
Latent, cannot be triggered today. The user asked for the same change to `svc::Handler` that `LoopServer::Delegate` had just got (`docs/changes/261028/ioservice.md`): a struct of a function pointer and a `void *context`, which every server had to feed through a static function that cast the pointer back and called a member. `EchoServer::on_connect()` was exactly that, four lines whose only work was the cast.

**Cause**
The same as for the Delegate: the shape was copied from `loop::Callback`, and nothing checks that `context` is the object the function expects. Here the interface is public and used by every provider, so each one carried its own trampoline.

**Change**
```cpp
// before
struct Handler {
    auto (*connect)(void *context, ours::Uandle channel) -> void = nullptr;
    void *context = nullptr;
};
// echo: outgoing.add_protocol(NAME, server.handler());   // server.handler() returned {&EchoServer::on_connect, this}

// after
class Handler {
  public:
    virtual auto connect(ours::Uandle channel) -> void = 0;
  protected:
    ~Handler() = default;
};
// echo: class EchoServer final: public svc::Handler; outgoing.add_protocol(NAME, server);
```

The destructor is protected and not virtual for the reason the Delegate's is: nothing is deleted through a `Handler`, and a deleting destructor would call `operator delete` in a program with no heap. `add_protocol()` takes a reference and keeps its address, so the object must outlive the directory; the old struct was copied by value and its `context` had to, too, so the lifetime rule is not new, only written down. EchoServer inherits publicly because `main()` passes it where a `Handler &` is wanted.

**Reference**
None, this project's own design.

**Verification**
- `host/obj/system/lib/svc/test/svc`: 9 passed, with the empty-handler case removed from `AddProtocolRefusals`. `OpenOfAProtocolReachesItsHandler` registers two handlers and checks that each Open reaches only its own, which is the lookup `connect()` does.
- clang-tidy on `outgoing.cpp`, `outgoing_test.cpp` and `echo_server.cpp` with the header filter on `system/lib/svc` and `examples/echo`: no diagnostic. clang-format and gn format: clean. The full build, user programs included, links.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change (`.worktrees/refactor-svc-handler`, at HEAD): build ok in 11.6 s; dbg PASS, shell finished at 9.5 s, 44 passed, 4 skipped, 0 failed; rel PASS, shell finished at 8.4 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines; host tests PASS, 148 test programs passed, 0 failed. rel.log holds `routing_checks: passed`, which reaches echo through an Open of `svc/ours.examples.echo.Echo`, and `init: echo exited with 0`.

**Deliberately not done**
- Folding `svc` into `ioservice` is the next change; the name `svc::Handler` is renamed there.
- `BootServer` and `Realm` have no `Handler`: the root's `/svc` entries reach `Realm::connect()` through the `Delegate`.

**How it was found**
The user's answer, "换", to whether `svc::Handler` should follow the `Delegate`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `add_protocol()` used to copy the `Handler` and now keeps its address. What must a program that registers two protocols guarantee about the two objects?
2. Why can the "empty handler" test not be written any more, and is that a loss?
3. `EchoServer`'s `connect()` is private and still reachable from `Outgoing`. Through which type is it called, and why does that make it legal?
