# server-guide-review-2: cold read of docs/guide by an outside C++ developer

**Date**: 2026-10-29

## Scope and method

Reader: a C++ developer who knows Git and a build system, has never seen this kernel, and wants a server running in the dev image. Input: `docs/guide/` (README, 00 to 06, 43,360 words) and the contract (`.agents/tasks/server-guide/PLAN.md`, "### 1. The contract", read first). No earlier review and no `RESULT.md` was opened. Nothing was built or booted.

Order of reading: the README's own order, then the rest. README, 00-setup, 01-concepts, 02-first-server, 03, 04, 05, 06. Chapter 2 was followed level by level against the real files: `examples/echo/*`, `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/{BUILD.gn,routing_checks.cpp}`, `.agents/skills/ours-verify/scripts/boot_check.py`, `system/lib/wire/test/ordinals_test.py`.

The path the README recommends before the tutorial can start (README "How to read it" and "Where to look next"): README 2,157 words, then 00-setup 1,662, then 01-concepts 5,971, which is 9,790 words, and chapter 2 itself is another 4,535. The README asks for chapter 3 "beside it" and chapter 5 "at hand" as well.

Verified, not only read:

- Every `file:line` range chapter 2 cites for `echo_server.cpp`, `BUILD.gn` (echo, dev, routing_checks), `routing_checks.cpp` and `boot_check.py` is right at the working tree, including the subtle "closed at line 204" (`02-first-server.md:343`).
- Level 0 was run in the scratchpad: copy, delete `test/` and `README.md`, cut the `tests` group, apply the rename table in order, then `grep -ri echo`. It prints nothing. The ordinal of `ours.examples.hello/Hello.Greet` is `0x0b34f151d5b5de80`, as the chapter says.
- Every relative link and anchor in `docs/guide/` resolves. The counts in chapter 6 (48 items, 41 in D/W/T, thirteen first-version items) are right.

Not verified: whether any level builds or boots (reasoned from the code only), and every command of `00-setup.md`.

## What is done well

- `02-first-server.md:31-45`, "The places you will touch": seven places, where, copy or write, which level. The best orientation in the guide. Keep it as the first thing after "Before you start".
- Every level of chapter 2 ends in a "Verification" that names the exact log lines (`:154-162`, `:294`, `:396-403`, `:470-476`, `:504-510`). Level 2's "to see that the check has teeth" (`:294`) and Level 5's "A check you have never seen fail proves nothing" (`:510`) are the guide's best teaching device.
- Level 0 is exact and works as written (verified above). The warnings sit exactly where a reader would fall: `grep -a` (`:29`), the `ort/main.hpp` link error with its text (`:102`), "Keep `MAX_CLIENTS` at 1" with the 60 s hang it prevents (`:464`), "spelled in six places" (`:306`).
- `README.md:75-92`: a real passing run, then "Read it like this" line by line. A reader can tell a pass from a fail on the first run.
- `01-concepts.md:156-161` ("Four consequences"), `:375-379` (rights have three meanings), `:422-431` (the three-hop diagram of an Open), `:473` (the two things called svc), `:203-240` (why there is no heap, answered before the reader trips on it).
- `05-testing-and-debugging.md:371-395` ("Four questions" and the lookup table keyed by what the client sees and what the root logged): the troubleshooting page exists and is the right shape.
- `06-checklist.md:22-33` and `:55-63`: ten rules first, then Rule/Why/Where per item, with "Enforced" and "Convention" told apart.
- The guide says what it cannot do, early: `README.md:14,29,49-50`, `00-setup.md:126-132`, `04-tree-servers.md:5`. No reader is misled about tree servers.
- Insider language is almost absent (see below): no plan or step names, no "the user", no repair or change-record pointers in the prose.

## High severity

### H1. The setup chapter cannot be completed from its text, and everything after it needs a boot

