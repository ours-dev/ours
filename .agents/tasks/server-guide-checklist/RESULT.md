# server-guide-checklist: chapter 6 of the developer's guide, a checklist for writing and reviewing a server

**Date**: 2026-10-29

## Delivered
`docs/guide/06-checklist.md`, not committed (the orchestrator commits the guide by explicit paths). It has the one-screen summary of the ten rules that matter most, and 48 items in four phases: design (D1 to D6), writing (W1 to W29), testing (T1 to T6) and review (R1 to R7). Each item is a rule, why it holds and where it is stated or enforced, and each Where line says Enforced or Convention. Fifteen snippets, all copied from real files with their file and line, none longer than 15 lines. The callback is described in words only; no callback signature appears.

Also written: `docs/repairs/261029/component-manifest.md` and `docs/repairs/261029/echo.md` (see "Found along the way").

## Verification
A small Python script (kept in the session scratchpad, not in the tree) checked the file against the tree at commit `693260d0`:

- 138 distinct `file:line` citations: every file exists and every line range lies inside it.
- 280 substring checks over 133 cited ranges: each range contains the words the text relies on. The facts behind them, with the lines they were checked against:
  - the callback's rules (one unit of work, never closes its own handle, never arms a wait, may add, Rearm only after consuming, level-triggered spin): `system/lib/loop/include/loop/loop.hpp:50-61`, the invariants I1, I3, I4 and the one-wait-per-slot rule at `:283-296`, why a key has a generation and why no packet can be withdrawn at `:298-306`;
  - handle ownership at the hand-over points: `loop.hpp:181`, `:189`, `:202-207`; `system/lib/ioservice/include/ioservice/outgoing.hpp:52-54`, `:138-141`; `loop_server.hpp:54-57`;
  - lifetimes: `loop.hpp:63-69`, `:172-173`, `:193-196`; `outgoing.hpp:113`, `:117-118`, `:122-124`; `tree.hpp:47-48`, `:102-103`, `:115-116`;
  - limits: `system/public/ours/syscall.hpp:43`, `:46`, `:59`, `:69`; `system/public/ours/port.hpp:167`; `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28`, `:57-61`; `outgoing.hpp:69`, `:76`, `:128`; `tree.hpp:88-90`; `system/lib/wire/include/wire/wire.hpp:190-194`; a full table's epitaph `NoResource` at `system/lib/ioservice/src/server.cpp:217-222`, `:529-531`;
  - channel signals: PeerClosed is set once when the peer loses its last handle (`channel_dispatcher.hpp:40`), a closed peer's last messages are delivered first (`:67-69`), a message too big for the buffer stays queued (`:69-72`);
  - messages: the unknown-method and malformed rule (`server.hpp:83-90`, `server.cpp:561-565`, `echo_server.cpp:172-191`, `ours_examples_echo.hpp:39-40`), a reply that cannot be written ends the connection (`echo_server.cpp:206-210`, `loop_server.cpp:115-125`), the epitaph (`wire.hpp:35-36`, `echo_server.cpp:77-84`, `:137`, `:141`), a decoded value views the request buffer (`ours_examples_echo.hpp:69-73`, `echo_server.cpp:197-198`);
  - no heap, `constinit`, no virtual destructor: `system/lib/ort/src/cpp_dependencies.cpp:16-35`, `system/lib/ort/src/start.cpp:41-45`, `echo_server.cpp:238-246`, `outgoing.hpp:46-49`;
  - leaving: `echo_server.cpp:214-232`, `loop.hpp:258-259`, the root's exit line and NotFound for an exited provider (`system/service/init/realm.cpp:782-785`, `:849-855`), no stop or kill (`docs/design/261004/component-framework.md:49`, `:392`);
  - the manifest: name characters and length (`system/component_manifest.py:227-238`), `startup` written out (`:450-452`), program under `bin/` (`:411-415`), expose of an undeclared capability refused (`:664-670`), `type` takes one protocol and no offer or expose has one (`:343-349`, `:563-568`, `:655-660`), the build error text (`:1214-1233`, pinned at `system/test_component_manifest.py:749-753`), the root's two log shapes (`realm.cpp:194-203`), the examples (`examples/echo/BUILD.gn:36-48`, `kernel/products/image/dev/BUILD.gn:186-191`, `system/service/init/test/routing_checks/BUILD.gn:85-88`);
  - the name mismatch symptom (an Open of an unserved name answers NotFound): `system/lib/ioservice/test/outgoing_test.cpp:204-216`, and by reading `realm.cpp:809-824` and `loop_server.cpp:163-169`;
  - testing and boot: `scripts/host_tests.py:3-9`, `.agents/skills/ours-verify/SKILL.md:6-31`, `.agents/skills/ours-verify/scripts/boot_check.py:66-71`, `:88-100`, `system/service/init/test/routing_checks/routing_checks.cpp:525-541`, `:793-826`.
