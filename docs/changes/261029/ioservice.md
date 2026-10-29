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
