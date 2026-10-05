# svc changes

## Title: svc::Outgoing serves a component's outgoing directory on the Loop, and an echo provider puts the sibling route on the target (2026-10-04)

**Files**
- `system/lib/svc/` — new library, scaffolded with copier from `tools/templates/cpp_library` (its placeholder `svc.hpp`, `svc.cpp` and `svc_test.cpp` renamed after what they hold)
- `system/lib/svc/include/svc/outgoing.hpp`, `src/outgoing.cpp` — `svc::Outgoing`: a vfs tree holding the outgoing directory, `svc/` and a service node per protocol, a vfs server bound to the DirectoryRequest channel, a Loop slot per connection, and a `Handler` per protocol that an Open of `svc/<name>` hands its channel to
- `system/lib/svc/test/fake_kernel.hpp`, `fake_kernel.cpp` — a host model of channels and ports, defining the eight system calls the library and its Loop make under the vDSO's own symbols
- `system/lib/svc/test/outgoing_test.cpp` — 9 host tests on a real Loop: add_protocol()'s refusals, serve()'s refusals, an Open of `svc/<name>` reaching its handler, a protocol added after serve(), NotFound for a name not served, a protocol reached through an opened `svc/`, a malformed message, a client gone, and the connection limit
- `system/lib/svc/BUILD.gn`, `test/BUILD.gn`, `README.md`, `.clang-format`, `.gitignore` — the library's targets and the copier template's files
- `examples/echo/protocol/` — new library, scaffolded with copier: the codec of `ours.examples.echo`, EchoString and Stop, and 7 host tests
- `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn` — the provider: `bin/echo_server` and its manifest `echo.cm`, which declares `ours.examples.echo.Echo` and exposes it from self
- `examples/BUILD.gn` — echo joins the `examples` group; the comment says which targets must be built in a user toolchain
- `kernel/products/image/dev/BUILD.gn` — root.cm gains the eager child `echo` and an offer of its protocol to `#routing_checks`, renamed `ours.test.Echo`
- `system/service/init/test/routing_checks/routing_checks.cpp`, `BUILD.gn` — a fourth protocol use, `ours.test.Echo`, and `check_echo()`: EchoString through the sibling route, Stop, then a connect that must answer NotFound
- `system/lib/wire/test/ordinals_test.py` — also reads the codec headers beside the examples, and expects the two echo selectors
- `.agents/skills/ours-verify/scripts/boot_check.py` — dev requires `init: echo exited with 0` besides `routing_checks: passed`
- `BUILD.gn` — `//system/lib/svc:tests` and `//examples/echo/protocol:tests` join the host test list
- `docs/repairs/261004/svc.md` — the Outcome-to-channel glue written twice, in init's BootServer and here
- `docs/repairs/261004/init.md` — item 10: a route to a provider that has exited answers NotFound without the root's log line

