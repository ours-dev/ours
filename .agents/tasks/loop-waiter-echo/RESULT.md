# loop-waiter-echo: the echo example's client is the Loop's Waiter

**Date**: 2026-10-29

## Delivered
Step 4 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, one commit on the branch `lw-echo` (its hash is in the report that handed this step back, since a commit cannot name itself), `refactor(echo): an echo client is the Loop's Waiter`, on top of `693260d0`:

- `EchoServer::Client` in `examples/echo/echo_server.cpp` is `final: public loop::Waiter`; its private `on_signals(observed)` is `server->serve(*this, observed)`. The static `EchoServer::on_signals(void *, Signals)` and its `static_cast` are gone. `connect()` fills the entry with `client.server = this; client.channel = channel;` (the aggregate assignment cannot compile for a class with a virtual function; the form was chosen over a member function and over private fields with a `friend`, see the change record) and registers `m_loop->add(channel, Signals::Readable | Signals::PeerClosed, client)`.
- Every comment that named the callback, the context or the slot's callback names `on_signals()`.
- `docs/changes/261029/echo.md` (new), `docs/repairs/261029/echo.md` (new, one item), this workspace.

## Verification
- `prebuilt/ninja/ninja -C out/bringup.x86` builds everything. Host tests, from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 29 passed, `host/obj/system/lib/ioservice/test/ioservice` 42, `host/obj/system/lib/ioservice/test/outgoing` 9, `host/obj/system/lib/ioservice/test/loop_server` 13, `host/obj/system/service/init/test/autorun` 5, all with 0 failed.
- clang-tidy against `out/bringup.x86`, `--header-filter='.*/examples/echo/.*'`, on `echo_server.cpp`: exit 0, no diagnostic. `format_cpp.py --check`: clean. `lint_docs.py --changed` on each document: no violations.
- What covers the change: echo has no host test of its own. On the target `routing_checks` (rel image) connects to the echo component through `ours.test.Echo`: `EchoString` through the sibling's `expose`, and `Stop`, after which the program exits; `boot_check.py` requires `init: echo exited with 0` and `routing_checks: passed`.
- Deliberate breakage, the source put back after (`cmp` against a saved copy): `Client::on_signals()` answering `Close` at once. Rel image: `routing_checks: EchoString through the sibling's expose = PeerClosed (expected Ok) FAILED`, `routing_checks: Stop = PeerClosed (expected Ok) FAILED`, `2 check(s) FAILED`, `init: routing_checks exited with 1`; `boot_check.py --only rel`: `rel FAIL`, 41 passed, 8 skipped, 3 failed, 4/6 required lines (not seen: `routing_checks: passed`, `init: echo exited with 0`), 0 error lines.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-echo` with the final tree, in a build directory the run configured: build ok in 7.0 s; dbg PASS (shell finished at 5.7 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines, `routing_checks: passed`, `init: echo exited with 0`, `pc-ps2: key a` and `pc-ps2: key A`); host tests PASS (148 passed, 0 failed). Both logs hold `shell: loop checks passed`. The tree holds steps 1 to 4 together (`23b6e9e7`, `c2b7a75a`, `94a62227` and this change), which had not been booted as one tree before.
- `sizeof`, from `-fdump-record-layouts-simple` on the compile command of `echo_server.cpp`: `Client` 16 to 24, `std::array<Client, 4>` 64 to 96, `EchoServer::Storage` 8512 to 8544. `echo_server` (`llvm-size -A`, `llvm-nm -S`): `.text` 25646 to 25694, `.data.rel.ro` 368 to 392, `.rela.dyn` 576 to 600, `.bss` 22992 to 23024, no `.data` section before or after; `ECHO_STORAGE` is type `b` before and after (8512 and 8544 bytes), so nothing moved from `.bss` to `.data`; the stripped image 29440 to 29552 bytes; RW `LOAD` file size 0x2b0 to 0x2c8.
- Not measured: the time of a message through a client, and anything on x86-64 hardware.

### Where the lines of `echo_server.cpp` went
For the developer's guide, which quotes this file by line. Old lines are those of `94a62227`; everything before line 87 is unchanged, and everything after the `Client` class is shifted by 17 lines unless the table says otherwise. The mapping was computed with a line diff of the old file against the new one, and "verbatim" means the lines are the same text.

| Old lines | What they are | Now |
|---|---|---|
| `:52-54` | `MAX_CLIENTS` | `:52-54`, unchanged |
| `:88-95` | `class EchoServer`, `private:`, `struct Client` | `:88-89` unchanged (the comment above, `:86-87`, says "per on_signals()"); the client is now `:90-113`: its comment `:90-99`, `class Client final: public loop::Waiter` `:100`, the two fields `:102-104` (the same text as old `:92-94`), the private `on_signals()` `:106-112`, the closing brace `:113` |
| `:97-108` | `public:` and `Storage` | `:115-126`; the two lines about the buffers' sharing (old `:102-104`) were reworded and are `:120-122` |
| `:118-121` | the static `on_signals(void *, Signals)` | gone, no lines |
| `:126-142` | `connect()` | `:139-159`. Signature `:139`, the search for a free entry `:140-143`, then the old `client = Client{...}` (`:131`) is a three-line comment `:144-146` and the two assignments `:147-148`; the `add()` call (old `:132-133`) is `:149-150` and now ends in `client`; the error path `:151-155` (old `:134-138`) and `close_with_epitaph(..., NoResource)` `:158` (old `:141`) are verbatim |
| `:144-149` | `release()` | `:161-166`, verbatim |
| `:153-192` | `serve()` | `:170-209`, verbatim |
| `:194-232` | `echo_string()` and `stop()` | `:211-249`; one comment line changed, "inside an on_signals()", old `:217`, now `:234` |
| `:238-246` | the comment above the globals and the three globals | `:255-263`, verbatim (`ECHO_STORAGE` is `:262`) |
| `:249-283` | `main()` | `:266-300`; only the comment old `:279`, now `:296`, changed ("from inside an on_signals()") |

The other lines the guide cites, by their old numbers, for those who prefer to look them up: `:7-12`, `:52-61`, `:63-66`, `:69-73`, `:75-84` stay; `:98-99` is `:116-117`; `:100-108` is `:118-126`; `:103-107` is `:121-125`; `:126-130` is `:139-143`; `:132` is `:149-150`; `:132-138` is `:149-155`; `:137` is `:154`; `:141` is `:158`; `:144-149` is `:161-166`; `:154-156` is `:171-173`; `:172-191` is `:189-208`; `:177-191` is `:194-208`; `:188-191` is `:205-208`; `:197-198` is `:214-215`; `:206-210` is `:223-227`; `:214-216` is `:231-233`; `:238-243` is `:255-260`; `:244-246` is `:261-263`; `:264-266` is `:281-283`; `:268` is `:285`; `:273` is `:290`; `:281` is `:298`. The text the guide quotes for `:132` and `:132-138` is what changed: the `add()` call now takes `client` instead of a function and a context, and the entry is filled just above it.

## Deviations from the design
None. The brief's list named `:52-54` among the lines that moved or changed; it did not, since the first edit is at line 87.

## Found along the way
- The saving of this step is in the source, not in the machine code: the compiler had inlined `serve()` into the trampoline, so `EchoServer::on_signals(void *, Signals)` and `Client::on_signals(Signals)` are both 579 bytes. `.text` grew by 48 bytes, all in `main()` (the four vptr stores that `ECHO_STORAGE.init()` now does) less 6 bytes in `connect()`.
- A client's entry stays held when the Loop frees its slot after a failed re-arm. Cannot happen today; the same defect as item 1 of `docs/repairs/261029/ioservice.md`, recorded for this program as `docs/repairs/261029/echo.md` item 1. The root's resource and launcher connections have it too and are not recorded here.

## Not done
- Steps 5 and 6 of `.agents/tasks/loop-waiter/PLAN.md` (`Realm`, and the removal of `Callback`): each waits for the user's go-ahead.
- `docs/guide/`: another session's; the table above lists what to fix there.
- Item 1 of `docs/repairs/261029/echo.md`, which needs the user's decision on the Loop's spec.
