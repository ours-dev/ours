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

## Revision

**Date**: 2026-10-29

The revision pass after the cold read (`.agents/tasks/server-guide-review/REVIEW.md`), under the rulings of `.agents/tasks/server-guide/PLAN.md` section 3a. The sections above describe the first draft; the counts here replace theirs. The chapter now has 512 lines (564 before), 48 items with the same ids, 8 code snippets (15 before), and ends "Checked against commit `3cb0d8a5` on 2026-10-29", the hash of `git log -1 --format=%h -- system examples tools scripts build kernel/products`.

Finding by finding (the review's ids; its T1 is not the checklist's T1):

- H3 (rules about a function the reader never saw): fixed. A new heading, "The callback that W4 to W9 bind", sits before W4 and shows the declaration of `loop::Waiter` verbatim (`system/lib/loop/include/loop/loop.hpp:74-83`, 10 lines), says that "the callback" is its `on_signals()`, and links `02-first-server.md`. The old callback signature and `void *` context appear nowhere; the rules W4 to W9 now cite the `Waiter` comments at their new lines. Chapter 1 now shows the same class, so the declaration has two homes; the brief asked for it here.
- H2 (W25, W26 one-line rules): fixed. Both stay one-sentence rules, and their Where lines link `02-first-server.md`, which shows the program target, the manifest and the root-manifest edit. The manifest snippets of W22 and W24 are gone with them (see M10).
- T1 of the review (what a first version needs): added "What a first version needs", a table of thirteen of the 41 items (D1, W1, W2, W5, W6, W8, W17, W22, W23, W25, W26, T4, T5), each with the id and what it protects. The ten rules stay the front page.
- M1 (W2): fixed. Chapter 1 now has `#where-a-servers-memory-comes-from` and shows echo's three declarations, so W2 links it and drops its own copy of the snippet.
- M6 (W13): fixed. The comment-only snippet is gone. W13 points at the real branch, `examples/echo/echo_server.cpp:189-208`, and at chapter 3's `#closed-protocols`, which shows it; W12 keeps its four-line `for` loop that the branch falls to.
- M9 (insider language): fixed. `.agents/` appears on four lines, all the one script path. Gone: the change-record quotation, the `docs/repairs` pointer, the invariant labels, every design-document citation and `docs/changes` and `SKILL.md` in the text and in Sources, the history words of T5, `pc-ps2`, and "skill" (the script is "the verify script"). Because the design documents are no longer cited, one fact had to go: D6 said the root "has no call to stop or kill" a component, which the design gave and the tree contradicts (`task_kill` exists, `system/public/ours/syscalls.gni:820`); D6 now says only what `realm.cpp` shows. The stale design line is written to `docs/repairs/261029/component-framework.md`.
- M10 (density and repetition): fixed. The manifest snippets of W22 and W24, `close_with_epitaph` of W15 and the build-error text of W24 are replaced by links to chapter 1; the five-bullet recap of chapter 1's words is one sentence of links; the `boot_check.py` description of T4 links the README. The opening says that the ten rules are the front page and that the rest is reference, read by phase.
- Minor items: W23's Where line starts with Enforced; W3 opens with "This rule is unusual, because nothing is ever deleted here"; "dev's" is gone (W18 says "the dev image's").
- M4 (words): the intro now defines the root (the root component, `init` the program that runs it), the dev image (`kernel.dev.obi`, linked to the README), the verify script and the callback. Connection means a tree server's connection only (D3, W18); other uses say channel or client. The tree is qualified ("directory tree", `ioservice::Tree`). No sentence says "rights" for a protocol use: T6 says an offer that gives less than a directory use asks for.
- M2 (root): fixed as above.

Not fixed, and why:

- The headings of W14 ("End the connection when a reply cannot be written") and W15 ("Refuse a connection you cannot take with an epitaph") still say "connection". The rulings forbid renaming a heading, and other chapters and the ten rules link their anchors; their bodies say channel and client.
- W24 and T6 quote no log line and no build error. The brief named no boot log to quote from, the lines the chapter used came from a change record and a design document, and the only log in the tree (`out/tmp/ours-verify-bringup.x86/rel.log`) is from 2026-10-07 and has no `init:` lines. They now describe the lines in words, cite the code that prints them (`system/service/init/realm.cpp:188-203`, `system/component_manifest.py:1214-1233`) and link chapter 5, which shows them.

Verification of the revision (a Python script in the session scratchpad, not in the tree, in the method of the first pass):

- 134 distinct `file:line` citations: every file exists, every range lies inside it, and 262 substring checks found in each cited range the words the text relies on. The files that moved since the first draft are `examples/echo/echo_server.cpp`, `system/lib/loop/include/loop/loop.hpp` and `system/service/init/realm.cpp`; every citation into them was re-read and re-numbered. The other cited files are unchanged since the first draft's commit (`git diff 693260d0 HEAD` is empty for them).
- 8 snippets compared line by line with the lines they cite: identical, the longest 10 lines.
- 89 links: every relative link and every anchor resolves except the four to `02-first-server.md`, which did not exist when the check ran.
- `python3 scripts/lint_docs.py --changed docs/guide/06-checklist.md` reports no violations.

Not verified: that anything is built or booted (nothing was); what `02-first-server.md` will show (the chapter says only what the rulings say it shows); that a global with a destructor fails to link and that `new` is a link error (the chapter quotes the runtime's comment, as before).
