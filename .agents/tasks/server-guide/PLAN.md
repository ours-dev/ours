# server-guide: a developer's guide to writing a server for ours, drafted chapter by chapter

**Date**: 2026-10-29
**Status**: Proposed

## Request
The user asked on 2026-10-29 for a developer's manual that teaches someone who has never seen this kernel how to write a server, "otherwise who will use our kernel". It lives in the repository, is written for outside developers, and is as friendly to a beginner as it can be while staying exact. On 2026-10-29 the user took the recommendations below. The scope ends at servers: components that answer requests on channels, whether they provide a protocol or serve a directory tree. Writing a kernel object, a driver's hardware side, or a new system call is not covered.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Where and in what language | (a) `docs/guide/`, English; (b) `docs/development/`, which is for kernel developers | (a): the readers differ, and the repository writes every document in English | Not asked | (a), 2026-10-29 |
| Where the guide's code comes from | (a) quote the real `examples/echo` and other real files in short snippets with file and line, and no examples of its own; (b) a buildable example program per stage of the tutorial | (a): (b) is more reliable and a good deal more work, and can follow once the guide has readers | Not asked | (a), 2026-10-29 |
| When chapter 2 is written | (a) now, against the Loop's present callback form; (b) after step 4 of `.agents/tasks/loop-waiter/PLAN.md` has moved `EchoServer` to `loop::Waiter`, so the guide teaches the final form | (b): the other chapters do not depend on the callback's form | Not asked | (b), 2026-10-29 |
| Who drafts | (a) one agent per chapter, then a cold reader and a consistency pass; (b) one agent for all | (a): chapters are independent once the contract below fixes the words and the links | Not asked | (a), 2026-10-29 |

## Steps
Each step has a workspace `.agents/tasks/server-guide-<name>/` with a `RESULT.md` (skeleton in `.agents/tasks/TEMPLATE.md`). The agents write their chapter into the main checkout under `docs/guide/` and **do not commit**: the files are new and disjoint, and one orchestrator commit by explicit paths avoids the shared index. The orchestrator lints, reads, and commits.

### 1. The contract
Fixed here so that parallel chapters agree.

**Audience.** A developer who knows C++ and Git and has never seen this kernel. They want to write a server and run it in the dev image. The guide defines every term of the kernel at first use or links to where it is defined (`01-concepts.md`), shows before it tells, and gives each chapter a short "What you will be able to do" opening and a "Where to look next" closing.

**Files.** `docs/guide/README.md`, `01-concepts.md`, `02-first-server.md` (written later, step 4), `03-protocols.md`, `04-tree-servers.md`, `05-testing-and-debugging.md`, `06-checklist.md`. Links between chapters are repo-relative (`03-protocols.md#closed-protocols`); the README lists chapter 2 as "in preparation" without a link.

**Words.** One meaning each, used the same way in every chapter:

