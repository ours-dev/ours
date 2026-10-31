# loop-waiter-notify: the Loop tells a Waiter when it cannot arm its wait again

**Date**: 2026-10-30

## Delivered
One commit on the branch `lw-notify` (worktree `.worktrees/lw-notify`), `feat(loop): a Waiter is told when the Loop cannot arm its wait again`, on top of `a8489d18`; its hash is in the report that handed this task back, since a commit cannot name itself.

- `loop::Waiter` has a second virtual function, `on_rearm_failed(ours::Status cause)`, with an empty inline body. `Loop::run()` step 4 calls it, once, for a re-arm that failed, after it has closed the slot's handle, bumped the generation and freed the slot. It is not called after `Close`, when `add()` fails, or by the destructor. `loop.hpp` stays the whole spec: the `Waiter` comment, steps 3 and 4 of `run()`, I1 and I3 say it.
- `LoopServer::Binding`, `ResourceConnection`, `LauncherConnection` and echo's `Client` override it and release their entry the way their `Close` paths do (the launcher's through `release()`, which also resets its state). The three programs log the cause with the debug line they already use; `Binding` does not. `OutgoingWaiter` and `TerminatedWaiter` keep the default and their comments say why.
- Six new loop tests, two new `loop_server` tests, and `fail_next_wait_async()` in the ioservice fake kernel. Item 1 of `docs/repairs/261029/ioservice.md` and item 2 of `docs/repairs/261029/echo.md` are deleted; the root's pools, which were never listed, are recorded in `docs/changes/261029/init.md`.
- `docs/changes/261029/loop.md`, `ioservice.md`, `init.md`, `echo.md` have the new sections, and `docs/repairs/261029/abi.md` has one new item (see "Found along the way").

