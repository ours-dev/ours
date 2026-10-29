# server-guide-review: review of docs/guide/ as an outside reader

**Date**: 2026-10-29

## Scope and method

Reviewed: `docs/guide/README.md`, `01-concepts.md`, `03-protocols.md`, `04-tree-servers.md`, `05-testing-and-debugging.md`, `06-checklist.md` (about 31,000 words; chapter 2 does not exist yet). Read cold, in the order the README gives: README, chapter 1, then chapters 3, 4, 5 and 6 (the README's "go on to chapter 2" has nowhere to go, see H2 and M12).

The only other file read first was the contract, `.agents/tasks/server-guide/PLAN.md`, "### 1. The contract", plus the REVIEW skeleton in `.agents/tasks/TEMPLATE.md`. I judged the guide against the contract's promises (audience, words, the 15-line snippet rule, "no callback signature", no other operating systems).

What I opened outside the guide, and only to judge a snippet or a claim, never to resolve my own confusion: `examples/echo/BUILD.gn`, `examples/echo/echo_server.cpp` (lines 48-70 and 120-215 read, the rest by `grep`), `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:26-37` and `system/lib/ioservice/include/ioservice/outgoing.hpp:36-100`. Every snippet I compared matched its file. The `grep` counts below (for `user_executable`, `component_manifest(`, `startup`, `processargs`, `.agents`) were run over `docs/guide/` and are exact.

Verified rather than judged: all relative links resolve and every anchor in the contract's list exists; `python3 scripts/lint_docs.py --changed docs/guide/*.md` reports no violations; the byte dump of an `EchoString` request (03:207-216) is self-consistent (20 bytes plus the string) and so are the two reply dumps (29 and 24 bytes).

A note on tone: the guide is accurate and honest about its limits, which is rare. The findings are about whether an outsider can act on it, not about whether it is true.

## What is done well

- `01-concepts.md:27-34`: the two-ended channel picture and the paragraph on handles travelling in messages. It answers "how does a client hand a server its reply end" before the reader asks.
- `01-concepts.md:77-88`: ports in one sentence ("wake me when any of these has something"), a one-line picture, then "You will not call these two yourself; the Loop does". This is the best bridge from kernel mechanism to library in the guide.
- `01-concepts.md:81-84` (one-shot, level-triggered) and `:114-119` (the four consequences). Each consequence reappears as a checklist rule, so the reader meets the idea once before the rule.
- `01-concepts.md:255-264` (the request's path through client, root and server) and `:288` ("Two things are called svc"). Both prevent a real confusion.
- `01-concepts.md:331-334`: "Two things an epitaph is not". Short, and it separates an epitaph from a failed reply.
- `03-protocols.md:155-164`: the ordinal computed from the selector, with the digest bytes shown, so a reader can check the arithmetic by hand. `:171-173` (the designated-initializer shape the test needs, and the `.ordinal = 0x0` workflow) are the kind of gotcha that normally costs an hour.
- `03-protocols.md:191-216`, `:288-307`, `:323-332`: byte dumps with an offset, bytes and meaning column. `:407-414`: "Which answer a server gives" is a four-row table that says everything about closed protocols.
- `03-protocols.md:485-497`: "Not versioned: a protocol ... The tree has no written policy for changing a protocol." Saying what is not promised is worth more than a vague promise.
- `03-protocols.md:507-652`: the eight-step recipe, especially the "target named after the directory" trap (`:551`) and step 7, "Without that line nothing builds your test, and nothing runs it" (`:644`).
- `04-tree-servers.md:23-30`: the choice table and the paragraph after it. `:49-54`: the one-line picture of who calls whom. `:138`: "The order of the checks is part of the protocol". `:211-216`: the generation, walked through with entry 3, one event per line. `:303-314`: an Open traced through the two tree servers it crosses. `:337-341`: three plain reasons for `constinit`.
- `04-tree-servers.md:460-466`: "What a tree server does not do" is honest and short.
- `05-testing-and-debugging.md:28-41`: "Two places to test" as a table. `:65-93`: "What a fake kernel is here", including "Do not confuse this with `//kernel/tests/fake-kernel`". `:112-150`: a negative case read line by line, including why the second assertion pins the order. `:304`: "A negative case you have never seen fail proves nothing". `:357-364`: "Four questions, in order". `:425`: "Read it from the top" on a real log.
- `06-checklist.md:17-28` ("the ten rules") and `:30-40` (Rule, Why, Where, and the Enforced or Convention split, which tells a reviewer where to look hardest). The `R` items turn rules into walks over a diff.
- `README.md:54-62` (what the running example is and what a passing boot proves) and `:90-93` (the guide says what it cannot promise). Every chapter has its "What you will be able to do" and "Where to look next", as the contract asks.

## High severity

### H1. A newcomer cannot get a first build, and learns it only after the command
`README.md:66-93`, `05-testing-and-debugging.md:203`, `03-protocols.md:519`. The README's first action is `python3 .agents/skills/ours-verify/scripts/boot_check.py` (`:69`). Some twenty lines later (`:92`) it says the script runs "the tools in the checkout's `prebuilt/` directory: clang, gn, ninja, QEMU and GRUB", that it is documented "on the project's Mac host ... and nothing else", and that "the tree does not yet say how to fill it, so this guide cannot either". An outsider who runs the command first gets a failure with no explanation. Chapter 5 then says "after the setup in [README.md]" (`:203`) although the README has no setup. Chapter 3 tells the reader to `pip install` (`:519`) before the build can have run. Nothing says whether Linux works.

Fix: move `:90-93` above the command under a heading "Before you start", state it as a prerequisite ("you need X, Y and Z in `prebuilt/`; this guide does not yet say how to get them"), say plainly which host operating systems are supported (one word is enough; this is a prerequisite, so it outweighs the contract's rule against naming other systems), and add a sample of the expected output (the `PASS` lines) so a reader can tell success from failure. Change `05:203` to point at "Before you start". If there is a way to fill `prebuilt/`, it belongs in the guide, because every other step depends on it.

### H2. Nothing takes the reader from a written `main()` to "the root starts my server and a client reaches it"
`README.md:11` promises "declare it so that the root starts it and routes clients to it", and `README.md:93` defers it to chapter 2. Meanwhile the pieces are one-liners or fragments:

- `06-checklist.md:405-415` (W25, W26) are single-sentence rules: "Add the component as a child of the image's root manifest with `startup = "eager"` written out" and "Build the program with `user_executable()`". Neither has a snippet. `grep` finds `user_executable` only at `06:413` and `component_manifest(` only as prose at `01:162`; `startup = ` only at `06:407,409`; no root `children` entry anywhere.
- The manifest snippets at `01-concepts.md:173-210`, `03-protocols.md:114-141` and `06-checklist.md:343-391` start inside a structure. The first begins at `program = ":echo_server"`; the offer (`01:194-201`) and the use (`01:205-210`) are bare `{ protocol = ... }` blocks. The reader cannot tell which key (`offer`? `use`? `uses`?) holds them, what encloses them, or that the target is `component_manifest("echo.cm")` with `visibility` (`examples/echo/BUILD.gn:33`).
- `05:243` says to add a required line to `REQUIRED` in `boot_check.py`, a step that makes sense only after a root-manifest edit and a client the guide has not described (see M7).

Fix, now: add to the README a short "Until chapter 2 exists" recipe: the list of every place to touch, each with a snippet of at most 15 lines: the program target, the manifest target (whole, with its header line), the library dependency (`03:652`), the child entry and the offer in the root manifest, the client's `use`, the required line. If that is too much for the README, show echo's whole 48-line `BUILD.gn` in chapter 1 as two snippets and the child/offer in chapter 1's manifest section. Do not leave it to a one-line checklist rule.

### H3. The two interfaces a protocol-server author must implement are never shown
`04-tree-servers.md:392` says of `Handler`: "Its exact form is in `outgoing.hpp:46`." The contract says "quote the declaration from the header"; this points instead. `Handler::connect(Uandle)` is three lines. The Loop side is the same: `01:104`, `03:403`, `04:318` and `04:373` each say "the exact form is in loop.hpp" or "this chapter does not describe it". The contract deliberately keeps the callback signature out, so the guide never shows how a client's channel is registered with the Loop. Yet `06-checklist.md:132-185` gives six rules (W4 to W9) about a function the reader has never seen, and `01:271-272` shows `EchoServer server(*ECHO_STORAGE.init(), loop);` without ever saying that `EchoServer` is the reader's own class, derived from `Handler`, with a `connect`.

Fix: (1) quote `Handler`'s declaration (class head, `connect`, the protected destructor) at `01:266` or `04:392`; (2) say in one place, in the README's status note and at `01:104`, what is missing and why: "the way a callback is registered is being replaced; chapter 2 will show it; until then copy `EchoServer::connect`, `examples/echo/echo_server.cpp:126-142`, and read `loop.hpp`". As written, the reader who follows the rules cannot see what they apply to, and the silent gap reads as an omission rather than a decision.

### H4. A tree server of your own cannot be reached by anyone, and the guide says so on line 34 of the chapter after promising the opposite
`04-tree-servers.md:34`: "A manifest cannot route a directory that a component provides ... So today a tree server of your own has no manifest route by which another component finds it ... no program in the repository does it yet." Before that, `:3` says "You write ... the tree, the storage it lives in", `:5-15` lists "Build a tree ... Put a tree on a Loop with `LoopServer`" as things you will be able to do, and the README presents the tree server as one of two kinds (`README.md:25-28`) and says "To serve files or a directory tree, read chapter 4 as well" (`:46`). A reader who chose a tree server on the strength of those lines does the whole chapter and finds a dead end.

Fix: put the limit in the chapter's second paragraph and in the README's chapter blurb (`README.md:37`); rewrite the goal list at `04:5-15` to say what the chapter really delivers ("understand how the root's `/boot` and every outgoing directory work, and use the library in a host test"); mark the worked example as reading, not copying; in `README.md:19` and `:28` say that a *reachable* tree server is not yet possible.

## Medium severity

### M1. `constinit`, `LazyInit` and "no heap" are taught only in the tree-server chapter, yet every server needs them
`01-concepts.md:102` says "A program here has no heap", without a consequence (how does a server hold a string or a table?). `01:130-146` and `:271-272` show `SLOT_STORAGE` and `OUTGOING_STORAGE.init()` without a declaration; the declaration and the reason are at `04:343-365`, under "Storage you lend, and why it is `constinit`". The README sends protocol-server writers away from chapter 4 (`README.md:46`). Chapter 6 W2 (`06:112-124`) gives the rule and the three-line declaration but neither explains `LazyInit`/`init()` nor links to chapter 4. A reader who writes a plain global gets a link error or, worse, a zeroed object with no constructor run (`04:339`).

Fix: add a short subsection to chapter 1 after the Loop's table (after `:134`): "Where a server's memory comes from": no heap, no constructors for globals, therefore `constinit` and `ustl::LazyInit`, with echo's three declaration lines. Let chapter 4 keep the sizing and cite it. Have W2 link to it.

### M2. "Root" means four things, and chapter 1 contradicts itself
- `01-concepts.md:161`: "It can also have a manifest and no program, as the root does."
- `01-concepts.md:163`: "The *root* is the program that starts the components ... On the dev image it is `init`."
- `05:23`: "the root is `init`, the first user program"; the moniker is `.` "for the root itself".
- `04:78`: "The root's tree holds `/boot` as one root and a `/svc` directory for each component as the others": the program, then a tree's top directory, in one sentence. `04:83-95` has a test directory called `root/`, and `04:413` says "one root for `/boot`".

Fix: in `01:161-163` name two things once: "the root component (its manifest is `meta/root.cm`, it has no program of its own) and `init`, the program that runs it". In chapter 4 call the nodes `add_root()` makes "top directories" (code font for the call) and rename the test tree's `root/`.

### M3. "Rights" is three vocabularies, and chapter 1 oversells AccessDenied
`01:23` (handle rights), `04:114` ("the rights the library duplicates: Basic, Map, GetProperty and Read ... Duplicate"), `04:135` ("A *right* is one thing a connection may do. There are nine"), `04:255-267` (manifest strings `rx*`, `r*`) are three different sets under one word. `04:263` ("The VMO it hands out can do no more than the connection can") moves between two of them without saying so. Also `04:152` uses `rx*` before `04:255`/`:267` explain it, and no snippet shows how a rights string is written in a manifest although `04:255` describes three routes that depend on it.

Separately, `01:218` says a failed walk answers "`AccessDenied` when rights are narrowed", which a reader of a protocol server takes to apply to protocols. `05:377` (row 6) says "A protocol use states no rights, so routing never answers a protocol this way".

Fix: begin `04:135` with "These are ours.io rights, a different set from the handle rights of chapter 1"; define `rx*`/`r*` at first use with one manifest line; in `01:218` write "(for a directory use only)".

### M4. Other words with two meanings
- *dev image*: `README.md:79` "`kernel.dev.obi`, the image this guide means by the dev image"; `05:22` "It comes in two builds, called `rel` ... and `dbg` ...; only `rel` has the root"; `05:209` "boots both dev images". The `05:22` definition also contradicts itself ("holds the root" and "only `rel` has the root"). Pick one: "the dev image is `rel`; `dbg` is the debug image".
- *tree*: `03:27` "In this tree a protocol is a library", `03:489` "The tree has no written policy", `03:497`, `04:41` "a set of node trees", `03:143` "a manifest's whole tree" (the component tree). The repository, the component tree, a directory tree and the `Tree` class share one word next to the flagship term "tree server". Say "repository" for the checkout.
- *connection*: `04:42` (a client's open handle on one node, with rights and a cursor) versus `03:371-377`, `05:361`, `06:72` (a channel to a server).
- *method number* (`01:294`) versus *ordinal* (`03:149`), never equated; *selector* is a third name for what `03` calls the method's full name.
- *client entry* (`03:336`) versus *slot* (`01:102`) versus the connection table's *entry* (`04:207`).
- *server* versus "the root's built-in servers" (`05:384`, row 8 at `05:379`): the Launcher and resource servers are not components, so they are outside the guide's own definition (`README.md:23`); and "built-in" is not defined (see M5).
- The README says "There are two kinds" (`:25`); chapter 4 says "or both" (`:7`) and "The two are not exclusive" (`:32`), because echo's outgoing directory is itself a tree built by `ioservice`.

### M5. Terms used before they are defined, or never
- *epitaph*: `01:119` (Loop section) and `01:3`, defined at `01:292-294`. Add "(the last section defines it)" at `:119`.
- *VMO*: first at `04:27` (the table), defined at `04:30`; `03:483` says "memory object" and `vmo@20`; chapter 1 never defines it (`01:19` says "a block of memory").
- *built-in(s)*: `01:164` ("the root's built-in `boot`"), `05:374` and `05:378` in log lines ("the built-ins"). No section says that the root itself provides some protocols and `/boot`.
- `ort`, `ustl`: `01:286`, `01:306`, `04:340`, `05:232` use `ort::` and `ustl::` types as if known; neither is said to be a library (the runtime a program links; the tree's own standard-library pieces).
- *vDSO*: used at `04:458`, defined at `05:19`.
- *processargs*: defined at `01:220` and used nowhere else (`grep`). Drop it.
- *Outcome*'s six actions: `04:44` says six; `Bind`, `Refuse` and `Drop` are used at `:169-190`; the six are first listed at `:379`.
- `Realm`, "instance", "component pool": `04:295-303`. The `ProtocolService` flag: `04:306`. `Syscalls`: `04:371`.
- `PeerClosed` is a status (`01:48`) and a signal (`01:49`) without a remark that both exist. `INSTANT_INFINITE` is used at `01:64`, explained at `:86`.
- *obi*, QEMU, GRUB: `README.md:75,79,92`; QEMU is explained only at `05:17`.

### M6. Snippets that do not show what the text says they show
- `03:381-403`: the text says the code "falls to the lines that close the message's handles and release the client" and that "the callback answers `Close` here". The snippet's last line is `return release(client);`; `release` is not shown in chapter 3 (it appears at `06:176-183`), so `Close` is invisible. Show `release` in the same snippet or say "`release` returns `Close`".
- `03:98`: "writes every message's layout as a table ... Do the same for your protocol", but the table is never shown. It is 12 lines (`ours_examples_echo.hpp:26-37`) and would fit as a snippet. The notations `size@16` (`03:601`) and `vmo@20` (`03:483`) are used unexplained.
- `04:322-332`: the `LOOP_SLOTS` snippet from the root uses `MAX_INSTANCES`, `RESOURCE_SERVERS` and so on that mean nothing to the reader; echo's own sizing (`01:121-134`) already shows the idea.
- `06:220-226` (W13): the "snippet" is a comment, not the behaviour; `03:383-401` is the real one.
- `01:88`: the text says `run` "calls `port_wait` once for every packet", the snippet shows `arm`.
- `05:297-302`, `05:414-438`, `06:473-483`: the log lines are quoted from change records and a design document, not from a boot. The reader cannot tell they will see them (the two `routing_checks` lines at `05:300-301` are produced by every boot and could be quoted as "in your `rel.log` you will see").

### M7. There is no minimal client, though the guide tells the reader to write one
`05:243`: "give a component the job of connecting to it and checking what it answers, the way `routing_checks` does". The pieces of a client are spread over three chapters: `connect_or_fail` (`01:235-249`), `receive` (`05:333-347`), `epitaph()` (`01:318-328`), prose at `03:342`. No snippet shows the whole exchange for echo: encode the request, write, wait, read, test for an epitaph, decode the reply. `routing_checks` is 900-plus lines (`05:291` cites `:906-913`), so the reader cannot lift a client from it. Without a client the reader cannot see their server answer, only see it start.

Fix: one snippet of at most 15 lines (from `routing_checks.cpp:793-826` or similar) of "send `EchoString`, read the reply", in chapter 5 beside `receive`, plus the manifest lines the client needs (`use`, and the root's `offer`).

### M8. The protocol recipe (chapter 3, steps 3 to 5) is described, not shown
- `03:549-554`: "Make four changes" to a generated `BUILD.gn` that the reader has not seen (it is not shown), then four bullets, the last of which holds two changes (`visibility` and the `tests` group). The reader cannot diff against an unseen file. Show the template's generated file before the four changes, or state the changes without reference to it and show the finished echo file only.
- `03:575`: `cpp_interface("headers")` is not explained.
- `03:597-613`: steps 4 and 5 describe the header and source of `ours.examples.hello` in bullets, with no `hello` code; the `Greet` reply type and whether a method with no arguments needs a struct are left open.
- `03:379`: "The reason is in the header ... would need a bit in the header to mark such a call". A step is missing: why would an answer need a mark? Presumably a client could not tell "unknown method" from a normal reply to a method it did call; the text should say so.
- `03:168`: "Two methods cannot be given one number by hand, and a method's number follows from its name" does not parse; it probably means a collision is unlikely and a rename changes the number.

### M9. Insider language an outsider cannot follow
- `.agents/`: 22 lines (`README.md:69,88,101`; `03:649,673`; `05:22,52,54,206,209,220,226,308,448,449,457,463`; `06:453,457,459,465,562`). The command path must stay. But "skill" is a tool word (`README.md:66`: "the `ours-verify` skill's script"), and the reader is sent to `SKILL.md` for the table of failures (`README.md:88`, `05:220`), the hang report (`05:448`) and the debugger. Say "the verify script", name its documentation once, and bring the rows that concern a server into chapter 5.
- Change records as evidence: `05:295-304`, `:355`, `:384` (row 13), `:414-438`; `06:477`; `03:497`. Rewrite as "run this and you will see ...". Delete "The change record of the type check broke a route on purpose" (`05:414`) and "once read ... now reads" (`05:355`).
- `docs/repairs/...`: `04:116`, `:465`, `:491`; `05:375`, `:470`. Say "a known limit" and drop the pointer.
- Design-document numbers: `04:431` ("invariant I2"), `04:491` ("decisions D10, D21 and D27", "section 4.2"), `06:56`, `:136`, `:154` ("invariants I1 and I2", "I4"). The rule is already stated in words in each item; drop the labels.
- `CLAUDE.md`: `03:595`. A repository convention; rename it ("the repository's formatting rule") and say where it lives.
- In-flux and history words: `04:318` ("they are being reworked as the Loop's callback changes"), `03:405` ("before the protocol type existed"), `03:27` ("written by hand today"), `06:464` and `:465` ("before `routing_checks` reached echo", "the history is in `echo_server.cpp:7-12`"). Delete them, and put one honest "Status of this guide" note in the README instead (chapter 2 is missing; the callback's registration form is being replaced).
- Unexplained local names: `ours.autorun.boot` (`05:41`), and the hardware component `pc-ps2` in the required lines (`05:216`), in two worked examples (`05:416-425`) and in `06:396-399,480`. Use `routing_checks` and echo, which the reader knows.
- Different "Checked against commit" hashes in each chapter (`94a62227` in the README and chapter 1, `c2b7a75a` in chapter 3, `693260d0` in chapters 4 to 6). A reader cannot tell which line numbers hold at their checkout; make them one hash at the consistency pass.

### M10. Density and length
The guide is about 31,000 words; chapters 3 to 6 are 6,000 to 7,000 each. The recurring causes:

- Citation load: about 145 `file:line` references per chapter in 3, 4, 5 and 6 (33 in chapter 1), nearly one per sentence (for example `03:110`, `03:143`, `04:115`, `05:372-384`). They break every sentence for a reader who is learning. Keep citations beside snippets and for non-obvious claims; collect the rest in a "Where this is stated" list at the end of a section.
- `01:159-220`, "Components and manifests": thirteen terms (component, manifest, boot image, root, capability, protocol, protocol type, use, offer, expose, route, processargs, startup handles) and three partial snippets in 60 lines. Split into "who may reach whom" (component, manifest, root, use, offer, expose, with a small parent/child picture, arrows up for expose and down for offer) and "routes", and drop `processargs`.
- `03:110` (three names, a rename and four citations in one paragraph): a table of the names already sits above it; cut the paragraph to two sentences. `03:171-179`: ordinal declaration shape, the Python test, the `0x0` workflow and the by-hand run in four paragraphs; make it a numbered "Adding a method" list.
- `04:134-135`: nine rights, `PERM_` bits and "five matter" in one bullet; a five-row table. `04:163-167`: step 9 is three node kinds by many flag cases by six status codes; a table (kind, request, answer). `04:255`: three manifest routes with four citations in one bullet; a three-row table. `04:220-233`: the generation wrap-around detail is more than a first reader needs; move it to a side note. `04:421`: step 2 of the worked example is a paragraph of mechanism.
- `05:43-63`: how `host_tests.py` finds tests (three `gn` queries) comes before the sentence the reader needs, "build the library's `tests` group and run its binary" (`:56-63`). Lead with the recipe; move the discovery rules to the end of the section.
- `05:366-384`, the table: 13 rows by 5 columns, cells of up to 80 words with code references inside them, unreadable in raw Markdown and heavy on screen. Split into (1) a lookup table "what the client sees, which rows to try" with three short columns, (2) one short list item per cause with its fix, (3) the "Produced by" column as a sources list.
- Repetition: the three manifest snippets appear in `01:173-210`, `03:114-141` and `06:343-391`; `write()` at `01:38` and `03:263`; `close_with_epitaph` at `01:304` and `06:252`; the epitaph is explained at `01:292`, `03:319`, `05:322` and `06:13`; `boot_check.py` is described at `README.md:64-88`, `03:648-650`, `05:201-221` and `06:455-459`; wiring a test into `//:default` at `03:632-644` and `05:176-195`; `05:13-26` and `06:9-15` re-define chapter 1's words. Give each its one home and link to it.
- Chapter 6 is 564 lines (41 items with a snippet in a dozen). "The ten rules" (`06:17-28`) is the right size for a front page; present the rest explicitly as reference.

### M11. The failure table says what happened, not what to do
`05:370-384`. Rows 2 to 5 are identical for the client ("an epitaph `NotFound`"), and the reader gets to the right row only through the four questions (`05:357-364`) and the root's log. Only row 2 ("which is where to add it") and the two worked cases (`05:425`, `:444`) say what to change. Add a short "Fix" column, or a line per row, and put the discriminating log line in the first column the reader looks at.

### M12. Order, inside chapters and across them
- README: "Build and boot" (`:64-93`) comes after the chapter list and before "Start with 01-concepts.md" (`:97`), so the first thing asked of a newcomer is a command whose result they cannot yet read and whose prerequisite is disclosed afterwards (H1). Order: who it is for, what a server is, before you start, build and boot (with expected output), chapters, how to read.
- README `:45`: "To write your first server, go on to chapter 2 when it exists, and read chapter 3 beside it" sends the reader to an absent chapter, then to chapter 3, which says "The program that answers them is chapter 2" (`03:14`). Give the interim path (see T4).
- Chapter 3: 120 lines of byte layout (`:187-275`) precede the recipe (`:507`). A reader who asks "how do I define a protocol" meets the full wire format first. Put a 10-line "In short" at the top, mark `:187-275` and `:467-497` as reference, and consider moving the recipe up, right after "Names and the protocol type".
- Chapter 4: "Answering an Open" (`:130-190`), "Connections and their ids" (`:192-249`) and "Rights only shrink" (`:251-267`) come before "Storage you lend" (`:316`) and "Putting a tree on a Loop" (`:367`), the assembly steps. Order for a builder: Building a tree, Storage, LoopServer and Outgoing, then how it answers, then rights. The worked example (`:409`) is correctly last.
- Chapter 5 opens with a "Words used in this chapter" list (`:13-26`) that re-defines root, route and epitaph; keep only the new words (host, host test, fake kernel, negative case).
- Constinit and storage (M1) should come before the Loop's first use of it (`01:121`).

## Minor and style

Naming and wording first.

- `README.md:66`, `05:22`: "skill" for the verify script is agent-tool vocabulary; say "script".
- `04:38`, `:132`, `:205`: "Objects first." is the author's recipe leaking into the text. Use headings ("What is involved", "What happens").
- `04:337`: "storage here is `constinit` for a reason that also explains why it sits in a `ustl::LazyInit`" does not parse. Say "for two reasons".
- `01:50`: "A process may have created at most 1024 live handles, wherever they are now" is clumsy; say "may hold at most 1024 handles at once".
- `01:121-122` and `06:284`: "One is all dev's routing checks use" (what is "dev"?); say "the dev image's".
- `01:220` writes `init: <name> exited with <code>`, `05:23` and `:239` write `<moniker>`; use one.
- `01:151` mentions `Stop` before the reader has met it (only the README names it); a half-sentence would do.
- `03:336` "free client entry" versus the Loop's "slot" (see M4).
- `03:259-269` repeats `01:36-44` verbatim; keep one.
- `03:465` lists `ours.io`, `ours.process` and `ours.resource` as examples with no one-line description of each at first mention (`Launch`, `GetBackingMemory`).
- `04:17`: a paragraph of citation rules (a bare name is a file of `system/lib/ioservice`, with three exceptions) and a style no other chapter uses (`03` and `01` use full paths; `05:368` defines short names for its table only). Use full paths everywhere, or one rule for all.
- `05:155-193`: the numbered list interleaves code fences and "path:lines:" labels; some renderers restart the numbering. Use sub-headings or write the four steps without numbers.
- `05:216`: the required lines `pc-ps2: ready`, `pc-ps2: key a` and `pc-ps2: key A` need one clause saying they are other components' lines.
- `05:63`: `vendor/googletest/docs/advanced.md:1835` is a citation by line number of a vendored document; name the flag only.
- `06:38` says every Where line starts with Enforced or Convention; `06:365` (W23) starts "The character set and the length are Enforced ...".
- `06:128` W3: "Give a base class no virtual destructor" is a surprising rule (every C++ reader expects the opposite); the Why is good, but it deserves one half-sentence at the head: "unusual: because nothing is ever deleted".

## Teaching opportunities

The test of the whole guide (item 8 of the brief), answered as a reader with only these pages.

### T1. Could I write a protocol server like `echo`? The library, yes. The program, no, and I could not run it.
What I could do: define the protocol and its library (`03` is the strongest chapter: names, ordinal, layouts, codec, test, `BUILD.gn`, root `BUILD.gn` wiring, the `ordinals_test.py` step), understand every rule my server must keep (`01`, `06`), and test the codec on the host (`05`).

What I would still not know or would have to guess:

1. How to get any build at all (H1).
2. How to write the program's `BUILD.gn`: `user_executable("...")`, the `component_manifest("....cm")` header, `visibility`, and in which directory it must sit for the image to see it (H2).
3. How to add my component to the root manifest (the child entry syntax, `startup = "eager"`, the name form), and how to offer my protocol to a consumer (H2).
4. The `Handler` base class I must derive from, and its protected non-virtual destructor idiom (`06:128`); I only learn the shape by reading `echo_server.cpp` (H3).
5. How to register a client's channel with the Loop and what the callback looks like (H3). I know the rules (`06` W4 to W9) but not the function they bind.
6. The `#include`s, namespaces, the `main` signature and the order of the start-up calls in one piece. The fragments are `01:138-155`, `01:271-284`, `04:357-363`, `06:316-327`, but never assembled.
7. How to write the reply encoder for a request of my own. `03:220-236` shows a request encoder and `03:428-441` an `ours.resource` reply; echo's reply encoder is only named.
8. How to write the minimal client that proves my route (M7) and which line to require in `boot_check.py` (`05:243`).
9. What splits a server into a host-testable library and an image-only program. `05:41` gives one sentence and echo has no host test, so I would design that split alone.
10. Which of the 41 checklist items I need for a first version, versus later. The "ten rules" help; there is no "minimum for a first boot".

### T2. Could I write a tree server? I could read the root's `/boot`, not write one.
Beyond the dead end of H4 (`04:34`), I would not know: the fields of `LoopServer::Memory` and the constructor's arguments (`04:318` and `:371` defer to the header); how to obtain a VMO to put in a `File` node, and the `bytes` view over it (`04:114` says the VMO must exist, mapped, with rights; no creation call is named); the other `Delegate` hooks (`write_failed()` is mentioned once at `04:381`); the client side (`04:21` says "uses `fsio` to open, list and read" with no snippet); the dependencies a tree server's `BUILD.gn` needs beyond three target names (`04:56`). Most of this chapter explains why the root's tree behaves as it does, which is good for understanding and not enough for building.

### T3. What would I do first, and is it clear? Not clear.
The README's order is: skim "What you will be able to do", read the chapter list, run the build command (blocked, H1), read chapter 1 ("Start with [01-concepts.md]" is the last line before Sources), then "go on to chapter 2 when it exists". If the build ran, I would read chapter 1, then try to copy `examples/echo` to `examples/hello` and change names, because nothing else says what to do. I would reach for chapter 3's recipe for the library, which works. Then I would hit H2 and H3. The guide does not say "start by copying echo"; it should.

### T4. How much does the missing chapter 2 hurt? A great deal, and the README does not say so.
Chapter 2 is the bridge from concepts to code and the one place that would assemble `main`, the targets and the root-manifest edit. Chapters 1, 3, 4, 5 and 6 are reference and rationale; they cross-refer to chapter 2 six times (`README.md:35,45,93`; `01:342`; `03:14`; `04:474`), and each time the reader is promised something that is not there. The README says "in preparation" and nothing else. In the meantime the README should say:

- A status box: chapter 2 is missing; the way a callback is registered with the Loop is being replaced and will be shown there; a reachable tree server is not yet possible.
- An interim path: read chapter 1; read `examples/echo/echo_server.cpp` top to bottom with chapter 1's snippets as a guide; copy `examples/echo` to a directory of your own; follow chapter 3's recipe for the library; make the listed edits (the H2 recipe) to the root manifest; run the boot; read chapter 5 when it fails; read chapter 6 before review.
- What to skip on a first pass: chapter 4 for a protocol server (except the storage part, until M1 is fixed), `03:187-275` and `:467-497`, and the detail tables in chapter 5.

## Suggested follow-up order

1. README: add "Before you start" above the command, with the toolchain limit, the host, and the expected output (H1); add the status box and the interim path (H2, T4); fix the order (M12).
2. Show `Handler`'s declaration and the whole `BUILD.gn` and manifest of echo; add the root-manifest child entry and offer (H2, H3).
3. Move the tree-server limit to the top of chapter 4 and the README; retitle its goals (H4).
4. Word fixes: "root" (M2), "rights" and the `AccessDenied` remark (M3), "dev image" and the other double meanings (M4). These are small edits with a large payoff.
5. Define the missing terms in chapter 1 (VMO, built-in, `ort`, `ustl`) and add the forward note on the epitaph; drop `processargs` (M5). Add the storage subsection (M1).
6. Repair snippets: add `release`, the layout comment, a real boot's log lines and a minimal client (M6, M7, M8).
7. Scrub the insider language in one pass, with the keep, rewrite and drop decisions in M9; normalise the commit hash.
8. Density edits (M10, M11): a rights table, a step-9 table, split the failure table, trim citations; then reorder chapters 3, 4 and 5 (M12) and remove the repeated snippets.
9. Re-read chapter 1 and the README cold again, since they carry the newcomer's first hour.
