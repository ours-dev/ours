# ioservice changes

## Title: a LoopServer connection's Binding is the Loop's Waiter, and the trampoline that cast its context back is gone (2026-10-29)

**Files**
- `system/lib/ioservice/include/ioservice/loop_server.hpp` — `LoopServer::Binding` is `final: public loop::Waiter` and overrides `on_signals(observed) -> loop::Action`; the static `LoopServer::on_signals(void *, Signals)` is deleted; the comments of the namespace, `Delegate`, `Binding`, `Memory` and `handle()` say a Binding's `on_signals()` where they said the callback
- `system/lib/ioservice/src/loop_server.cpp` — `LoopServer::Binding::on_signals()` is `m_server->handle(m_connection, observed)`, and `watch()` registers `m_loop->add(channel, CONNECTION_SIGNALS, binding)`, the Waiter overload
- `system/lib/ioservice/test/loop_server_test.cpp` — `EachConnectionIsReachedThroughItsOwnBinding`, with the helpers `request_dirents()` and `expect_dirents_reply()`; no existing test changed
- `system/lib/ioservice/README.md` — names the Binding as the connection's `loop::Waiter`

**Symptom**
Latent, cannot be triggered today. This is step 2 of 6 of `.agents/tasks/loop-waiter/PLAN.md`: the first of the six registration sites to move from the Loop's function pointer and `void *context` to a `loop::Waiter` (`docs/changes/261029/loop.md`). The slot's behaviour, the messages on the wire and the connection table are as they were; what changes is the type the Loop is given.

**Cause**
What the old form made `LoopServer` write. `watch()` gave the Loop a pointer to a static function and a pointer to the connection's `Binding`, and the function's only work was to take the `void *` back and call a member:

```cpp
auto binding = static_cast<Binding const *>(context);
return binding->m_server->handle(binding->m_connection, observed);
```

That is a cast the compiler cannot check. A context of the wrong type compiles and fails at run time, and every site that registers with a Loop wrote the same function once more.

Why a Binding can be the Waiter. A `Waiter` is the object a slot waits on, so it is right where one object has one slot and one behaviour. A connection is that: the Server's connection table gives each connection an index, `Memory::bindings` gives it the Binding at that index, `watch()` fills the Binding in and registers it, and from then on that slot's wait fires only for that connection. The Binding already held exactly what the behaviour needs (the server and the connection), and the behaviour was already one call, `handle()`. Making the Binding derive from `Waiter` and own that call moves nothing: the data stays where it was, the function is no longer a static member of the enclosing class that has to be handed the Binding, and `handle()` stays private to `LoopServer`, reached from the nested class as any member is.

Why the rules of `Waiter` hold for it:
- One unit of work. `handle()` reads one message or reports the client gone, and answers.
- Never closes its own handle. It answers `Close` and the Loop closes the channel (I3 of `Loop::run()`); a Drop or a failed write releases the connection first and then answers `Close`.
- Lifetime. The Loop keeps a Waiter's address, so the Binding must stay where it is from `add()` until its slot is freed. The Bindings are an array the program lends (`Memory::bindings`), which outlives the `LoopServer` and is never moved. A Binding is registered again only for a new connection, and a new connection takes an index that no live connection holds. `handle()` releases a connection only to answer `Close` in the same call, and nothing runs between that answer and the Loop freeing the slot.
- The destructor is protected in `Waiter`, so a Binding is never deleted through the base, and `Binding` is `final`.

What it costs. A Binding grows by a vptr: `sizeof` is 24, where it was 16 (`-fdump-record-layouts` on the user target: the vptr at 0, `m_server` at 8, `m_connection` at 16). The root's /boot has 128 of them in `BootServer::Storage` (1 KiB more) and each `Outgoing` has 4 (32 bytes). The slot table does not shrink in this step: the Waiter overload still stores the Loop's one function and the Waiter in the old two fields, so `Slot` is as it was. Step 6 drops one of the two pointers from `Slot`, 8 bytes in each of the root's 176 slots, and that is the saving. Where each slot's object has its own Binding, as here, the two cancel: a vptr paid in the Binding, a pointer saved in the slot, so the net change per connection is about zero. The reason for the change is the interface and the removed cast, as `loop-waiter/PLAN.md` says, not memory.