`00-setup.md:44-83` ("Getting each piece") and `:126-132` ("What this chapter could not check"). On any host but an Apple-silicon Mac the reader has nothing (`:23`: "has not been checked on Linux, and does not describe it further"). On that Mac, two of the four tools cannot be obtained from the text: the macOS LLVM archive is not named ("the one for macOS on arm64 is listed beside it", `:46`) and it is a release candidate (`23.1.0-rc3`); QEMU and GRUB are "Homebrew packages, extracted" but "How the packages were extracted is not recorded in the tree" and "this chapter has not run it" (`:73`); the gn and ninja commands use `latest` and "were not run again" (`:67`). `scripts/activate.py` is said to fetch what a `prebuilt.yml` lists, but "The tree does not ship a `prebuilt.yml`" (`:103`). The guide is honest about all of this, but honesty does not unblock the reader: no level of chapter 2 can be tried without the first boot.

Fix: this is not a wording problem. (1) The maintainers need a script or a pinned `prebuilt.yml` that someone has run on an empty `prebuilt/`; the guide should then say `python3 scripts/activate.py` and nothing else. (2) Until then, run each command once and write down the exact asset file names, formula names and versions, and the extraction commands. (3) Put three lines at the top of the README, before "What you will be able to do": what you need (Apple-silicon Mac, about N GB, these tools), and "if `ls prebuilt/` is empty and you cannot get them, stop here: you cannot run anything in this guide". (4) For Linux, say whether the dev container plus the missing four tools would work, or say that it is unsupported.

### H2. The path before the tutorial is 9,790 words long, and the README never names a short one

`README.md:113-123` ("How to read it"), `:134` ("Where to look next") and `02-first-server.md:14` ("Read 01-concepts.md first"). A newcomer is told to read the README, set up the tools, run the build, and read chapter 1 "once, in order": 9,790 words before the first edit. Level 0 needs only the first three sections of chapter 1 (handles and channels, ports and signals, the Loop: 2,080 words); the other four (components and manifests 1,227, `/svc` and the outgoing directory 935, memory 457, epitaphs 449) are needed at Levels 1 and 2, and each is linked from there. The skip list is at the very end of the README (`:125-130`), covers chapters 3 to 6 only, and gives no sizes. A reader who sees six chapters and three "read first" instructions will either read everything or give up.

Fix: a "Shortest path" box at the top of the README: README (Status, Before you start, Build and boot) then `00-setup.md`, run the build once, then chapter 2 with "read now" links at the level that needs them: before Level 0, three sections of chapter 1 (about 2,100 words); at Level 1, "Components and manifests"; at Level 2, "`/svc` and the outgoing directory", "Where a server's memory comes from" and "Epitaphs". State the sizes in words. Change `02-first-server.md:14` to match. This moves about 3,000 words of chapter 1 from before the tutorial to inside it, and a reader gets a booted `hello: started` after about 5,500 words in all.

### H3. The tutorial has no known-good file to compare with, and its riskiest edits are given as line ranges

`02-first-server.md:232-238` (Level 2 table), `:337-341` (Level 3), `:460` (Level 4). At Level 3 the reader assembles `hello_server.cpp` from `echo_server.cpp` by line numbers ("lines 86-229 and 251-253, leaving out lines 200-203"), renames about fifteen identifiers (M3), and writes a client of several dozen lines in a 914-line file (M2). If the build fails the reader has no file to diff against. Only Level 2 has an "If it fails" table (`:296-304`); Levels 3 and 4, where the edits are largest, have none. The line numbers are correct only at one commit: the chapter ends "Checked against commit `3cb0d8a5`" and the guide never tells the reader to check that commit out. The contract ("no examples of its own", PLAN decision 2) caused this, and the cost falls on the reader.

Fix: keep a finished copy of each level in the tree and compile it in the build so it cannot rot: for example `examples/hello/` as the Level 5 result, and one `hello_server.cpp` and one `routing_checks` diff per level under `docs/guide/hello/level-N/`, or one git tag per level. Say "if your build fails, diff your file against level N". Say in "Before you start" which commit the line numbers are for and how to get it. Add an "If it fails" table to Levels 3 and 4 (an undeclared `ours_examples_hello`, a missing `[[noreturn]]` function body, the 60 s hang of `:464`).