**Symptom**
None; this is step 17 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`), its last. Until it, half of the router ran only on the host: every route on the target ended at a built-in of the root (`boot`, the Launcher, DebugResource), so `route()`'s child branch (an expose), its self branch (a declared capability), and `Realm::forward()`, which writes the Open into a provider's outgoing directory, had never run against a real process. Design D3 (a) chose to keep expose and declared capabilities, and this step is the cost it named: a provider library, a provider, and a route to it.

**Cause**
What a provider has to do, and why each piece is the shape it is.

- **Why an outgoing directory at all, and not a channel per protocol.** The root cannot hand a provider one channel per client in advance: clients come later, and how many is not known. Fuchsia's answer, which ours keeps, is one directory per component, the outgoing directory, whose server end the runner hands the component at start as the DirectoryRequest startup handle. The root keeps the client end (`Instance::m_outgoing`), made at resolve so that an Open forwarded before the provider runs waits in the channel, as in component_manager's escrow. A route that ends at the provider becomes one message on that channel: `Open("svc/<name>", PROTOCOL_SERVICE, server)`, the client's server end inside it (`Realm::forward()`, Fuchsia's `OutgoingConnector::send`). The root does not wait for an answer, so from then on the client and the provider talk directly, and the root is out of the path (I1, I2).
- **Why the provider's side is a vfs tree.** The forwarded message is an ordinary fuchsia.io Open, so the provider needs exactly what the root already has for /svc: a pseudo-directory holding service nodes, and a server that checks the Open (path syntax, TRAVERSE, CONNECT for PROTOCOL_SERVICE) and turns an Open of a service node into "here is the channel". `vfs::Server::dispatch()` answers that with `Action::Connect` and the node's value; `svc::Outgoing` uses the value as an index into its handlers. So the provider reuses the root's whole Open logic, and a client that opens `svc/` itself as a directory, or lists it, gets the same answers it would from the root's /svc. Fuchsia's `OutgoingDirectory` is built the same way, on `svc_dir` over the C++ vfs.
- **Why `svc/` and not the root of the directory.** component_manager forwards to `svc/<name>` (`kServiceDirectory`), because the outgoing directory also carries other things in Fuchsia (`diagnostics/`, `debug/`). The path is part of the contract between `Realm::forward()` and every provider, so `svc::SVC_DIRECTORY` and init's `OUTGOING_SVC_PREFIX` must agree; both say "svc".
- **Why it runs on the Loop and hands channels to handlers.** One thread waits on everything (the root's D16 reasoning applies to any component on ort, which cannot start a second thread in a running process). The DirectoryRequest channel is a vfs connection with a Loop slot; a handler receives each protocol channel inside that slot's callback and typically adds it to the same Loop. The Loop's rules hold unchanged: a callback handles one message and answers Rearm or Close, and only a slot's own callback closes its handle (I3), which is why a client gone is answered by `release()` plus Close and never by closing from elsewhere.
- **Why it is served with every right.** Fuchsia serves an outgoing directory with `fuchsia_io::Rights::kMask` (`FuchsiaVfs::ServeDirectory()`); the provider does not limit what the root may ask of it, and every Open through it is held to its own flags by the vfs. The tree has no file, so no right reaches anything to write or execute.
- **Why Stop, and why it is not Fuchsia's protocol.** D24 (a) wants a provider that exits on request, so that the routing checks can see a route to an exited component answer NotFound (D23, Fuchsia's `RouteSourceShutdown`). Fuchsia's routing example protocol, `fidl.examples.routing.echo.Echo`, has only EchoString, and Fuchsia stops components through the Lifecycle channel component_manager holds, never from a client. ours has no Lifecycle, and the client that checks the route is the one that has to know when the provider is gone, so the method lives in the protocol, and the protocol takes a name of ours: `ours.examples.echo.Echo` (the design's open question on the echo protocol's name, closed here). EchoString keeps Fuchsia's shape, with the string always present.
- **Why echo exits from inside a callback.** The Loop has no quit, and `run()` returns only when the port fails. Stop's handler writes its reply and calls `process_exit(0)`, the counterpart of `exit()` in a handler. Every handle echo holds closes as it dies: the outgoing directory's server end, which the root sees as PEER_CLOSED on `m_outgoing` and so stops forwarding there, and its process, which the root sees as TERMINATED and logs `init: echo exited with 0`.
- **Why the NotFound check is ordered, and what it rests on.** routing_checks learns echo is dead from PEER_CLOSED on its echo connection; the root learns it from PEER_CLOSED on the outgoing directory (or TERMINATED). All of these come from echo's handle table being emptied one handle at a time. ours's `HandleTable::clean()` closes handles in the order they were added, so the outgoing directory (a startup handle) closes before the connection, the root's packet is queued before routing_checks writes its next Open, and the port's FIFO order makes the first connect answer NotFound; five boots in a row saw no other outcome. Zircon's `HandleTable::Clean()` closes newest first, and under that order the Open could be forwarded into the dying directory and lost with it: PEER_CLOSED with no epitaph, the design's second worst interleaving. Losing it requires the directory's close, which queues the root's packet, so the next connect answers NotFound. routing_checks allows that one loss and no more, so the check stays exact under either kernel order.

**Change**
The provider's whole program, after setup (`examples/echo/echo_server.cpp`):

```cpp
loop::Loop loop(port, slots);
svc::Outgoing outgoing(*outgoing_storage.init(), loop);
EchoServer server(*echo_storage.init(), loop);
outgoing.add_protocol(ours_examples_echo::ECHO_NAME, server.handler());
outgoing.serve(ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0));
loop.run(ours::INSTANT_INFINITE);
```

which is Fuchsia's echo_server line for line: `OutgoingDirectory(dispatcher)`, `ServeFromStartupInfo()`, `AddUnmanagedProtocol<Echo>(handler)`, `loop.Run()`. `ServeFromStartupInfo` is left to the program, since `ort` does not build on the host, where the library's tests run.

The heart of `svc::Outgoing::handle()`, the one case that differs from init's BootServer:

```cpp
case vfs::Action::Connect: {
    if (outcome.service >= m_protocols) {
        auto _ = ours::handle_close(outcome.handle);
        return loop::Action::Rearm;
    }
    auto const &handler = storage.handlers[outcome.service];
    handler.connect(handler.context, outcome.handle);
    return loop::Action::Rearm;
}
```

The route, in dev's root.cm and echo.cm:

```gn
# root.cm, one of its offers
{
  protocol = "ours.examples.echo.Echo"
  from = "#echo"
  to = "#routing_checks"
  as = "ours.test.Echo"
}