What it saves. The static function and its `static_cast`, and with it the one place where `LoopServer` had to know that a `Binding` travels as a `void *`.

What it does not change. Where Bindings are stored. `BootServer::Storage` and `Outgoing::Storage` hold them as `std::array<Binding, N>` members and are built by `ustl::LazyInit<...>::init()` in `main()` (init.cpp, echo_server.cpp), which runs `Binding`'s constructor and so sets its vptr; neither is `constinit` itself, and `Binding`'s constructor stays `constexpr`, so a program that wants the array constant-initialised can still do it. Nothing copies a Binding: `Waiter`'s copy is protected, `Binding` has no copy of its own, and the arrays are passed to the server as `std::span<Binding>`.

**Change**
```cpp
// before
class Binding final {
  public:
    constexpr Binding() = default;
  private:
    friend class LoopServer;
    LoopServer *m_server = nullptr;
    ConnectionId m_connection = {};
};
// LoopServer
static auto on_signals(void *context, ours::Signals observed) -> loop::Action;
// watch()
m_loop->add(channel, CONNECTION_SIGNALS, &LoopServer::on_signals, &binding);
// on_signals()
auto const *const binding = static_cast<Binding const *>(context);
return binding->m_server->handle(binding->m_connection, observed);

// after
class Binding final: public loop::Waiter {
  public:
    constexpr Binding() = default;
    auto on_signals(ours::Signals observed) -> loop::Action override;
  private:
    friend class LoopServer;
    LoopServer *m_server = nullptr;
    ConnectionId m_connection = {};
};
// watch()
m_loop->add(channel, CONNECTION_SIGNALS, binding);
// loop_server.cpp
auto LoopServer::Binding::on_signals(Signals const observed) -> loop::Action {
    return m_server->handle(m_connection, observed);
}
```
`on_signals()` is defined in `loop_server.cpp` and not in the class: it is then `Binding`'s first non-inline virtual function, so the vtable is emitted in that one file and not in every file that includes the header, as `Delegate`'s is through `write_failed()`. `m_server` and `m_connection` stay private to `Binding`, with `LoopServer` as the friend that fills them in `watch()`, as before.

`Outgoing` and `BootServer` needed nothing. They reach a Binding only as the element type of an array in their `Storage` and pass the array to `LoopServer::Memory`; neither registers a callback of its own. Their comments say "a connection's callback", which is still true of `on_signals()` in the plain sense, so they are left as they are.

**Reference**
None, this project's own design. The class it derives from is `loop::Waiter` of step 1 (`docs/changes/261029/loop.md`), which follows `LoopServer::Delegate` and `launch::LauncherOps` (`docs/changes/261028/ioservice.md`, `launch.md`).

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/ioservice/tests phony/system/lib/loop/tests`, then from `out/bringup.x86`: `host/obj/system/lib/ioservice/test/loop_server` 13 passed (12 before, one new), `.../ioservice/test/outgoing` 9 passed, `.../ioservice/test/ioservice` 42 passed, `host/obj/system/lib/loop/test/loop` 29 passed. The 21 tests of `loop_server` and `outgoing` that were there before pass unchanged.
- Four deliberate breakages of `loop_server.cpp`, the source put back after each. `on_signals()` answers `Close` whatever `handle()` said: 9 of 12 `loop_server` tests and 6 of 9 `outgoing` tests fail. It passes `Signals::None` for `observed`: 8 and 5 fail. It handles connection `{0, FIRST_GENERATION}` whatever its Binding holds: `OpenOfADirectoryMakesAConnectionOfItsOwn`, the new test, and `OutgoingTest.ProtocolsReachedThroughAnOpenedSvcDirectory` fail. `watch()` registers `bindings[0]` for every connection: **no existing test failed**, so the old tests did not pin that each connection has a Binding of its own, and the new `EachConnectionIsReachedThroughItsOwnBinding` is the one that fails, and only it. That is the gap the new test closes: every older test serves one connection at a time, or sends its only request on the connection served last, which a shared Binding still gets right.
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build; init, echo and the shell all link `ioservice`) and `phony/benches/benches` build.
- `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86 --header-filter='.*/system/lib/ioservice/.*'` on `loop_server.cpp`, `loop_server_test.cpp`, `outgoing.cpp`, `outgoing_test.cpp`, `boot_server.cpp` and `echo_server.cpp`: no diagnostic. `format_cpp.py`: clean.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-loopserver` in `.worktrees/lw-loopserver` with the final tree: build ok in 6.9 s; dbg PASS, shell finished at 5.6 s, 44 passed, 4 skipped, 0 failed, 0 error lines; rel PASS, shell finished at 4.7 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines; host tests PASS, 148 passed, 0 failed. `rel.log` holds `init: echo exited with 0` and `routing_checks: passed`, and both logs `shell: loop checks passed`. This is the real proof: the root serves /boot, /svc and every component's namespace through Bindings, and echo serves its outgoing directory through them, so a Binding that the Loop could not reach as a Waiter would stop the boot before the shell.
- Not measured: the cost of the detour (`Loop::run()` calls `call_waiter()`, which makes the virtual call) on its own; it goes in step 6.

