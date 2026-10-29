# init changes

## Title: BootServer is served by an ioservice::LoopServer and Realm is its Delegate, so the last copy of the Outcome-to-channel glue is gone (2026-10-28)

**Files**
- `system/service/init/boot_server.cpp`, `system/service/init/boot_server.hpp` — `handle()`, `watch()`, `write()`, the slot callback, the `Connector` and the private `Binding` are removed, about 170 lines; `serve()` is one call to `LoopServer::serve()`; `set_connector()` is `set_delegate()`
- `system/service/init/realm.cpp`, `system/service/init/realm.hpp` — `Realm` derives privately from `ioservice::LoopServer::Delegate`: its `connect()` is the override, its static `on_connect()` that only forwarded to it is gone, and `write_failed()` carries the log line `BootServer::handle()` wrote
- `system/service/init/BUILD.gn` — depends on `//system/lib/ioservice:loop_server`
- `docs/repairs/261004/svc.md` — deleted: its one item is fixed
- `docs/architecture/component-framework/libraries.md` — the glue is no longer a place that is not converged

**Symptom**
Latent, cannot be triggered today. `BootServer::handle()` was the last of the two copies of the glue that reads a message, dispatches it and carries out the `Outcome`; a change to how a failed reply write is handled, or to any other action, still had to be made in init by hand after being made in `LoopServer`.

**Cause**
See `docs/changes/261027/ioservice.md` and `docs/changes/261028/svc.md`: the glue was written where it was first needed, in init, and copied to the provider side. With `LoopServer` and its `Delegate` in place, init's copy and its `Connector` have no reason to stay.

**Change**
`BootServer` keeps what is its own: the tree built from the BootFS, the `/svc` directories beside it, the node budget of each share, and the files the root reads by direct call. It hands the serving to a `LoopServer` and forwards `set_delegate()` to it. The `Delegate` is `Realm`, since the root's policy, routing every Open of a `/svc` entry again (D11), already lives there, so there is no middle layer that only passes the call on:

```cpp
// before: BootServer had its own Connector and Realm a static function to receive it
boot.set_connector({.connect = &Realm::on_connect, .context = this});
static auto Realm::on_connect(void *context, u64 service, Uandle channel) -> void;

// after
boot.set_delegate(this);                       // Realm: private ioservice::LoopServer::Delegate
auto Realm::connect(u64 service, Uandle channel) -> void override;
auto Realm::write_failed(Status status) -> void override;   // "init: /boot cannot answer a client: <status>"
```

The log line is the one `handle()` wrote through `report()`, and it reads the same. Without a delegate, an Open of a service node still closes its channel, as `BootServer` did without a connector.

**Reference**
Fuchsia's component_manager serves its own directories and every `OutgoingDirectory` through the one connection implementation in `src/storage/lib/vfs/cpp/connection/`.

**Verification**
- `grep -n "Action::Drop" system/service/init/boot_server.cpp system/lib/svc/src/outgoing.cpp` finds nothing: the acceptance line of the repair item holds on both sides.
- clang-tidy on `boot_server.cpp` and `realm.cpp` with the header filter on `system/service/init`: no diagnostic. clang-format and gn format: clean. The full build, user programs included, links.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change (`.worktrees/refactor-boot-server-on-loop-server`, at HEAD): build ok in 7.6 s; dbg PASS, shell finished at 6.0 s, 44 passed, 4 skipped, 0 failed; rel PASS, shell finished at 4.9 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines; host tests PASS, 148 test programs passed, 0 failed. rel.log holds `routing_checks: passed` and `init: echo exited with 0`, the two lines the repair item names as its acceptance; `routing_checks` opens /boot files and connects through /svc, so both of BootServer's trees were served by LoopServer. No `cannot answer a client` line appeared.

**Deliberately not done**
- Folding `svc` into `ioservice` is the next change.
- `BootServer::Storage` keeps its fields and size, so `init.cpp` is untouched.
- No test of `Realm::write_failed()`: it needs a client whose reply cannot be written on the real root, and `LoopServer`'s own test covers the path that calls it.

**How it was found**
None. It is the third step of item 1 of `docs/repairs/261004/svc.md`, filed when step 17 of the component framework copied the glue.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is `Realm` the `Delegate` and not `BootServer`, and what would `BootServer` have had to do with an Open of a service node if it were?
2. `Realm`'s constructor passes `this` to `BootServer` as its delegate. What must be true of the order in which init builds the `BootServer`, the `Realm` and the Loop for that to be safe?
3. What does an Open of a service node do if no delegate is set, and why is that the right default for a server that serves /boot before any component exists?