## Medium severity

### M1. The header include in `routing_checks.cpp` is never told, and the Level 2 failure table names the error early

`02-first-server.md:302` (table row) and `:360-394` (Level 3 client). Level 3 asks for `ours_examples_hello::encode_greet` and `decode_greet_reply` in `routing_checks.cpp`, which needs `#include <ours_examples_hello/ours_examples_hello.hpp>` beside the echo include (`routing_checks.cpp:68`). No sentence says so. The row at `:302` ("`'ours_examples_hello/ours_examples_hello.hpp' file not found` in `routing_checks.cpp` | `routing_checks` does not depend on your library") describes an error that cannot happen at Level 2, where the reader has written no include; the error a reader who forgets the include at Level 3 actually gets (an undeclared name) is in no table.

Fix: in Level 3, "add `#include <ours_examples_hello/ours_examples_hello.hpp>` after line 68; it resolves because of the dependency you added at Level 2". Move the table row there and add the undeclared-name error.

### M2. Four pieces of code are described only in words, and the chapter says there is one

`02-first-server.md:45` says "The one piece of code in this chapter that echo does not already contain is the short `main` of level 1." The reader also has to write: the stub `HelloServer` (`:228`, prose), Level 2's `check_hello` (`:292`, prose), Level 3's `check_hello` (`:394`, prose: "prints ... if the count of failures, `FAILURES`, did not grow"), and Level 4's client extension (`:462-468`, three bullets). A reader who trusts "code under a `file:line` is copied exactly" looks for snippets that are not there. Level 4 is the worst: Level 3 ends `check_hello` by closing the channel, so the three bullets must be put in an order the text does not state (connect A and greet; connect B and expect `NoResource` while A is open; unknown method on A and expect `PeerClosed`; connect C and `stop_hello`), and a wrong order hangs the boot for 60 s without a `FAILED` line (`:464`).

Fix: correct the sentence at `:45`. Give the stub class and Level 2's `check_hello` as four-line snippets (Level 1's `main` is the precedent for invented code). Give Level 4's client as numbered steps in order, or as the roughly 25-line function.

### M3. "Renamed" is never defined for the server code

`02-first-server.md:337-341` ("Replace your stub class with echo's `EchoServer`, renamed"), `:356` ("`greet` (lines 211-229)"), `:460`. Level 0 gives a rename table for the library. The pasted class has about fifteen echo identifiers (`ECHO_STRING`, `decode_echo_string`, `EchoStringRequest`, `encode_echo_string_reply`, `STOP`, `decode_stop`, `StopRequest`, `encode_stop_reply`, the method `echo_string`, `EchoServer`, `ECHO_STORAGE`, the string `"echo: stopping, as a client asked"`). The chapter calls the method `greet` though echo's file has no function of that name, and Level 4's verification expects the line `hello: stopping, as a client asked` (`:473`), which only a correct rename of that string produces.

Fix: "apply the Level 0 table to what you paste, and also `echo:` to `hello:`, `EchoServer` to `HelloServer`, `ECHO_STORAGE` to `HELLO_STORAGE`, and the method `echo_string` to `greet`", or list the lines.

### M4. Level 2 carries far more than "one or two ideas"

`02-first-server.md:3` ("adds one or two ideas") and `:166` ("Two ideas are new: serving a protocol, and where a program's memory comes from"). In Level 2 the reader meets the port, the Loop, slots, `Outgoing`, `Handler`, an epitaph, `constinit`, `LazyInit`, `capabilities`, `expose`, `offer`, `use`, a client check, and edits five files (rows 2 to 6 of the table at `:35-43`). It is also the first level whose verification is mostly an absence of lines (`:294`). If anything goes wrong the reader cannot tell which of five files is at fault.