## Verification
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build, `-Werror`) and `phony/benches/benches` build. Host tests, from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 27 passed (21 before), `host/obj/system/lib/ioservice/test/loop_server` 15 (13), `.../ioservice/test/outgoing` 9, `.../ioservice/test/ioservice` 42, `host/obj/system/service/init/test/autorun` 5, all with 0 failed.
- Mutation checks, the source put back after each and compared with `cmp`. `loop.cpp`: the call removed, 4 loop tests fail and the 2 new `loop_server` tests; the guard removed so that `Close` is told too, 5 loop tests fail; told before the close, 3; told with `Ok` instead of the cause, 2; told after the close and before the slot is cleared, 1 (`RunLetsTheWaiterAddFromOnRearmFailed`, the test with a full table). `loop_server.cpp`: `Binding::on_rearm_failed()` doing nothing, and `rearm_failed()` releasing the connection at index `i ^ 1`, each fail both new `loop_server` tests and leave the other 13 passing.
- clang-tidy against `out/bringup.x86`, `--header-filter` on `system/lib/loop`, `system/lib/ioservice`, `system/service/init` and `examples/echo`, on `loop.cpp`, `loop_test.cpp`, `loop_server.cpp`, `loop_server_test.cpp`, `fake_kernel.cpp` (ioservice), `resource_server.cpp`, `launcher_server.cpp`, `realm.cpp` and `echo_server.cpp`: exit 0, no diagnostic. `scripts/format_cpp.py --check`: clean. `scripts/lint_docs.py --changed` on each document: no violations.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-notify`, with the final code, in a build directory the run configured: build ok in 7.4 s; dbg PASS (shell finished at 5.7 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.9 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines); host tests PASS (148 passed, 0 failed). `rel.log` holds `routing_checks: passed`, `init: echo exited with 0` and `pc-ps2: key a`; both logs hold `shell: loop checks passed`; no log holds `cannot wait on a client again`.
- What covers the resource server, the launcher server and echo: they call the kernel and have no host test, so the compile (`override` makes a wrong signature fail to build) and the boot, which runs all three. The failing path cannot be provoked on the target, because no program breaks the rules that would earn `BadHandle` and the kernel refuses `object_wait_async` for none of them; the Loop's side of it is pinned by the host tests with the fake kernel, and each owner's side by the same pattern in `Binding`'s tests.
- Sizes (`llvm-size -A` and `llvm-objcopy --strip-all`; before is a build of `a8489d18`, after is this tree, same toolchain):

| | `init` before | after | `echo_server` before | after |
|---|---|---|---|---|
| `.text` | 108,878 | 109,358 | 25,294 | 25,470 |
| `.data` | 51,360 | 51,360 | none | none |
| `.bss` | 55,296 | 55,296 | 22,960 | 22,960 |
| `.data.rel.ro` | 2,296 | 2,624 | 392 | 408 |
| `.rela.dyn` | 5,712 | 6,696 | 600 | 648 |
| stripped file | 177,776 | 179,656 | 30,240 | 30,512 |

  The second virtual function itself is 48 bytes of `.data.rel.ro` and 144 of `.rela.dyn` in `init` (five vtables, six slots) and 16 and 48 in echo, with no per-object cost. The rest of `init`'s growth in those two sections, 280 and 840 bytes, is a table of `ours::to_string(Status)` that changed form between the two builds; see "Found along the way" and `docs/changes/261029/loop.md`.

### The lines of `examples/echo/echo_server.cpp` that changed
For the developer's guide and `examples/hello`, which copy this file. The diff is 19 lines added and 1 removed, in three hunks (old `:99` is replaced by four lines); everything else is the same text. Old lines are those of `a8489d18`, new lines those of this commit.

| Where (new lines) | What |
|---|---|
| `:99-102` | the comment of `Client`: its last sentence (old `:99`, "thread, finds the entry free only after that.") now goes on and says that on a failed re-arm the Loop closes and frees first and then calls `on_rearm_failed()`, "so the order is the same" |
| `:116-122` | new: a blank line, the comment of `Client::on_rearm_failed()` (`:117-119`) and the override (`:120-122`), `server->rearm_failed(*this, cause)`, inside `Client`'s private section, right after `on_signals()` and before `};` (`:123`) |
| `:178-185` | new: the comment (`:178-180`), `EchoServer::rearm_failed(Client &, Status)` (`:181-184`: `client.channel = Uandle::Invalid;` and `report("cannot wait on a client again", cause);`) and a blank line, between `release()` and `serve()` |

Where the old lines went: old `:1-98` are the same; old `:100-112` (the `Client` class from `class Client final: public loop::Waiter`, its two fields, `on_signals()`) are `:103-115`; old `:113-167` (the closing of `Client`, `public:`, `Storage`, the constructor, `connect()`, `release()`) are `:123-177`; old `:168-end` (`serve()`, `echo_string()`, `stop()`, the members, the globals and `main()`) are `:186-end`. The anchors the guide cites most, old to new: `MAX_CLIENTS` `:54` same; `report()` `:64` same; `close_with_epitaph()` `:77` same; `class Client` `:100` is `:103`; `Client::on_signals()` `:110-112` is `:113-115`; `Storage` `:118` is `:128`; `connect()` `:139-159` is `:149-169`; `release()` `:163-166` is `:173-176`; `serve()` `:170` is `:188`, and its refusal branches `:189-208` are `:207-226`; `echo_string()` `:212` is `:230`; `stop()` `:241` is `:259`; `OUTGOING_STORAGE` and `ECHO_STORAGE` `:261-262` are `:279-280`; `main()` `:266` is `:284`. The text the guide quotes about how a `Waiter` frees an entry (`release()` and the failed `add()` in `connect()`) is unchanged; a copy of the file gains the two additions above.

## Deviations from the design
- `run()` keeps `auto &waiter = *slot.m_waiter;` before it clears the slot, not a copied pointer: the same object, and a reference reads as the invariant that a slot in use has a `Waiter`. The status is kept in `rearm`, and the condition for telling is `action == Action::Rearm` at that point, which can only be a failed re-arm.
- The default is written `virtual auto on_rearm_failed([[maybe_unused]] ours::Status const cause) -> void {}`: the repository's style asks for `const` and `[[maybe_unused]]` on an unused parameter.
- The ioservice fake kernel did not have `fail_next_wait_async()` (only the Loop's did); it gained one with the same meaning, since the acceptance of the repair item needs it.
- The existing test `RunClosesTheSlotWhenTheRearmFails` expected the log `ab` and now expects `a!ab`: its `Client` is told, which is the new behaviour. Nothing else of an older test changed.
- The cause is logged by the resource server, the launcher server and echo, and not by `Binding`: the brief allowed it where a debug line is at hand, and `ioservice` has none.
- The comment in `run()` step 4 that said the close "cannot be refused" now names its one exception (a `Waiter` that closed its own handle, `BadHandle`). It is not a code change.
- The surviving repair items keep their numbers (`ioservice.md` has 2 and 3, `echo.md` 1 and 3), because the guide's task records and other documents cite them by number.
- The brief did not ask for `docs/repairs/261029/abi.md`; it records what the size measurement turned up.

## Found along the way
- `ours::to_string(Status)` compiled to a different table in `init`'s kept copy: 35 absolute addresses in `.data.rel.ro` with 35 relocations, in place of 35 32-bit offsets in `.rodata`. It added 1,120 bytes to `init`'s file in this build. Not a defect of this change, and not fixed: `docs/repairs/261029/abi.md` item 1, with options for the user.
- The kernel's references in the brief for the `arm()` failures are right but point at different functions than their order suggested: `NoMemory`, `BadHandle`, `BadType`, `AccessDenied` and `InvArgs` come from `HandleTable::attach_port_observer()` (`handle_table.cpp:218`), `NoResource` from `PortDispatcher::register_observer()` (`port_dispatcher.cpp:341`). The change record cites them that way.

## Not done
- The developer's guide and `examples/hello`: the next step, from the table above.
- A check of the new path on the target in the shell's `check_loop()`. Only a `Waiter` that breaks the rules (closes its own handle and answers `Rearm`) reaches it on the real kernel; the host tests pin the Loop's side. Recorded in `docs/changes/261029/loop.md`.
- A host test for the resource server, the launcher server and echo (`docs/repairs/261029/echo.md` item 1 is the open item for echo).
- The `ours::to_string` table, above.