**Deliberately not done**
- `Outgoing` and `BootServer` are not touched, nor their comments; see the Change.
- No `Binding` constructor or function takes the server and connection to hide `friend class LoopServer`. The friend is as it was and is not what this step is about.
- `Slot` and `run()` are not touched: that is step 6.
- No test drives a Binding by calling `on_signals()` on it directly. The tests reach it the way the program does, through `Loop::run()`.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A `Binding` is registered with the Loop by address and the Loop does not own it. Which two facts about `Memory::bindings` keep that address valid, and what would break if the program lent a `std::vector` that grew?
2. Why does the Waiter overload make the slot table no smaller in this step, and which step makes the saving real? Where does the vptr show up in the root's memory?
3. Why did none of the 12 older tests fail when `watch()` registered `bindings[0]` for every connection, and what does the new test do that they did not?

## Title: a LoopServer connection is released when the Loop cannot arm its wait again (2026-10-30)

**Files**
- `system/lib/ioservice/include/ioservice/loop_server.hpp` — `Binding::on_rearm_failed(cause)`, which overrides the Loop's new notification, and the private `LoopServer::rearm_failed(connection)`; the comments of the class, of `Binding` ("registered again only after its connection was released and the Loop has freed its slot") say the new path
- `system/lib/ioservice/src/loop_server.cpp` — the two definitions; the comment in `handle()` about a slot outliving its connection says the new path
- `system/lib/ioservice/test/fake_kernel.hpp`, `fake_kernel.cpp` — `fail_next_wait_async(status)`: the next `object_wait_async` answers `status` and arms nothing
- `system/lib/ioservice/test/loop_server_test.cpp` — two tests, `ConnectionIsReleasedWhenTheLoopCannotArmItsWaitAgain` and `OnlyTheConnectionWhoseRearmFailedIsReleased`
- `system/lib/ioservice/README.md` — the `Binding` bullet names the second function
- `docs/repairs/261029/ioservice.md` — item 1 deleted (items 2 and 3 keep their numbers)

**Symptom**
Latent, cannot be triggered today; item 1 of `docs/repairs/261029/ioservice.md`. Were `object_wait_async` to refuse the re-arm of a connection's wait, `Loop::run()` closed the connection's channel and freed its slot, and the `Server` still held the connection: its index was never handed out again, so one of the program's connections (128 in the root's /boot, 4 in each `Outgoing`) was lost for good, and `Server::channel()` still answered the closed handle for it.