Fix: split into Level 2a (the program serves its outgoing directory and refuses every client; verify with `init: started hello` and no `init: hello exited`, because it now stays up) and Level 2b (the manifests, the offer, the client, the epitaph check).

### M5. Chapter 5 teaches how the libraries' own tests work, not how to test the reader's server

`05-testing-and-debugging.md:37-201` ("Tests on the host", about 1,800 words): fake kernels, the `ioservice` and `loop` tests, a negative case from `server_test.cpp`. The chapter also says (`:33`) that a server like `hello` has no host test because its `BUILD.gn` asserts a user toolchain. What the reader can host-test is the codec, already covered by chapter 3, steps 6 and 7. The fake kernels are test-local to `system/lib/ioservice/test` and `system/lib/loop/test`, so they cannot be reused. The title promises "Testing a server", and the tutorial's own tests are boots.

Fix: say at the top of the chapter that for a server of yours host testing means the codec, and that the rest of the host section is how the libraries are tested (skip unless you change one). Better, move it out. If there is a way to split a Waiter-based server so its decisions run on the host (`:35` advises it), show one real example; if there is none, say so.

### M6. There is no client of the reader's own

`02-first-server.md:358-394` ("The client"). The client of `hello` is a check inside `routing_checks.cpp`, and the helpers it uses (`Incoming`, `call`, `connect_or_fail`, `routing_checks.cpp:325-438`) sit in that file's anonymous namespace. The README's goal is "a program that other programs reach over channels"; a reader whose own program should call `hello` gets no recipe for the other end: a second component, its manifest with a `use`, its child entry and offer, `fsio::connect`, the read, the epitaph test, closing the handles.

Fix: add a short Level 6, "a client of your own": a minimal `examples/hello_client` with its `component_manifest` (a `use`), the root's child entry and offer, and about 40 lines that connect, call `Greet` and print the answer. Move `Incoming` and `call` into a small library, or tell the reader plainly that they must be copied.

### M7. Two recipes for the same step

`02-first-server.md:55-68` (Level 0: copy and rename) against `03-protocols.md:249-468` ("How to add a new protocol library", about 1,500 words, with a copier route that Level 0 does not use). The README says to follow chapter 2 "with chapter 3 beside it for the protocol" (`README.md:117`). A reader who does so meets a different recipe for creating `examples/hello/protocol` and no statement of which one to use. Chapter 3 mentions chapter 2's shortcut only at `:261`, after the reader has read the steps of "How to add".

Fix: README, "chapter 3 when you add a second method or a different shape"; Level 0, one sentence: "chapter 3's recipe is for a protocol that is not a copy; skip it for `hello`".

## Minor and style