- *server*: a component with a program that answers requests on channels. The library class `ioservice::Server` is always written in code font, and the guide says "server" for the program and `Server` for the class.
- *handle*, *channel*, *port*, *signals*, *Loop*, *slot*: the kernel objects and the library that waits on them.
- *component*, *manifest*, *capability*, *protocol*, *protocol type*, *use*, *offer*, *expose*, *root*, *outgoing directory*, *`/svc`*, *epitaph*: as `docs/architecture/component-framework/glossary.md` defines them.
- *tree server*: a server built on `ioservice` that serves a directory tree (the root's `/boot` is one). *Protocol server*: one that answers a protocol of its own on each client's channel (`echo`).

**Anchors** of `01-concepts.md`, which other chapters may link to: `#handles-and-channels`, `#ports-and-signals`, `#the-loop-and-its-slots`, `#components-and-manifests`, `#svc-and-the-outgoing-directory`, `#epitaphs`. Its headings are exactly: "Handles and channels", "Ports and signals", "The Loop and its slots", "Components and manifests", "`/svc` and the outgoing directory", "Epitaphs".

**Rules for what is written.**

- Every statement of fact has a source in the code or in a design document, read by the author, not recalled. A chapter ends with a "Sources" list of the files it drew from, and a line "Checked against commit `<short hash>` on 2026-10-29." (take the hash from `git log -1`).
- A snippet is copied exactly from a real file, is at most 15 lines, and names its file and line (`examples/echo/echo_server.cpp:126`). No invented API, no pseudo-code that looks like code. Where the guide needs to say what a call looks like and no real snippet shows it, quote the declaration from the header.
- What cannot be verified is not asserted. It goes under "Not verified" in the author's `RESULT.md`, and the chapter says nothing about it.
- The Loop's callback form is being replaced (`.agents/tasks/loop-waiter/PLAN.md`). Describe a callback in words (the Loop calls it with the signals the wait observed, and it answers Rearm or Close) and link to `system/lib/loop/include/loop/loop.hpp` for the exact form. Show no `Callback` signature and no `on_signals(void *context, ...)` code.
- Do not name other operating systems. Code font for identifiers, files and commands; text diagrams in a fence with the language `text`.
- Document layout is CLAUDE.md's: `-` bullets, one H1 first, no skipped levels, a language on every fence, one paragraph or list item per line, a final newline. Run `python3 scripts/lint_docs.py --changed <file>` and check that every relative link resolves.
- Where the code or its comments are wrong, missing or contradict each other, say so in `RESULT.md`, and if it is a real defect, write it into `docs/repairs/261029/<module>.md` from `docs/repairs/TEMPLATE.md`. Do not paper over it in the guide.

### 2. Draft the chapters, in parallel
Five agents, one workspace each, disjoint files.

- **`guide-concepts`**: `docs/guide/README.md` and `01-concepts.md`. The README: who it is for, what is in each chapter, how to read it, the running example (`examples/echo`), how to build and boot the dev image (from `.agents/skills/ours-verify/SKILL.md`), and the chapter list with chapter 2 "in preparation". `01-concepts.md`: the six sections above, each one defining its terms in plain words, with one small text diagram where it helps, and the order a reader needs (a channel before a Loop, a Loop before a component). Sources to read: the component-framework glossary and libraries, `docs/design/261004/component-framework.md`, `system/lib/loop/include/loop/loop.hpp`, `system/lib/{wire,ours.io,fsio,ort}/README.md`, the kernel headers for handles, channels, ports and signals under `system/public/ours/` and `kernel/object/include/ours/object/`, `docs/architecture/object/handle-lookup.md`.
- **`guide-protocols`**: `03-protocols.md`. How to define a protocol: its name and protocol type, its methods and how the method number comes from the name, the message layout and codec, a reply and its status, closed protocols (an unknown method closes the channel without an epitaph; the anchor `#closed-protocols`), handles carried in messages, the largest message, what is and is not versioned. Then how to create the library: the files, the GN target, the test, and the copier template `tools/templates/cpp_library`. The running example is `examples/echo/protocol`; point at `system/lib/ours.process` and `system/lib/ours.resource` as larger ones. Sources: `system/lib/wire`, `examples/echo/protocol` (header, source, test, README), `system/lib/ours.process`, `system/lib/ours.resource`, `tools/templates/cpp_library`.
- **`guide-tree-servers`**: `04-tree-servers.md`. When to choose a tree server over a protocol server; the objects (`Tree` and its Directory, File and Service nodes, `Server`, `LoopServer`, `Outgoing`, `Handler`); a walk through an Open and the ordered checks it goes through; connections and why a connection id carries a generation; rights only shrink; what a Service node does with an Open; how a program lends the storage and why it is `constinit`; the root's `/boot` (`BootServer`) as the worked example. Sources: `system/lib/ioservice` (README, headers, tests), `system/service/init/boot_server.*`, `system/lib/ours.io`, `docs/design/260924/userspace-filesystem.md`, `docs/changes/261008/ioservice.md`, `docs/changes/261028/ioservice.md`.
- **`guide-testing`**: `05-testing-and-debugging.md`. How to test a server: logic apart from system calls and tested on the host with a fake kernel (`system/lib/ioservice/test`, `system/lib/loop/test`), googletest targets and how host tests are found and run (`scripts/host_tests.py`), booting the dev image (`ours-verify`, `boot_check.py`, `kshell_run.py`), negative cases the way `system/service/init/test/routing_checks` makes them, the logs a server and the root print and how to read a failing route (`init: ... routes nowhere`, `reaches another protocol`), and what a client sees for each failure (NotFound, AccessDenied, BadType, PeerClosed, NoResource) with the code that produces each; debugging links to `docs/development/debugging.md` (in Chinese: link, do not copy). Sources: the skills under `.agents/skills/`, `scripts/`, `system/service/init/realm.cpp`, `docs/changes/261029/init.md`, the tests named above.
- **`guide-checklist`**: `06-checklist.md`. A checklist for writing and reviewing a server, each item a rule, why it holds, and where it is stated or enforced: no heap and `constinit` storage; the callback's rules (one unit of work, never closes its own handle, Rearm only once the message is read); who closes which handle; limits and what is answered when they are hit; unknown methods and failed writes; leaving the program and what routes then answer; lifetimes of everything the Loop points at; manifest and routing (names, `type`, offers); tests and boot. Group by the phase a reader is in (design, write, test, review). Sources: `system/lib/loop/include/loop/loop.hpp` (the spec in its comments), `examples/echo/echo_server.cpp`, `system/lib/ioservice` headers, `docs/design/261004/component-framework.md` (the invariants I1 to I4), `docs/development/pitfalls/README.md`, `docs/design/261029/protocol-identity.md`.

Each agent reports (under 250 words): the files written, the number of facts checked, what it could not verify, and any defect found.

### 3. A cold read, then the consistency pass
One agent reads all five files as the audience, knowing nothing else, and lists every place where a term is used before it is defined, a step is skipped, a statement is not backed by a snippet or a source, or a chapter contradicts another. The orchestrator fixes what it finds, runs the linter on every file, checks every relative link, and commits the guide by explicit paths with the agents' workspaces.

### 4. Chapter 2, after step 4 of loop-waiter
`02-first-server.md`: the staged tutorial, written against `loop::Waiter`. Level 0 is choosing the kind; 1 an empty skeleton that starts; 2 reachable; 3 answers one method; 4 holds its limits and answers its errors; 5 tested and booted, each level with how it is verified. It is planned with the user when its turn comes.

## Files
- Created: `docs/guide/{README,01-concepts,03-protocols,04-tree-servers,05-testing-and-debugging,06-checklist}.md`, and later `02-first-server.md`.
- Created: `.agents/tasks/server-guide-<name>/RESULT.md` for each agent.
- Possibly: `docs/repairs/261029/<module>.md` for defects the authors find.

## Test plan
- `python3 scripts/lint_docs.py --changed` on every file of the guide reports no violations.
- Every relative link in the guide resolves to a file, and every `file:line` it cites exists and shows what the text says.
- Every code snippet is found in its file, verbatim.
- The cold read of step 3 finds no term used before its definition, or the findings are fixed.