**Cause**
`Server::release()` is the only way a connection is freed, and `LoopServer::handle()` called it only on the paths where it answered `Close`. The comment in `handle()` stated the invariant one way, "nothing releases a connection without closing its slot in the same callback"; the converse, a slot closed with the connection not released, had no owner, because the Loop never told a `Waiter` that it had freed the slot (`docs/changes/261029/loop.md` of this day). The release belongs here and not in the Loop: the Loop does not know the `Server` exists, and only the `Binding` holds the `ConnectionId`.

What `rearm_failed()` must not do. It does not close the channel: the Loop has closed it, and a second close of a handle number could name another object. It does not call the Delegate: `write_failed()` is for a reply that could not be written, and nothing here has a client to answer. It releases `m_connection` of its own `Binding` and no other: the test with two connections shows that releasing another index would not pass.

Why the order holds. The Loop frees the slot and then calls `on_rearm_failed()`, so by the time the entry is free to be bound again the slot is free as well. `watch()` registers a connection's `Binding` again only after `Server::bind()` handed out the freed index, which can only happen after `release()`. `Server::release()` ignores a stale `ConnectionId` (its generation was bumped), so a `Binding` told twice or after the connection was released by another path would be harmless; the Loop tells it once.

**Change**
```cpp
// loop_server.hpp, class Binding
auto on_signals(ours::Signals observed) -> loop::Action override;
auto on_rearm_failed(ours::Status cause) -> void override;

// loop_server.hpp, class LoopServer, private
auto rearm_failed(ConnectionId connection) -> void;

// loop_server.cpp
auto LoopServer::Binding::on_rearm_failed(Status) -> void {
    m_server->rearm_failed(m_connection);
}

auto LoopServer::rearm_failed(ConnectionId const connection) -> void {
    m_server.release(connection);
}
```
The cause is not logged: `ioservice` has no debug-line dependency (`ort`), and a library does not gain one for a message. The program that owns the `LoopServer` has no hook for it either; `Delegate` is deliberately not extended.

**Reference**
None, this project's own design. It uses the notification of `docs/changes/261029/loop.md` of this day.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/ioservice/tests`, then from `out/bringup.x86`: `host/obj/system/lib/ioservice/test/loop_server` 15 passed (13 before, two new), `.../ioservice/test/outgoing` 9, `.../ioservice/test/ioservice` 42.
- The acceptance of the repair item. `ConnectionIsReleasedWhenTheLoopCannotArmItsWaitAgain` serves the root on one connection, makes the next `object_wait_async` fail with `NoMemory` (it is the re-arm after the reply), runs, then checks that the reply was still written, that the server end of the channel is closed, and that `serve()` succeeds `CONNECTIONS` times and then answers `NoResource`. `OnlyTheConnectionWhoseRearmFailedIsReleased` does the same with two connections and `BadHandle` and checks that the other connection still answers a request, and that exactly `CONNECTIONS - 1` more fit.
- Three deliberate breakages, the source put back after each and compared with `cmp`: the Loop does not call `on_rearm_failed()` (`loop.cpp`): both new tests fail, the 13 others pass; `Binding::on_rearm_failed()` does nothing: both fail; `rearm_failed()` releases the connection at index `i ^ 1` instead of its own: both fail.
- `prebuilt/ninja/ninja -C out/bringup.x86` (init, echo and the shell link `ioservice`) and `phony/benches/benches` build. clang-tidy against `out/bringup.x86`, `--header-filter` on the library, on `loop_server.cpp`, `loop_server_test.cpp` and `fake_kernel.cpp`: no diagnostic. `format_cpp.py --check`: clean. `lint_docs.py --changed`: no violations.
- `boot_check.py --out out/lw-notify` with the final tree: dbg PASS, rel PASS, host tests PASS, `routing_checks: passed` and `shell: loop checks passed`; the numbers are in `loop.md` of this day. The root serves /boot, /svc and every component's namespace through `Binding`s, so the override is linked into the boot, but the path itself is not run on the target.
- Not measured: anything on the target for the failing path, which no program provokes.

**Deliberately not done**
- No log line in `rearm_failed()`; see the Change.
- No change to `Server`, `Outgoing` or `BootServer`: they reach a `Binding` only as an array element and pass the array on.
- The fake kernel's `fail_next_wait_async()` fails whatever the next `object_wait_async` is, as the Loop's own fake does. A test that wants one particular wait to fail must make sure nothing arms in between; both tests set it right before `run()`.