- 15 snippets compared line by line with the file they cite: all identical, none over 15 lines.
- 60 links: every in-page anchor matches a heading, and every link to a sibling chapter uses a file name and an anchor from the contract (`01-concepts.md` anchors from the contract's list, `03-protocols.md#closed-protocols`). `docs/development/debugging.md` and the glossary exist.
- `python3 scripts/lint_docs.py --changed docs/guide/06-checklist.md` reports no violations.
- No other operating system is named; no `Callback`, `Waiter`, `on_signals` or `void *context` appears.

## Deviations from the design
- `docs/development/pitfalls/README.md`, named as a source in the plan, is not used: it holds only kernel pitfalls (privilege levels, per-CPU data, the TSS), in Chinese, and nothing about servers. Its advice to boot both images and change one thing at a time is covered by the `ours-verify` skill, which the chapter cites instead.
- The chapter has no item for the transaction id of a reply: it is a protocol rule, and `03-protocols.md` owns it.

## Found along the way
- The code moved under this chapter while it was written. `system/lib/loop/include/loop/loop.hpp` gained the `Waiter` form at `23b6e9e7`, and `ioservice::LoopServer` and the root moved to it in the next commits (`c2b7a75a`, `94a62227`). `examples/echo/echo_server.cpp` still uses the older callback form, so the chapter cites it only for lines that do not show a callback, and every `echo_server.cpp` line number will move when the loop-waiter plan's step 4 lands. The citations have to be re-checked then; the verification script's method (a substring that must appear in each cited range) is what to repeat.
- A provider that serves its protocol under another name than its manifest declares fails silently. Written to `docs/repairs/261029/component-manifest.md`.
- The echo server's refusal paths (unknown method, full table, unwritable reply) are run by no test, although the guide sends readers to it as the model. Written to `docs/repairs/261029/echo.md`.
- `loop.hpp:175-179` leaves the check of the table's size against `PORT_MAX_OBSERVERS` to the caller's `static_assert`, so a server that forgets it meets `NoResource` from `Loop::add` at run time instead of a build error. This is stated, not wrong, and the chapter says so (W18); a template parameter on the table would turn it into a build error. Not written to `docs/repairs`, since it is a design choice to put to the user.

## Not done
Not verified, and so not asserted in the chapter:

- That a global object with a destructor fails to link in a user program. `examples/echo/echo_server.cpp:240-243` says it would register with `__cxa_atexit`, which the runtime does not have; no definition of `__cxa_atexit` is in `system/lib/ort` or `system/public`, but the link was not tried. The chapter gives only the reason `system/lib/ort/src/start.cpp:41-45` states (the constructors of globals never run).
- That `new` is a link error: the chapter says what `system/lib/ort/src/cpp_dependencies.cpp:27-29` says, and no definition of `operator new` exists in the runtime, but no program was linked to see it.
- The sibling chapters were still being drafted when the chapter was checked. Their file names and the anchors it links (`01-concepts.md`'s six, `03-protocols.md#closed-protocols`) match the contract and, at the last check, the headings in `01-concepts.md` and `03-protocols.md`; the chapters may change before the cold read, and the links to them are to whole files except those anchors. What the checklist says they teach (for example that `04-tree-servers.md` covers W28 and W29, and `05-testing-and-debugging.md` T1 to T6) was written from the plan, not from the drafts.
- Nothing was built or booted; the chapter makes no claim about a boot beyond what the cited sources and `ours-verify` say.