# echo.cm
capabilities = [
  {
    protocol = "ours.examples.echo.Echo"
  },
]
expose = [
  {
    protocol = "ours.examples.echo.Echo"
    from = "self"
  },
]
```

routing_checks uses `ours.test.Echo` at `/svc/ours.test.Echo`; the walk goes up to the root's offer, down through echo's expose, and ends at echo's declaration, so the root forwards `Open("svc/ours.examples.echo.Echo")`: the provider sees its own name, not the one its client was given.

Where things were put, and the options not taken:
- The codec lives beside the example, `//examples/echo/protocol`, as Fuchsia keeps `fidl.examples.routing.echo` under `examples/components/routing/fidl`; `system/lib` holds the codecs of system protocols. Its include prefix and namespace, `ours_examples_echo`, follow `fuchsia_io` for fuchsia.io. The ordinal test now also reads `examples/**/include`.
- The program is `bin/echo_server`, Fuchsia's name, which also keeps it apart from the shell's `echo` builtin; the child is named `echo`.
- echo is root.cm's second child, after routing_checks, so the order the plan's test plan lists (routing_checks, echo, then the shell in step 15) holds. routing_checks may send its Open before echo serves anything: the Open waits on routing_checks's /svc channel until the root enters its Loop, which is after every eager child has started, and once forwarded it waits in echo's outgoing channel until echo reads it.
- Storage is fixed, as everywhere in user space: 8 protocols and 4 directory connections per outgoing directory, 4 clients for echo. A client past them is refused with NoResource.

**Reference**
- Fuchsia `component::OutgoingDirectory::Serve()`, `ServeFromStartupInfo()` and `AddUnmanagedProtocolAt()` (`sdk/lib/component/outgoing/cpp/outgoing_directory.cc`), `kServiceDirectory` (`outgoing_directory.h:72`); `svc_directory_serve()` (`sdk/lib/svc/dir.cc`) over `FuchsiaVfs::ServeDirectory()` with `fuchsia_io::Rights::kMask` (`src/storage/lib/vfs/cpp/fuchsia_vfs.h:196-199`).
- component_manager `OutgoingConnector::send()` (`src/sys/component_manager/src/model/component/instance.rs:801-822`), the forward `Realm::forward()` mirrors; `RoutingError::RouteSourceShutdown` (`src/sys/lib/routing/src/error.rs:353`, `:424`).
- Fuchsia's routing example: `examples/components/routing/cpp/echo_server/main.cc` and `meta/echo_server.cml`, `meta/echo_realm.cml` (an offer from `#echo_server` to `#echo_client`), `fidl/echo.fidl`.
- Zircon `HandleTable::Clean()` and `AddHandleLocked()` (`zircon/kernel/object/handle_table.cc`), newest first, against ours's `HandleTable::clean()` (`kernel/object/handle_table.cpp`), oldest first.

**Verification**
- Host: `//system/lib/svc/test:svc` 9 tests and `//examples/echo/protocol/test:ours_examples_echo` 7 tests, `[  PASSED  ]`; `python3 system/lib/wire/test/ordinals_test.py` OK (5 tests).
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/bringup.x86`) on `outgoing.cpp`, `outgoing_test.cpp`, `fake_kernel.cpp`, `ours_examples_echo.cpp`, `ours_examples_echo_test.cpp`, `echo_server.cpp` and `routing_checks.cpp`: nothing on these files (googletest's own headers report, as for every test here).
- `boot_check.py`: dbg PASS (28 passed, 1 skipped), rel PASS (25 passed, 3 skipped, 2/2 required); rel five more times and both images once with `SMP=1`, all PASS. The dev log now reads `init: /boot has 10 files, 5 executable`, `init: started routing_checks`, `init: started echo`, then `echo: stopping, as a client asked`, `init: echo exited with 0`, `routing_checks: passed`. No `init: cannot forward` line in any run: the first connect after echo's exit answered NotFound every time.

**Deliberately not done**
- No Lifecycle channel and no stop from the root: there is no `task_kill` (D23), and the plan's check only needs the provider gone.
- echo is not restarted on its next route, as Fuchsia would; D23 (a) keeps an exited component exited.
- The Outcome-to-channel glue is not factored out of init's BootServer and `svc::Outgoing` into the vfs library: that changes init, outside this step (`docs/repairs/261004/svc.md`, item 1).
- No log line for a route to an exited provider: that is the root's `Realm::forward()`, step 14's code (`docs/repairs/261004/init.md`, item 10).
- No hand-written function for `docs/authored.md`: this step has none of P3's.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. routing_checks asks for `/svc/ours.test.Echo`, but echo serves `svc/ours.examples.echo.Echo`. Which declaration does the rename, and at which hop of `route()` does the name change?
2. routing_checks may send its Open before echo serves its outgoing directory. In which two channels can that Open wait, one after the other, and why does it not answer NotFound?
3. After Stop, why does the first connect answer NotFound under ours's kernel, and what would make it answer PEER_CLOSED with no epitaph under Zircon's handle-table order instead?