**How it was found**
None for the defect; it was written down as latent when `Binding` became a `Waiter` (this file, 2026-10-29).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The old comment in `handle()` said that nothing releases a connection without closing its slot in the same callback. Which new path breaks that sentence as it was, and what does the comment say now?
2. Why does `rearm_failed()` not close the channel, and what would a second close of the same handle number do in the real kernel?
3. `ClientGoneReleasesTheConnection` (older) and the first new test both loop `CONNECTIONS` times to show a release. What does the loop prove that checking `Server::channel()` of the one connection would not, and why does the new test end with one more `serve()` that must answer `NoResource`?

## Title: a provider's refused Open is said on its console, and the reporting hook that makes it possible (2026-10-30)

**Files**
- `system/lib/ioservice/include/ioservice/server.hpp` — `Outcome` of a `Refuse` carries `status`, and `path` (a `ustl::Option<std::string_view>`: the path of a refused Open, none for a refused Clone)
- `system/lib/ioservice/src/server.cpp` — `refuse()` fills `status`; the new `refuse_open()` also fills `path`, and `dispatch()` uses it for Open
- `system/lib/ioservice/include/ioservice/loop_server.hpp` — `LoopServer::Delegate::open_refused(NodeId directory, std::string_view path, Status status)`, a virtual function that does nothing unless overridden, beside `write_failed()`
- `system/lib/ioservice/src/loop_server.cpp` — the `Refuse` case calls it once the client has its epitaph
- `system/lib/ioservice/include/ioservice/outgoing.hpp`, `system/lib/ioservice/src/outgoing.cpp` — `Outgoing` overrides it and writes one line with the vDSO's `debug_write`
- `system/lib/ioservice/include/ioservice/tree.hpp`, `system/lib/ioservice/src/tree.cpp` — `printable_path()`, which makes a client's path fit a log line
- `system/lib/ioservice/test/fake_kernel.hpp`, `system/lib/ioservice/test/fake_kernel.cpp` — the fake kernel defines `debug_write` and keeps what each call wrote
- `system/lib/ioservice/test/server_test.cpp`, `system/lib/ioservice/test/loop_server_test.cpp`, `system/lib/ioservice/test/outgoing_test.cpp`, `system/lib/ioservice/test/tree_test.cpp` — 2, 6, 5 and 4 new tests
- `system/lib/ioservice/BUILD.gn`, `system/lib/ioservice/README.md` — say that `:outgoing` calls `debug_write` and what the hook is
- `docs/repairs/261029/component-manifest.md` — deleted: its only item, 1, is fixed by option (a)

**Symptom**
A provider whose manifest declares `protocol = "ours.examples.echo.Echo"` while its program passes another string to `ioservice::Outgoing::add_protocol()` builds, is routed to without error and logs nothing, and its client reads the epitaph `NotFound` on the channel it connected with. Nothing on the console names the provider, the protocol or the two spellings. Latent today, since every provider in the tree (`examples/echo`, `examples/hello`) spells the name once, from one constant; it is the typo a new provider's author will make, which is what the developer's guide expects its readers to write. `LoopServer` refused every Open it did not accept the same way, silently, whatever the reason (a name that is not there, a path that is not valid, rights the connection does not hold, flags that do not fit the node).

**Cause**
The refusal is decided in the core (`Server::open()`), which touches no channel and prints nothing by design, and carried out in `LoopServer::handle()`, whose `Refuse` case wrote the epitaph and closed the request's channel. At that point the code held the epitaph's bytes and nothing else: the `Outcome` of a `Refuse` named the channel and the encoded message, and not the status inside it or the path the client had asked for. A program told only "an Open was refused" would have had to decode its own epitaph to say why, and could not have said which path at all, since the request's bytes had been read, dispatched and were about to be overwritten by the next message. `Delegate` had one hook, `write_failed()`, for a reply that could not be written; the case the maintainer wants reported, a reply that *was* written and says no, had none.

