# server-guide-concepts: the guide's README and its concepts chapter, drafted

**Date**: 2026-10-29

## Delivered
Two new files, not committed (the orchestrator commits by explicit paths):

- `docs/guide/README.md`: who the guide is for, what a server is, the chapter list with chapter 2 "in preparation" and unlinked, how to read it, the running example, and how to build and boot the dev image.
- `docs/guide/01-concepts.md`: the six sections with the exact headings and anchors of the contract (`#handles-and-channels`, `#ports-and-signals`, `#the-loop-and-its-slots`, `#components-and-manifests`, `#svc-and-the-outgoing-directory`, `#epitaphs`), 13 code snippets copied from real files, 4 text diagrams, "What you will be able to do", "Where to look next", "Sources" and the "Checked against commit" line.
- `docs/repairs/261029/prebuilt.md` (one item) and `docs/repairs/261029/loop.md` (one item): defects found along the way, below.

Both files were written against HEAD as it moved during the work (`75099335`, then `23b6e9e7`, `78a4dbd0` and later); the "Checked against commit" line holds the hash read at the end.

## Verification
Mechanical checks, run at the end:

- `python3 scripts/lint_docs.py --changed docs/guide/README.md docs/guide/01-concepts.md docs/repairs/261029/loop.md docs/repairs/261029/prebuilt.md` reports no violations.
- A script (`check_guide.py` in this session's scratchpad) checks that each of the 13 fenced code snippets is copied exactly from the file and line named in its caption and is at most 15 lines, that every one of the 34 `file:line` citations in the prose names an existing line of an existing file, that every one of the 21 relative links resolves, and that every anchor linked in a sibling chapter exists (`03-protocols.md#closed-protocols` does). It also checks the exact H2 headings of `01-concepts.md` and that neither file names another operating system. At the last run it reported no problem; links to `04-tree-servers.md`, `05-testing-and-debugging.md` and `06-checklist.md` resolved or were waiting for their authors, as the file names are the contract's.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --help` runs and lists the options the README names.

### Facts checked
Each fact below was read in the source named, not recalled. 62 entries, several of which group related facts.

| # | Fact | Source |
|---|---|---|
| 1 | The command, run from the repo root, and its flags `--out`, `--no-build`, `--only dbg or rel`, `--timeout`, `--no-tests` | `.agents/skills/ours-verify/SKILL.md:17`, `:49-52`; `.agents/skills/ours-verify/scripts/boot_check.py:547-552` |
| 2 | It builds `out/bringup.x86` with `prebuilt/ninja`, boots the two images side by side, about 25 seconds end to end | `SKILL.md:20-23` |
| 3 | A full build takes about 6 seconds from a warm cache and about 40 cold | `SKILL.md:61-62` |
| 4 | One verdict line per image starting with the tag and `PASS` or `FAIL`; a `host-tests` line; a last `logs` line; exit code 0 when all pass | `boot_check.py:283-284`, `:524`, `:588-589` |
| 5 | `dbg` is `kernel.dev.dbg_1.obi` and `rel` is `kernel.dev.obi` | `boot_check.py:66-73` |
| 6 | `rel` must print `routing_checks: passed` and `init: echo exited with 0` | `boot_check.py:90-97` |
| 7 | The dev image starts `bin/init`; the other image starts `bin/shell` and has no root | `kernel/products/image/dev/BUILD.gn:269`; `kernel/products/image/dev.dbg_1/BUILD.gn:86`; `boot_check.py:86` |
| 8 | `--out` is configured on first use; two builds must not share a directory | `SKILL.md:49`, `:200` |
| 9 | `--no-tests` is for a loop of boots, never for the check before a commit | `SKILL.md:50-52` |
| 10 | The skill's "Reading a failure" table exists | `SKILL.md:108` |
| 11 | The skill documents a Mac host with `prebuilt/` only | `SKILL.md:184` |
| 12 | Git ignores `prebuilt/`; no script fills it: the fetch scripts take one tool each and `activate.py` reads a `prebuilt.yml` that is not in the tree | `git check-ignore prebuilt`; `scripts/fetch_ccache.py:1-5`; `scripts/fetch_llvm_coverage.py:2`; `scripts/activate.py:116` |
| 13 | echo provides `ours.examples.echo.Echo` with `EchoString` and `Stop`; Stop answers Ok and exits with 0 | `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:14-25`, `:56-67`; `examples/echo/echo_server.cpp:194-232` |
| 14 | `echo_server.cpp` is 283 lines; echo's three parts are the program, `protocol/` and `BUILD.gn` with `echo.cm` | `wc -l`; `examples/echo/BUILD.gn:33-49` |
| 15 | echo is an eager child of the root; routing_checks sends EchoString, then Stop, then expects NotFound | `kernel/products/image/dev/BUILD.gn:94-98`; `system/service/init/test/routing_checks/routing_checks.cpp:793-826` |
| 16 | `ioservice::Server` is the class that holds the connection table and answers ours.io | `system/lib/ioservice/README.md:8` |
| 17 | A handle is a number naming an entry of the process's own table, meaningless elsewhere; `Uandle` is an enum over u32; `Invalid` is 0 and closing it is a no-op | `system/public/ours/types.hpp:50-72` |
| 18 | Rights live on the handle; two handles to one channel may differ; a call checks the handle it is given | `system/public/ours/rights.hpp:12-14` |
| 19 | A channel is a two-ended pipe, each end with a queue the other writes; signals Readable, Writable, PeerClosed | `kernel/object/include/ours/object/channel_dispatcher.hpp:33-40` |
| 20 | A channel end has exactly one holder by default, so closing it tells the peer | `channel_dispatcher.hpp:14-18` |
| 21 | A message is at most 4096 bytes and 64 handles | `system/public/ours/syscall.hpp:43-46` |
| 22 | `channel_write` moves handles out of the sender's table: into the message, or closed on failure | `system/public/ours/syscalls.gni:203-205` |
| 23 | `channel_read` answers ShouldWait, PeerClosed, BufferTooSmall | `syscalls.gni:247-249` |
| 24 | A closed peer's last messages are still delivered first; BufferTooSmall leaves the message queued | `channel_dispatcher.hpp:64-72` |
| 25 | A queue holds 64 messages; a write to a full end answers NoResource and consumes the message | `channel_dispatcher.hpp:20-28`, `:57-62` |
| 26 | A process may have created at most 1024 live handles | `syscall.hpp:48-59` |
| 27 | A process exiting closes its handles, so far ends read PEER_CLOSED | `syscalls.gni:549-551` |
| 28 | Every system call here answers an `ours::Status`; the names NotFound, NoResource, BadHandle, ShouldWait, PeerClosed, AccessDenied, BadType exist | `system/public/ours/status.hpp:41-121` |
| 29 | Signals: Readable, Writable, PeerClosed, Terminated (a process once dead) | `system/public/ours/signals.hpp:29-37` |
| 30 | `object_wait_one` blocks until a signal is raised | `syscalls.gni:295-310` |
| 31 | `object_wait_async` is one-shot and level-triggered, queues a SignalOne packet carrying the key | `syscalls.gni:331-340` |
| 32 | `port_wait` takes the first packet; deadline 0 or INSTANT_INFINITE, any other NotSupported | `syscalls.gni:517-523` |
| 33 | A packet has a key and, for a signal wait, the trigger and observed signals | `system/public/ours/port.hpp:31-41`, `:107-128` |
| 34 | A port holds at most 1024 live waits, queued packets included; a wait past it answers NoResource | `port.hpp:160-167` |
| 35 | A program has no heap, so the Loop's table is the caller's, sized at build time | `system/lib/loop/include/loop/loop.hpp:13-19`, `:139-141` |
| 36 | A slot is free while its handle is Invalid; `add` takes the lowest free slot, answers NoResource when full, InvArgs for an invalid argument | `loop.hpp:149-151`; `system/lib/loop/src/loop.cpp:47-59` |
| 37 | The callback handles one unit of work and answers Rearm or Close | `loop.hpp:35-42`, `:50-53` |
| 38 | Never closes its own handle; Rearm only once the cause is consumed, else a spin | `loop.hpp:50-61` |
| 39 | Once `add` succeeds the handle is the Loop's; when it fails the handle stays the caller's, who may send an epitaph | `loop.hpp:198-212` |
| 40 | `run`: finds the slot by key, drops a stale or unknown packet, calls the callback once, then re-arms, or closes and frees on Close or a failed re-arm | `loop.cpp:103-146` |
| 41 | A key carries the index and a generation bumped each time the slot is freed | `loop.hpp:99-103`; `loop.cpp:146` |
| 42 | `run` returns only when `port_wait` fails; with INSTANT_INFINITE a server calls it last | `loop.hpp` comment on `run`; `loop.cpp:95-101`; `examples/echo/echo_server.cpp:279-282` |
| 43 | One thread, a callback runs to completion before the next, nothing is locked | `system/lib/ioservice/include/ioservice/outgoing.hpp:42-43` |
| 44 | One callback per packet, then re-arm behind everyone else's | `loop.cpp:128-130` |
| 45 | echo's table is `MAX_CLIENTS` plus `Outgoing::MAX_CONNECTIONS`, bounded by the port's limit; the port and Loop are made at the start of `main` | `echo_server.cpp:52-61`, `:258-264` |
| 46 | echo on a failed `add` writes an epitaph and closes the channel | `echo_server.cpp:132-138` |
| 47 | A component is a program and a manifest; one with no program only holds children; `component_manifest()` compiles to `meta/<name>.cm` in the boot image | `system/component_manifest.gni:5-6`, `:83-86` |
| 48 | `userboot` is the first user process and starts `bin/init`, the root; the root reads `meta/root.cm` and starts its children in manifest order | `dev.dbg_1/BUILD.gn:67`; `dev/BUILD.gn:269`, `:56-58`; `system/service/init/realm.cpp:312-322`, `:418-429` |
| 49 | Capabilities: protocols and directories; today the only directory is the root's built-in `boot` | `docs/architecture/component-framework/glossary.md:9` |
| 50 | Protocol type: the string saying which protocol, equal to the name when left out, there because an offer may rename | `glossary.md:11`; `system/component_manifest.gni:98-111` |
| 51 | Use is always from the parent; offer is in the parent's manifest with `as`; expose is in the child's | `glossary.md:12-14` |
| 52 | The three example manifests | `examples/echo/BUILD.gn:35-48`; `kernel/products/image/dev/BUILD.gn:186-191`; `system/service/init/test/routing_checks/BUILD.gn:85-88` |
| 53 | The root routes every Open again; failures answer NotFound, AccessDenied or BadType as epitaphs | `realm.cpp:738-745`; `docs/design/261029/protocol-identity.md`, "Error output" |
| 54 | processargs is the one message a process starts with: arguments, names, handles each tagged | `system/public/ours/process_args.hpp:5-20` |
| 55 | The root hands a component NsDir handles for its uses and `/svc`, a DirectoryRequest handle if it has a program; none to a component without one | `realm.cpp:356-378`, `:484-571` |
| 56 | An exit is logged `init: <moniker> exited with <code>`; later routes answer NotFound; never restarted | `realm.cpp:782-785`, `:849-857` |
| 57 | ours.io is the file and directory format; `Open` is one-way and a failed one is answered by an epitaph on the new channel; `fsio` fills the namespace before `main`; `connect` is `open` with ProtocolService and does not wait | `system/lib/ours.io/README.md:3`; `system/lib/fsio/include/fsio/fsio.hpp:35-42`, `:52-64`, `:82-87`; `system/lib/fsio/src/fsio.cpp:402-404` |
| 58 | The root serves one `/svc` per component that uses a protocol, an entry per used protocol; an Open there is routed, then forwarded as `Open("svc/<name>", ProtocolService, server end)` on the provider's outgoing client end, without waiting | `realm.cpp:536-561`, `:679-720`, `:726-748`, `:778-822` |
| 59 | The outgoing directory: `svc/` holds a service node per protocol; `Outgoing::add_protocol` and `serve`; a `Handler` receives each client's channel; echo adds it to its Loop | `outgoing.hpp:18-62`, `:120-143`; `echo_server.cpp:126-142`, `:265-277` |
| 60 | The directory request is taken with `ort::take_startup_handle(DirectoryRequest, 0)`; the name in a client's `/svc` is the use's, the name in `svc/` is the capability's | `echo_server.cpp:252`; `glossary.md:37`; `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:42-46`; `kernel/products/image/dev/BUILD.gn:183-190` |
| 61 | The message header (method number, transaction id, 0 for a one-way request and an epitaph); the epitaph is the 24-byte status form with txid 0 and the all-ones ordinal; a client checks `is_epitaph` before decoding a reply | `system/lib/wire/include/wire/wire.hpp:35-39`, `:78-88`, `:233-247` |
| 62 | An epitaph is written by the root on a failed route, by `ioservice` on a refused Open, and by a server for a connection it cannot take; Echo is a closed protocol and closes without an epitaph on an unknown method | `realm.cpp:279-288`; `system/lib/ioservice/include/ioservice/server.hpp:100-102`, `system/lib/ioservice/src/server.cpp:95-101`, `system/lib/ioservice/src/loop_server.cpp:163-169`; `echo_server.cpp:141`; `ours_examples_echo.hpp:39-40` |

### Not verified
Not asserted in the guide:

- The commands were not run. `boot_check.py` builds into `out/bringup.x86` and boots under QEMU; the working tree held other sessions' uncommitted kernel edits and the same output directory is theirs, so a run here would have proved little about the README. What the README says of the script comes from the skill and the script's own source, and `--help` ran.
- The exact text of a passing `rel` or `dbg` line was not seen; the README describes the format from `boot_check.py:524-526` and shows none.
- Where the toolchain under `prebuilt/` comes from: the tree does not say, so the README says so.
- That the quoted snippets stay at their lines: `examples/echo/echo_server.cpp` is moved to `loop::Waiter` by step 4 of `.agents/tasks/loop-waiter/PLAN.md`, and `system/lib/loop/src/loop.cpp` loses the callback form in step 6. Six snippets cite `examples/echo/echo_server.cpp` and one cites `system/lib/loop/src/loop.cpp`. The chapter's "Checked against commit" line says what they were true of; whoever lands those steps should re-run the check script, or re-read the cited lines.
- Behaviour of the kernel below the headers (the order in which a dying process's handles close, what `Writable` does under load): the chapter says only what the headers and the system call documentation say.

## Deviations from the design
- The README names the host as "the project's Mac host". The skill says "macOS host", and the contract says not to name other operating systems, so the OS is not named.
- `01-concepts.md` has no heading beyond the contract's six, "What you will be able to do", "Where to look next" and "Sources". The sub-structure inside a section is bold lead-ins and lists, not H3 headings.
- The plan points to "kernel headers for handles, channels, ports and signals under `kernel/include/ours/`". They are not there: the public ABI headers are in `system/public/ours/` and the channel's dispatcher is in `kernel/object/include/ours/object/`. The sources above are the ones read.

## Found along the way
- `prebuilt/` has no documented way to be filled: `docs/repairs/261029/prebuilt.md`, item 1 (Medium). The README says so instead of inventing steps.
- `system/lib/loop/include/loop/loop.hpp:16` and `system/lib/loop/README.md:3` say `ioservice::Outgoing` will use the Loop "later", and it does already: `docs/repairs/261029/loop.md`, item 1 (Low).
- `docs/architecture/component-framework/glossary.md:49` and `system/lib/loop/README.md:5` describe the Loop as "a port and a set of callbacks" and `Callback`; they go stale when `Callback` goes (loop-waiter step 6). Not a defect today; listed so that step 6 updates them.
- `system/lib/ioservice/README.md:3` and `:10`, `system/lib/loop/README.md:3` and `system/lib/wire/README.md:3` name other systems in prose; not a defect, but they are not text the guide can quote.
- `.agents/skills/ours-verify/SKILL.md` says where the kshell logs go (`$TMPDIR/ours-kshell-<out>/`) but not where `boot_check.py` writes its own: `$TMPDIR/ours-verify-<out dir name>/` (`boot_check.py:560`). The README gets it from the script's last output line. Minor, not filed.

## Not done
- Chapter 2 and the other four chapters are other agents' files.
- The README's section on adding a server of your own to the dev image is left to chapter 2, as the contract's list of README contents does not ask for it.
- No commit, as instructed.

## Revision
The cold read (`.agents/tasks/server-guide-review/REVIEW.md`) found the README and chapter 1 accurate and not yet usable. Both files were revised against the rulings of "3a. The revision rulings" in `.agents/tasks/server-guide/PLAN.md`, on HEAD `3cb0d8a5`. Nothing is committed or staged. Every `file:line` and snippet below was re-read in the tree at that commit: the `loop::Waiter` form has replaced the callback in `examples/echo/echo_server.cpp`, `system/lib/loop` and `system/lib/ioservice`, and every line number touching them moved.

### README.md
- **H1 (a newcomer cannot get a first build).** Done. A "Before you start" section now sits above the command. It states the `prebuilt/` prerequisite as a prerequisite, links `00-setup.md`, names the one host that is described (macOS on Apple silicon) and says the script neither checks for the tools nor says where to get them. The command is followed by a sample of the real output (the build, `dbg`, `rel`, host-tests and logs lines from `boot-routing-checks.log`, paths shortened to `…`, times and counts said to vary) and a line-by-line reading of it. The old closing "limits" paragraph, which promised only a Mac host, is gone.
- **H2 and T4 (nothing takes the reader from `main()` to a running server; chapter 2 missing).** Done. Chapter 2 is listed with a link and a description of its levels as the chapter has them (0 to 5). The status box shrank to the two limits that are still true (no installer for `prebuilt/`; a tree server of your own cannot be reached), and the interim "copy echo and follow chapter 1" path is removed. The README's "How to read it" sends a first server to chapter 2, with chapter 3 beside it.
- **H4 (tree-server limit).** Done. The limit is stated in "What a server is", in "Status of this guide", in the chapter 4 line of "Chapters", and in "What you will be able to do" (the tree bullet now says "understand how the root's `/boot` and every outgoing directory are built, and use the tree library in a host test"). The "two kinds" against "or both" wording is settled: the kinds are not exclusive in what they are built from (an outgoing directory is itself a tree), and what separates them is what a client sees.
- **M12 (order).** Done: who it is for, what a server is, the running example, status, before you start, build and boot, chapters, how to read it. "What to skip on a first pass" is added under "How to read it". The running example comes before the boot because the boot output is read in terms of it. No heading was renamed; "Before you start" and "What to skip on a first pass" are new.
- **M4 (dev image).** Done. The dev image is defined once, at the head of "Build and boot the dev image": `kernel.dev.obi`, which the script calls `rel`; `kernel.dev.dbg_1.obi` (`dbg`) is the debug image and starts no components. The intro links to it.
- **M5 (obi, QEMU, GRUB).** One clause each, in "Build and boot the dev image".
- **M9 (insider language).** "The `ours-verify` skill's script" is now "the verify script". `.agents/` appears as the script's path, once as its documentation (`SKILL.md`, for the failure table), and in Sources.
- **Minor.** "About 280 lines" is now "about 300" (`wc -l` of `echo_server.cpp`). The README said the script requires two lines on `rel`; it requires six (`REQUIRED` in `boot_check.py`), and now says so.

### 01-concepts.md
- **M1 (memory).** New section "Where a server's memory comes from" (`#where-a-servers-memory-comes-from`) after the Loop section: no heap, no constructors for globals, no `__cxa_atexit`, so `constinit` and `ustl::LazyInit`, with echo's three declarations and the `init()` lines from `main`. It links chapter 4's storage section for a tree's sizing. The Loop section points forward to it where it first shows `SLOT_STORAGE`.
- **M2 (root).** "Components and manifests" now says the root is the component (manifest `meta/root.cm`, no program of its own) and `init` the program that runs it.
- **M3 (rights).** `AccessDenied` is "for a directory use only"; "A protocol use states no rights". The three meanings of *rights* are told apart in "Routes" (handle rights, ours.io rights, the rights words `r*` and `rx*`), `rx*` and `r*` are defined at the first manifest line that has one, and the first section points forward.
- **M4 (double meanings in this chapter).** *dev image* links the README's definition; *tree* is qualified ("component tree", "node tree"); *connection* is defined once, in the outgoing-directory section, and the epitaph section says *channel* where it meant a protocol server's channel; *method number* is the only name used here (chapter 3 equates it with *ordinal*, as the rulings say); *root* settled by M2.
- **M5 (undefined terms).** New section "Words used elsewhere" (`#words-used-elsewhere`) with VMO, built-in, `ort`, `ustl`, vDSO and obi. The Loop section defines *epitaph* by a forward note. `processargs` is dropped, and *startup handles* are defined in a clause at the head of "Components and manifests". `PeerClosed` is said to be both a status and a signal. `INSTANT_INFINITE` is explained where `object_wait_one` first appears.
- **M6 (`01:88`).** The `run` text now has a snippet that shows `port_wait` (`loop.cpp:75-81`), next to the `arm` snippet.
- **M10 (density).** "Components and manifests" is split into three subsections: "Components, manifests and the root", "Who may reach whom" and "Routes". `processargs` is gone and the terms are at most six in a subsection. The three manifests have their home here: `echo.cm` from the line that opens it, `routing_checks.cm` from its opening line with its first use, the root's `children` entry and the offer. Inline `file:line` citations fell from about 20 to 9 in prose; the rest are beside a snippet or in a "Where this is stated" list at the end of a section.
- **M9 (insider language).** The design-document entries are removed from Sources (the facts they gave are stated in the code and in the architecture documents); no change-record, repair, decision-number or history wording remains.
- **H3 (the interfaces are never shown).** The `loop::Waiter` declaration (`loop.hpp:74-83`) and the `ioservice::Handler` declaration (`outgoing.hpp:50-62`) are shown. The text says `EchoServer` is echo's own class, derived from `Handler`, and `EchoServer::Client` is a `Waiter`; `m_loop->add(...)` (`echo_server.cpp:149`) shows how a client's channel is registered, and chapter 2 is linked for the whole class.
- **Minor.** "One is all dev's routing checks use" is in prose as "the dev image's `routing_checks`" (the snippet now begins at `echo_server.cpp:54`, after that comment); `init: <moniker> exited with <code>` is explained once with an example; the half-sentence about `Stop` is before the snippet that names it; the "Where to look next" chapter 2 line is a link.

### Not fixed, and why
- **M10, whole manifests.** `echo.cm` is 17 lines and the rulings cap a snippet at 15, so it is shown from its opening line to the end of `expose`, "whole but for its two closing braces". The root's manifest cannot be shown whole in 15 lines; it has an outline tree with the line where the target opens, and two snippets. `routing_checks.cm` shows its head and one use; its protocol use is a second snippet.
- **`01:50` wording.** The finding proposed "may hold at most 1024 handles at once". The limit counts the handles a process created, wherever they now are (`syscall.hpp:47-59`), so "hold" would be wrong; the text says a handle sent to another process still counts against its creator.
- **M12, memory before the Loop.** The finding wanted the memory text before the Loop's first use of it; the rulings place it after, so the Loop section links forward.
- **Duplicates in other chapters.** `02-first-server.md` shows the `Waiter` declaration (around line 326) and `Handler` (around line 215) again; chapter 1 is their home, so chapter 2 could link instead. Left to the owner of that chapter.

### Not verified
- `02-first-server.md` was written in parallel and was read only for its levels and its links into chapter 1; the README's description of its levels follows the draft I saw.
- The sample output is quoted from `boot-routing-checks.log`, not re-run; the boot was not run here.
- That a global with a destructor does not link was read from the comment at `echo_server.cpp:255` and from the absence of a `__cxa_atexit` definition under `system/`; no test program was linked.
- The expansion of the letters in VMO and obi is not stated anywhere in the tree, so the guide gives none. "Debug image, built with assertions on" is from the comment on `IMAGES` in `boot_check.py`, not from the build files.

### New defects
- `docs/repairs/261029/loop.md`, item 1 (Low): `docs/architecture/component-framework/glossary.md:49` still describes the Loop as "a port and a set of callbacks".

### Checks run
- `python3 scripts/lint_docs.py --changed` on `docs/guide/README.md`, `docs/guide/01-concepts.md`, `docs/repairs/261029/loop.md` and this file reports no violations.
- A script (`check_concepts.py` in this session's scratchpad) found all 20 `cpp` and `gn` snippets of chapter 1 verbatim at their cited lines, none longer than 15 lines; every `file:line` citation of both files names an existing line; all 44 relative links and anchors resolve, including `02-first-server.md`; no file names another system. The bare `:NNN` citations in "Where this is stated" lists were read by hand.
