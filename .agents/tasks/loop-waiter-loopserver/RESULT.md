# loop-waiter-loopserver: a LoopServer connection's Binding is the Loop's Waiter

**Date**: 2026-10-29

## Delivered
Step 2 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, one commit on the branch `lw-loopserver`, `refactor(ioservice): a LoopServer connection's Binding is the Loop's Waiter` (its hash is in the report that handed this step back, since a commit cannot name itself), on top of `23b6e9e7`:

- `LoopServer::Binding` is `final: public loop::Waiter` with `on_signals(observed) -> loop::Action override`, defined in `loop_server.cpp` as `m_server->handle(m_connection, observed)`. The static `LoopServer::on_signals(void *, Signals)` and its `static_cast` are gone, and `watch()` registers `m_loop->add(channel, CONNECTION_SIGNALS, binding)`, the Waiter overload. `m_server` and `m_connection` are private to `Binding`; the constructor is still `constexpr`.
- Every comment in `loop_server.hpp` that named the callback or the `void *` context says a Binding's `on_signals()`; the `Binding` comment says what a connection's slot waits on and why the Loop's keeping its address is safe. The README names the Binding as a `loop::Waiter`.
- One new test, `LoopServerTest.EachConnectionIsReachedThroughItsOwnBinding`, with the helpers `request_dirents()` and `expect_dirents_reply()`.
- `docs/changes/261029/ioservice.md` and one repair item, `docs/repairs/261029/ioservice.md`.

## Verification
- Host tests, from `out/bringup.x86`: `host/obj/system/lib/ioservice/test/loop_server` 13 passed (12 before, one new), `.../ioservice/test/outgoing` 9 passed, `.../ioservice/test/ioservice` 42 passed, `host/obj/system/lib/loop/test/loop` 29 passed. The 21 older tests of `loop_server` and `outgoing` are unchanged.
- Which tests say that the Loop reaches a connection through its Binding as a Waiter: every test of `loop_server_test.cpp` and `outgoing_test.cpp` that calls `run()` does, since each serves a connection and answers its message through `Loop::run()` (for example `ReadDirentsIsAnsweredOnTheConnection`, `CloseIsAnsweredAndEndsTheConnection`, `OutgoingTest.OpenOfAProtocolReachesItsHandler`). Four deliberate breakages of `loop_server.cpp` showed what they pin and what they do not, the source put back after each:
  - `on_signals()` answers `Close`: 9 of 12 and 6 of 9 fail.
  - `on_signals()` passes `Signals::None`: 8 and 5 fail.
  - `on_signals()` handles connection `{0, FIRST_GENERATION}`: `OpenOfADirectoryMakesAConnectionOfItsOwn` and `OutgoingTest.ProtocolsReachedThroughAnOpenedSvcDirectory` fail (and the new test).
  - `watch()` registers `bindings[0]` for every connection: no older test failed. The new test fails, and only it. That is why one test was added.
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build) and `phony/benches/benches` build.
- clang-tidy against `out/bringup.x86`, header filter on `system/lib/ioservice`, on `loop_server.cpp`, `loop_server_test.cpp`, `outgoing.cpp`, `outgoing_test.cpp`, `boot_server.cpp` and `echo_server.cpp`: no diagnostic (`boot_server.cpp` only after the full build, which generates its `programs.hpp`). `format_cpp.py --check`: clean. `lint_docs.py --changed` on each document: no violations.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-loopserver` with the final tree: build ok in 6.9 s; dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines); host tests PASS (148 passed, 0 failed). `rel.log` holds `init: echo exited with 0` and `routing_checks: passed`; both logs hold `shell: loop checks passed`.
- `sizeof(LoopServer::Binding)` is 24 (was 16): `-fdump-record-layouts` on the user target. Not measured: the speed of a Binding's dispatch, which step 6 reports on.

## Deviations from the design
- One test added although older tests do exercise the path, because a breakage showed that none of them pins a Binding per connection; see Verification. The brief's rule was to add nothing if they already said it.
- The net saving per connection is not zero in this step but plus 8 bytes: the slot table keeps its two pointers until step 6, so the vptr is paid here and the saving arrives there. `docs/changes/261029/ioservice.md` says so.

## Found along the way
- `Outgoing`, `BootServer`, init and echo reach a Binding only as the element type of `std::array<Binding, N>` in `Outgoing::Storage` and `BootServer::Storage`, which are built by `ustl::LazyInit<...>::init()` in `main()` (init.cpp, echo_server.cpp), where the constructor sets the vptr. Nothing is `constinit` with a Binding in it, nothing copies one, and none of them registers a callback of its own, so none of them changed. Their comments say "a connection's callback", which is still true in the plain sense, and are left.
- A connection stays held in the Server when the Loop frees its slot after a failed re-arm. Cannot happen today; recorded in `docs/repairs/261029/ioservice.md` item 1.

## Not done
- Steps 3 to 6 of `.agents/tasks/loop-waiter/PLAN.md`: each waits for the user's go-ahead.
- The comments of `Outgoing` and `BootServer` that say "a connection's callback" are not reworded.
- Item 1 of `docs/repairs/261029/ioservice.md`, which needs a decision on the Loop's spec.