There was a second reason nothing printed. `ioservice` is host-testable on a fake kernel and has no debug-line dependency (`ort`, which is a user-only target and does not build on the host): the entry above this one, about a failed re-arm, says "a library does not gain one for a message". So even with the information, `Outgoing` had no way to put a line on the console that did not add a dependency.

**Change**
Three layers, each with one job.

The core says what it decided. `Outcome` gains the two fields a report needs; the core still makes no system call and prints nothing:

```cpp
// server.hpp, struct Outcome
ours::Status status = ours::Status::Ok;
ustl::Option<std::string_view> path = ustl::NONE;

// server.cpp
auto refuse_open(Uandle const handle, Status const status, std::string_view const path,
                 ReplyBuffer &out) -> Outcome {
    auto outcome = refuse(handle, status, out);
    outcome.path = ustl::some(path);
    return outcome;
}
```

`path` is an `Option` and not an empty view for a Clone, because an empty path is something a client can send (it encodes, and the server answers `BadPath`): "no path was asked for" and "the path asked for was empty" are different, and the second is reported.

`LoopServer` tells its delegate, after the answer:

```cpp
// loop_server.hpp, Delegate
virtual auto open_refused(NodeId directory, std::string_view path, ours::Status status) -> void;

// loop_server.cpp, case Action::Refuse
auto _ = write(outcome.handle, outcome.message);
auto _ = ours::handle_close(outcome.handle);
if (m_delegate != nullptr && outcome.path.is_some()) {
    if (auto const directory = m_server.node(connection); directory.is_some()) {
        m_delegate->open_refused(*directory, *outcome.path, outcome.status);
    }
}
```

It costs nothing when unused: the base function is empty, a program that does not override it pays one virtual call per refused Open, and a server without a delegate pays a pointer test. The report comes after the epitaph so that a delegate that is slow to print delays nobody's answer; the path still views the request buffer, which nothing has touched since the read. The hook is given the `directory` the Open was made on because the root serves /boot and every component's /svc with one `LoopServer`, and a refused Open on /boot (a missing file) is ordinary while one on /svc is not; the program tells them apart by that node (the root's use of it is in `docs/changes/261029/init.md`). A refused Clone asks for no path and is not reported.

`Outgoing` overrides the hook and writes one line, `ioservice: the outgoing directory refused an Open of "<path>": <status>`, assembled in a stack buffer of the size one `debug_write` takes and written in one call, so it cannot be split by another program printing at the same moment. It calls the vDSO's `debug_write` as it already calls `handle_close`, so a provider's program sets up nothing, `examples/echo`, `examples/hello` and the developer's guide are unchanged, and the library gains no dependency on `ort`.

The path is the client's own bytes, and a name is bytes, not UTF-8: it may hold a newline or an escape. Quoted as it came, it would let a client end the line and write another, which on the root's console would let it forge a line `boot_check.py` looks for (`routing_checks: passed`), or drive the terminal. `printable_path(path, buffer)` copies it into a buffer with every byte outside 0x20 to 0x7E turned into `?`, cut to fit and ended in `...`. It lives in `tree.hpp` beside `is_valid_path()` because both the root and `Outgoing` need it and it is pure, so it is tested on the host.

The options for how an `Outgoing`'s users get the line:
- An optional sink the program sets, with a default of none: no cost and no policy in the library, but a provider hears nothing until its author adds the line, and the typo this exists for is made by authors who do not know to; and `echo` and `hello` would each change.
- `ort::debug_line`, the print helper every user program links: nicer lines (it joins pieces), but a dependency edge from the library to a target that does not build on the host, so the fake-kernel tests could not link `Outgoing`.
- The vDSO's `debug_write`, chosen: the library already calls the vDSO (`handle_close`, `handle_duplicate`), the host tests link a fake of it (`fake_kernel.cpp`), and the line is checked in a test.

**Reference**
None, this project's own design. The hook is `Delegate::write_failed()`'s pattern, of `docs/changes/261028/ioservice.md`.

**Verification**
- `prebuilt/ninja/ninja -C out/log-silent phony/system/lib/ioservice/tests`, then from `out/log-silent`: `host/obj/system/lib/ioservice/test/ioservice` 48 passed (42 before: 2 new in `server_test.cpp`, 4 for `printable_path()`), `.../loop_server` 21 (15), `.../outgoing` 14 (9).
- Nine deliberate breakages, the source put back after each and compared byte for byte: `LoopServer` never calls the hook (3 `loop_server` and 4 `outgoing` tests fail); the null-delegate test is removed from the condition (the `loop_server` binary dies with SIGSEGV); a refused Clone is reported too (traps dereferencing the empty `path`); the hook is called before the answer is written (`RefusedOpenIsReportedWithItsDirectoryPathAndStatus` fails on its `queued_on_watched`); `Outgoing` quotes the path unchanged (the forged-line and the long-path tests fail); the hook gets node 0 instead of the directory (`RefusedOpenNamesTheDirectoryItWasMadeOn` fails); `printable_path()` writes its ellipsis one byte early (its test and the long-path `outgoing` test fail); the core drops the status (tests of all three binaries fail); the printable range ends one byte late (its test fails).
- The line on a real boot, from a scratch worktree with `examples/echo` passing `"ours.examples.echo.Echoo"` to `add_protocol()` and the manifest unchanged (not committed; `boot_check.py --only rel`):

```text
init: started echo
ioservice: the outgoing directory refused an Open of "svc/ours.examples.echo.Echo": NotFound
routing_checks: EchoString through the sibling's expose = NotFound (expected Ok) FAILED
```

  The path is the manifest's spelling, which the root forwards, so it is the one to compare with the string `add_protocol()` was given.
- `boot_check.py --out out/log-silent` with the committed tree: dbg PASS (44 passed, 4 skipped, 0 failed), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required), host tests PASS (151), the counts of the tree before this change. `echo_server` and `hello_server` contain the new line (`grep -a`).
- clang-tidy against `out/log-silent` with `--header-filter` on the library, on the four changed sources, the four test files and `fake_kernel.cpp`: no diagnostic. `format_cpp.py --check` and `lint_docs.py --changed`: clean.