- m1. `02-first-server.md:314`: the sentence ends in a colon and the next paragraph is prose ("...with the signals it saw:" then "Its whole declaration is in chapter 1"). Same in `06-checklist.md:151-153` ("The declaration is `...:74-83`:" then "Its declaration is in chapter 1"). A snippet was replaced by a pointer and the lead-in stayed. Delete the first sentence or the colon.
- m2. `02-first-server.md:104`: "delete its comments, and ... delete ... (lines 24-28 and 36-48)". The line numbers are echo's; if the reader deletes the comments first they are wrong. Say "numbers are the original file's; delete the comments last".
- m3. `02-first-server.md:59`: "Rename `include/ours_examples_echo` and the header and the source inside it". The source is `src/ours_examples_echo.cpp`, not inside `include/`.
- m4. The copy brings insider comments into the reader's file: `ours_examples_echo.hpp:14-15` ("component-framework design D20; PLAN step 17"), `:21` and `:24` ("design D23"), `protocol/BUILD.gn:5`. `grep -ri echo` passes, and the reader now has "PLAN step 17" in their header. Tell them to rewrite the header comment.
- m5. `02-first-server.md:478`: "a full table is its row 10, an unknown method its row 13". Chapter 5's table (`05-testing-and-debugging.md:384-395`) has ten unnumbered rows; "10" and "13" are numbers of causes in the list below it. Write "cause 10" and "cause 13".
- m6. `README.md:130` ("In chapter 6, everything after 'The ten rules that matter most'") against `02-first-server.md:512`, which sends the reader to "What a first version needs", a section that comes after the ten rules (`06-checklist.md:35`). Make the README say "the ten rules and 'What a first version needs'".
- m7. `02-first-server.md:29`: `<dir>` is not defined; the run prints `.../ours-verify-hello/{rel}.log`. Say "`<dir>` is the directory the last line of the output names (`.../ours-verify-hello`)".
- m8. `02-first-server.md:510`: "The full command, with the host tests, now ends in `8/8 required`". The reader has used `--only rel` throughout; say to run without `--only`, and that the `dbg` line is also printed.
- m9. Words with two meanings that the contract does not fix: `client` (the program that calls a server, but also echo's `Client` entry, `02-first-server.md:320` against `:358`, and "client entry" in `03-protocols.md:622`); `serve` (the method that reads one message, `02-first-server.md:343`, against "serves its outgoing directory", `:170`); `port` (the kernel object, and the "I/O port" in `05-testing-and-debugging.md:253`).
- m10. `05-testing-and-debugging.md:462` and `06-checklist.md:481` send the reader to `docs/development/debugging.md`, "written in Chinese". It is a dead end for the audience, and the chapter says nothing about how to see a crash in a user program. Say that there is no English text, or write the ten lines needed.
- m11. Kernel-developer material in a server guide: `05-testing-and-debugging.md:82` (`//kernel/tests/fake-kernel`, a different thing), `:322-324` (`kshell_run.py`, `vmosnapshot.test`). Remove.
- m12. `00-setup.md` speaks in the reference machine's voice ("fetched on 2026-08-24", "were not run again", "the machine used for this chapter"). One "Unverified" list at the top would be shorter and more useful than the same caveat in five places.
- m13. `03-protocols.md:747`: "This guide found no written policy in the repository for changing a protocol." The guide is talking about its own process. Say "There is no written policy."
- m14. `.cm` is never expanded (`01-concepts.md:249` says the manifest compiles to `meta/<name>.cm`); "component manifest" would do.
- m15. The README's `out/bringup.x86` and the tutorial's `out/hello` both appear; `03-protocols.md:467` runs a host test from `out/bringup.x86`, which a reader who followed chapter 2 does not have. Say "your build directory".

## The tutorial walk, level by level

The question for each level: is every edit precise, is anything assumed, is the test believable?

- **Level 0.** Precise and correct (verified). The rename table, the order of its rows and the ordinals test all work. Verification is believable and honest about its limit (`02-first-server.md:84`). Stuck points: none that block (m3, m4).
- **Level 1.** Precise: `hello_server.cpp` is given whole, `BUILD.gn` is made by copy, replace and delete with line numbers (m2), the child entry's lines (`dev/BUILD.gn:94-98`) are right. Assumed and not said: that `component_manifest` without a `manifest` is legal (it is: `component_manifest.gni` lists `manifest` as optional). Verification (`init: started hello`, `hello: started`, `init: hello exited with 0`) is believable. Cannot fail on the text.
- **Level 2.** The big table (lines 27-50, 56-61, 63-84, 255-264, 266-300) is precise. Guesses: the stub class (prose, M2), where `check_hello` goes (it must follow `connect_status`, which ends at `routing_checks.cpp:788`; unstated), the offer entry's `protocol`/`to` lines (implied by "modelled on echo's"). Verification is mostly absences, but the "teeth" test makes it believable. The failure table names an error that cannot occur yet (M1). Too much in one level (M4).
- **Level 3.** The class by line ranges is precise but the rename is not (M3); the header include is missing (M1); `HELLO_TEXT` has no value (any string works, not said); `check_hello` is prose (M2). Nothing says what to do when the build fails here (H3). Verification is believable.
- **Level 4.** Restoring `serve`'s lines and `stop` is precise (lines 200-203, 231-249). The client extension is the weakest passage of the chapter (M2). A reader who gets the order wrong waits 60 s for a hang. The wording "row 10, row 13" is wrong (m5).
- **Level 5.** Precise (`boot_check.py:90-100` is right) and the best level: add two lines, remove a call, watch the boot fail. The only gap is m8.

Points where I would have to guess or would produce something that fails, in order of reading: (1) the macOS archive and the QEMU and GRUB extraction (H1); (2) what "renamed" covers at Level 3 (M3); (3) the missing `#include` (M1); (4) where `check_hello` goes and what its Level 2 and Level 3 bodies are (M2); (5) the order of Level 4's client (M2); (6) the `hello: stopping...` string for the verification (M3); (7) whether to diff against anything when the build fails (H3).

## Terms used before they are defined, or never defined

| Term | First used | Defined |
|---|---|---|
| protocol | `README.md:11` ("define its protocol") | `01-concepts.md:48` (loosely), `:256` (formally); the README defines "protocol server" (`:28`) without it |
| the root | `README.md:11` | `README.md:60` ("`init`, the root"), `01-concepts.md:250` |
| host, host test | `README.md:73` | `05-testing-and-debugging.md:17-18` |
| serial log | `02-first-server.md:29`, `README.md:73` | never (it is QEMU's console output) |
| codec, encoder, decoder | `02-first-server.md:37,57` | `03-protocols.md:42` |
| selector, ordinal, `TwoWay` | `02-first-server.md:70-79` | `03-protocols.md:35-42`; Level 0 links the anchor |
| `Result`, `is_ok`, `unwrap_err` | `02-first-server.md:327-333` | `03-protocols.md:48` |
| statuses (`NotSupported`, `NoResource`, `PeerClosed`, `InvArgs`, `BadType`, `AccessDenied`, `BadState`, `ShouldWait`) | `01-concepts.md:25` onward | never as a list; each is glossed once where it first matters, and the troubleshooting table is keyed by them |
| user toolchain, host toolchain | `02-first-server.md:104` ("programs that run in user mode") | `05-testing-and-debugging.md:60`, `06-checklist.md` W26; why one library builds twice is never said in chapter 2 |
| closed protocol | `01-concepts.md:526`, `02-first-server.md:420` | `03-protocols.md:608`; both link it |
| built-in | `01-concepts.md:254` | `01-concepts.md:533` (the last section); linked |
| ours.io rights | `01-concepts.md:378` | `ours.io` itself is defined later, at `:395` |
| `.cm` | `02-first-server.md:104` | never expanded |

Words with two meanings: m9, and the three meanings of "rights" and the two of "svc", which chapter 1 handles well (`:375-379`, `:473`).

## Skipped steps, leaps and contradictions

- Leap: Level 1 to Level 2 (M4). Leap: Level 3 to Level 4 in the client (M2).
- Contradiction: the README's skip list against `02-first-server.md:512` (m6).
- Contradiction: `02-first-server.md:45` against the chapter's own content (M2); `:3` against Level 2 (M4).
- Contradiction: "row 10" against chapter 5's cause numbers (m5).
- Two recipes for one step (M7).
- Not said, and a reader will wonder: that `hello` need not be added to `examples/BUILD.gn` (which lists `echo:echo_server`); that the tutorial edits the project's own test component and the dev image for good, and how to keep a server out of them.

## Insider language

The guide is almost clean. The only `.agents/` path in the prose is the verify script. There are no plan names, step names, design-document numbers, "the user", repair or change-record pointers. What remains:

- Inherited by the copy in Level 0: m4.
- Internals of the root and the kernel: chapter 4 (`pools.hpp`, `BootServer`, `Realm`, `init.cpp:272`, about 44 `system/service/init/` citations), chapter 5 (`kshell_run.py`, `//kernel/tests/fake-kernel`, `vmosnapshot.test`), see m11.
- "Checked against commit `3cb0d8a5`" at the end of every chapter: provenance for the maintainer, not for the reader, except where line numbers matter (H3).
- "A known limit" (`04-tree-servers.md:118,535`, `05-testing-and-debugging.md:402`): a limit nobody tells the reader how to look up.
- A Chinese-only pointer: m10.
- About 580 `file:line` citations, roughly 760 words of "Sources" lists and about 1,000 words of "Where this is stated" lists. They are the guide's evidence; they are noise on a first read.

## Length and density

The guide is 43,360 words; the shortest path to a booted `hello` that says it started is about 5,500 words (README 1,300, 00-setup 1,662, "Components and manifests" 1,227, Levels 0 and 1 about 1,300), and to one that answers a client about 12,600 (add all of chapter 1, 5,100, and the rest of chapter 2). So a first server needs about 29% of the guide; the rest is reference. The README does not make that short path obvious (H2).

Read for a first server: README (Status, Before you start, Build and boot, Chapters), 00-setup, chapter 1 (not "Words used elsewhere"), all of chapter 2, and then chapter 5's table when a boot fails. Skip on a first pass: chapter 4, chapter 3 beyond the Level 0 pointers, the host half of chapter 5, chapter 6 beyond the ten rules and "What a first version needs".

| Chapter | Words | Proposal | Saves |
|---|---|---|---|
| 04 | 7,812 | Keep "Choosing" cut to 300, "Storage you lend" (500), one 800-word account of how `/boot` and an outgoing directory are served, "Testing without a kernel" (230), "What a tree server does not do" (140). Move "Building a tree", "LoopServer", "Answering an Open", "The other methods", "Service nodes", "Connections and their ids", "Rights only shrink" and the worked example (about 5,000 words) to `docs/architecture/` as the `ioservice` reference. The `Outgoing` section (313) repeats chapter 1 | about 5,300 |
| 05 | 6,996 | Move "What a fake kernel is here", "How the ioservice and loop tests use one", "A negative case on the host" and "The kernel shell" (1,195) out (M5). Drop "Where each cause is produced" (about 430) and fold "Other lines the root prints" (238) into the table. "Boot the dev image" and "Read the boot log" (994) repeat the README and chapter 2: keep one | about 2,300 |
| 06 | 6,735 | Fold R1 to R7 (470) into 120 words, since the chapter says Review "adds no rules"; fold the "first version" table (330) into the ten rules; move W28 and W29 to chapter 4 | about 1,100 |
| 01 | 5,971 | Drop the `Loop::arm` and `run` snippets (the reader never writes them), the `connect_or_fail` and `epitaph()` snippets, the second copy of `Handler` (chapter 2 and chapter 4 show it again), the three "Where this is stated" lists of the first three sections; turn "Words used elsewhere" into a glossary | about 1,250 |
| 03 | 7,492 | Condense steps 2 and 3 of "How to add" (copier route, 700 words) to 300, drop "Larger protocols to read" (143), cut "What is versioned" to 80; mark and move "A message on the wire" and "Replies and statuses" to the end | about 1,200 |
| README | 2,157 | Add the shortest path; cut "Build and boot" by a third once chapter 2 and chapter 5 stop repeating it | about 300 |

Total cut or moved: about 11,500 words (26%). Add about 1,500: Level 3 and 4 code and failure tables (about 400), Level 2 split, a client of your own (about 500), a glossary with the statuses (about 450, replacing "Words used elsewhere"). Result: about 33,000 words. The reading before Level 0 falls from 9,790 to about 4,500, and the whole path to a `hello` that answers from about 12,600 to about 11,000.

Sections I would rather see longer: the client side of the tutorial (M2, M6); a second method with a different shape (every example in chapter 3 is a string; none has a `u32` field, a one-way method or a reply with several fields); a build-error page (see below).

## The whole-guide test

**(a) Could I write a protocol server like `echo` and get it into the image?** Yes, if `prebuilt/` existed: copy `echo`, rename it, add a child entry and an offer, boot, and read the log. What I would still not know: how to get `prebuilt/` (H1); how to design a method whose fields are not a single string; how to keep state per server or per client beyond echo's `Storage`; how to write a client of my own (M6); how to add a server to an image other than the dev image, or to the dev image without editing `routing_checks` and `boot_check.py`; how a server uses another protocol (a `use` in a server's manifest and a `connect` in its program; chapter 1 shows the pieces, chapter 2 has no example); how to see why a user program crashed (m10); what to do about a GN or compile error that is not a routing failure.

**(b) A tree server.** The README says early and more than once that another component cannot reach one (`README.md:14,29,50`; `04-tree-servers.md:5`). That is clear enough. After chapter 4 I could build a tree and drive a `Server` in a host test, and I could not run it. The chapter is worth reading only as an account of how `/boot` and every outgoing directory work; at 7,812 words, 18% of the guide, it is the wrong size for that. About 2,500 words in the guide and the rest as library reference.

**(c) What would I do first?** `ls prebuilt/`. The guide does say so (`README.md:134`, a single line near the bottom), but the README's own order puts "What you will be able to do", "Who this is for", "What a server is" and "The running example" first, and "Status of this guide" (the two limits that decide whether you can proceed at all) is the fifth section. Move the limits up (H1, H2).

**(d) Missing.**

- A glossary in the guide, with the status names (see the terms table). The framework glossary is linked, but it is a design-level document.
- A page for build-time failures: a GN visibility error, an `undefined symbol: main`, a `constinit` refusal, an unresolved dependency. Chapter 5's table covers only what a client sees at run time; Level 2's table covers five build and run symptoms.
- A worked failure of the build (chapter 5 has two worked routing failures, `:453-457`, and they are good).
- An index of the form "I want to add a method / a second protocol / state / a client, so I read...".
- A statement of what to do when the machine cannot be set up (H1).

## Teaching opportunities

### T1. Make the other bugs fail on purpose, as Level 2 and Level 5 already do

`02-first-server.md:294` and `:504-510` teach by breaking a check and watching the boot fail. Apply it to the two bugs that chapter 1 and chapter 6 call silent: at Level 3, "move the read below the `Rearm` and watch `routing_checks` hang" (W6); at Level 4, "return `Rearm` instead of `release` in the unknown-method branch and watch the entry leak" (W9). Each is two lines of text and a result the reader can see.

### T2. A server that is also a client

Every example here provides a protocol and uses none. A second tutorial level in which `hello` calls another protocol would exercise `use`, `/svc`, the epitaph test and the Loop together, and would answer "how does a server reach another server", which the README promises in its first sentence.

### T3. A method with another shape

Chapter 3 teaches the wire layout with a string. One real method with a `u32` and a result code (a counter) would show offsets, `wire::store` and `wire::load`, and a one-way method, without leaving `examples/hello`.

## Suggested follow-up order

1. Make `prebuilt/` reproducible or say plainly that it is not (H1).
2. Add the shortest path to the top of the README and move the "read now" links of chapter 1 into chapter 2 (H2, m6).
3. Fix the tutorial's gaps, in this order: the missing include and the wrong early table row (M1), the "renamed" definition (M3), the code in prose and the false sentence at `:45` (M2), the dangling colons (m1), the row/cause wording (m5).
4. Add finished files per level and "If it fails" tables for Levels 3 and 4 (H3); name the commit the line numbers are for.
5. Split Level 2 (M4); add Level 6, a client of your own (M6).
6. Settle chapter 3 against chapter 2 (M7) and chapter 5's host half (M5).
7. Cut and move as in the length table; add the glossary and the build-error page.
8. The minor items, naming and wording first (m3, m4, m7 to m9, m13 to m15).