**Deliberately not done**
- No throttle. A client that repeats a refused Open makes the provider print one line of at most 256 bytes (one `debug_write`) per Open, on the provider's own console, which it fills at the pace the client can send, and the provider already answers each of those Opens. This is the same exposure as the root's `log_route_error()`, which logs every failed route unthrottled; a rate limit would need a clock the library does not have and would hide the first line of a real typo behind a counter.
- No program name in the line. The library does not know the program it is linked into and the kernel's debug console adds no prefix. `ioservice:` says where the line came from, the path says what was asked for, and the manifest that declares the path says who provides it. Naming the program needs a setter on `Outgoing` and a line in each provider (`echo`, `hello`), which was not asked for.
- No report of a refused Clone. Its causes (the connection table is full) are another case and have no path to name.
- `Delegate::connect()` and `write_failed()` are as they were. The comment above `Delegate` said its users were `BootServer` and `Outgoing`; the root's delegate is `Realm`, and it says so now.
- The developer's guide (`docs/guide/04-tree-servers.md`, which lists `write_failed()` as the delegate's one other function, and chapter 5's table of what a provider's author sees) is not edited here.

**How it was found**
None for a bug in the code; it was written down as a latent problem in `docs/repairs/261029/component-manifest.md`, item 1 (deleted here), with this fix as its option (a), and chosen on 2026-10-30.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the `Outcome` carry the status and the path, when the hook could decode the epitaph and the request itself? Whose job is each, and what would `Server` have to depend on to do it?
2. Why is `path` an `Option` and not an empty view for a Clone, and what does `RefusedCloneIsNotReported` fail with if the condition is `m_delegate != nullptr` alone?
3. A refused Open's path comes from the client. What can a path contain that a name in a manifest cannot, what could a client do with it on the root's console, and where is that stopped?
