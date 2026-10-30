# server-guide-protocols: chapter 3 of the server guide, how to define a protocol, drafted

**Date**: 2026-10-29

## Delivered
- `docs/guide/03-protocols.md`, 19 verbatim snippets of at most 15 lines, a section `## Closed protocols` (the anchor `#closed-protocols`), a closing section "How to add a new protocol library" in eight numbered steps, a "Sources" list and the line "Checked against commit `c2b7a75a` on 2026-10-29."
- Four repair items, in three files, for defects found on the way: `docs/repairs/261029/wire.md` (2 items), `docs/repairs/261029/ours-resource.md` (1) and `docs/repairs/261029/tools-templates.md` (1).
- Nothing is committed or staged, as the task says.

## Verification
- `python3 scripts/lint_docs.py --changed docs/guide/03-protocols.md`: no violations; the same for the three repair files.
- A checking script (kept in the session scratchpad, not in the tree) compared every `file:line` cite of the chapter against its file: 146 distinct cites (156 occurrences): 134 are checked for content (each phrase the text relies on was found in the cited lines) and the other 12 are ranges of the 19 verbatim snippets, each of which equals its file lines exactly. The 134 content-checked cites are the facts below.
- `python3 system/lib/wire/test/ordinals_test.py`: 5 tests, OK.
- Every relative link of the chapter resolves: `01-concepts.md` and `04-tree-servers.md` exist; `01-concepts.md` has the six headings the contract names, so its anchors resolve; `05-testing-and-debugging.md` and `06-checklist.md` do not exist yet (other agents write them).
- Run, not only read: the real encoders (`wire.cpp` and `ours_examples_echo.cpp`, compiled on the host with `clang++ -std=c++23` and `-include kernel/include/ours/prelude.hpp`, in the scratchpad) produced the byte dumps in the chapter: a 25-byte `EchoString` request, a 29-byte Ok reply, a 24-byte failed reply (status 7) and a 24-byte `NotFound` epitaph. The ordinal `0x6e806bd776386789` and the digest `89 67 38 76 d7 6b 80 ee` were recomputed with hashlib.
- Run, not only read: `python3 -m copier copy --defaults` of `tools/templates/cpp_library` into the scratchpad (not the tree) produced the eight files the chapter lists; a minimal GN project of the generated shape reproduced "Unresolved dependencies"; a temporary root with a `.ordinal = 0x0` declaration reproduced the failure message that names the right value.

Facts checked, each against the file and lines named:
- Header layout (txid 0-3, direction 4, reserved 5-6, magic 7, ordinal 8-15): `system/lib/wire/include/wire/wire.hpp:78-89`, `system/lib/wire/test/wire_test.cpp:31-35`. Little-endian: `wire.hpp:100-102`. Body from 16, no implicit padding, one trailing array: `wire.hpp:22-26`, `system/lib/ours.io/include/ours_io/ours_io.hpp:41-42`.
- Direction byte and why: `wire.hpp:46-51`. Magic is the version of the framing: `wire.hpp:57-59`.
- txid rules and `MAX_TXID`: `wire.hpp:70-72`, `system/lib/wire/src/wire.cpp:24-29`.
- Method number: `wire.hpp:113-119`, `system/lib/wire/test/ordinals_test.py:46-49`, `:37`, `:58-72`, `:114-118`; top bit cleared so none equals the epitaph's: `wire.hpp:66-68`. Run by hand and by nothing else: `system/lib/wire/BUILD.gn:26-27`, plus a search of `.agents/skills`, `scripts`, `.githooks`, `.claude` and the root `BUILD.gn` for "ordinals" with no hit.
- Request decoding refusals: `wire.cpp:49-60`, `:62-73`; encoding refusals: `wire.hpp:190-210`, `wire.cpp:104-138`.
- Reply forms: `wire.hpp:31-39`, `wire.cpp:75-102` (cited as `:93-100`, `:94-95`, `:96-98`), `:134-138`. Status numbers fixed and refused past the last: `system/public/ours/status.hpp:10-15`, `wire.cpp:15-21`.
- Epitaph form, encoder, decoder, `is_epitaph`: `wire.hpp:35-36`, `:66-68`, `:221-244`, `wire.cpp:157-173`; a client's use of it: `system/lib/ours.resource/src/client.cpp:63-86`.
- Closed protocols: `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40`, `system/lib/ours.resource/include/ours_resource/ours_resource.hpp:45-47`, `system/lib/ours.process/include/ours_process/ours_process.hpp:50-57`, `ours_io.hpp:45-50`, `system/lib/ioservice/include/ioservice/server.hpp:83-90`, `system/lib/ioservice/src/server.cpp:562-565`, `examples/echo/echo_server.cpp:154-156`, `:172-191`, `:141`.
- A message written before a close is still delivered: `kernel/object/include/ours/object/channel_dispatcher.hpp:67-70`.
- Handles: `wire.hpp:61-64`, `:126-132`, `:249-261`, `wire.cpp:175-195`; `system/lib/ours.resource/src/ours_resource.cpp:50-60`, `:72-79`; ownership: `system/public/ours/syscalls.gni:204-207`, `kernel/lib/syscall/src/channel.cpp:183-193`, `echo_server.cpp:188-190`; per-process limit of 1024: `system/public/ours/syscall.hpp:59`; a `Uandle` is a table index: `system/public/ours/types.hpp:47-72`.
- Size limits: `syscall.hpp:36-46`, `kernel/lib/syscall/src/channel.cpp:72`, `:107`, `channel_dispatcher.hpp:20-28`, `:57-61`, `wire.hpp:74-76`, `wire.cpp:51`; echo's bound and `static_assert`: `ours_examples_echo.hpp:48-49`, `examples/echo/protocol/src/ours_examples_echo.cpp:24`; `ours.io`'s bounds: `ours_io.hpp:56-65`; bulk data by handle: `ours_io.hpp:38`.
- No version for a protocol: `docs/design/261029/protocol-identity.md:30`; every peer built in one build: `docs/changes/261004/ours-io.md:29`.
- Names: `ours_process.hpp:59-60`, `ours_resource.hpp:49-50`, `system/lib/ours.resource/BUILD.gn:10-14`, `ours_examples_echo.hpp:42-47`, `echo_server.cpp:268`; protocol type: `system/component_manifest.gni:62-65`, `:109-111`, `kernel/products/image/dev/BUILD.gn:183-191`, `system/service/init/test/routing_checks/BUILD.gn:85-88`, `system/lib/component/include/component/route.hpp:303-306`, `system/service/init/realm.cpp:188-197`, `:740-743`.
- Larger examples: `ours_process.hpp:17-21`, `:33-48`, `:74-102`; `ours_resource.hpp:35-39`, `:51-56`, `ours_resource.cpp:30-32`.
- Library files, GN targets and the test: `examples/echo/protocol/BUILD.gn:9-12`, `:13-23`, `:28-36`, `:39-42`; `examples/echo/protocol/test/BUILD.gn:4-13`; `examples/echo/BUILD.gn:24-28`, `:36-48`; `examples/echo/protocol/test/ours_examples_echo_test.cpp:26-33`, `:70`, `:96-103`; the root `BUILD.gn:212-214`; `scripts/host_tests.py:5-12`.
- The template: `tools/templates/cpp_library/copier.yml:3`, `:5-9`, `:13-33`, `.clang-format.jinja:13`; the file list, the generated `BUILD.gn` and the test's dependency label by running copier into the scratchpad.

### Not verified
- That a client reads `PeerClosed` after a silent close on the booted image: the chapter relies on `docs/design/261029/protocol-identity.md:11-17` and the `channel_read` documentation (`syscalls.gni`), not on a boot. The same for the root's epitaph `BadType` reaching a component: read in `realm.cpp:740-743`, not booted.
- Nothing outside the repository runs `ordinals_test.py`: only the repository's scripts, skills and hooks were searched, not any CI the user may have elsewhere.
- That no written policy exists for changing a protocol: searched `docs/design/261004`, `docs/design/260924`, `docs/architecture/component-framework`, `system/lib/wire` and the `ours.io` README for "compat" and "version" only. The chapter says the tree has none and labels its advice on changes as a consequence of the rules, not a promise.
- That the interactive prompts of copier ask what `copier.yml` declares: only `--defaults --data` was run. The command in the chapter is the one in `copier.yml:3`.
- The existence and anchors of `05-testing-and-debugging.md` and `06-checklist.md`, which other agents write.
- The wording of the Loop's `Close` answer: the chapter says only that the callback answers `Close` and the Loop closes the channel, which `echo_server.cpp:144-149` states in a comment; `loop.hpp` was not read, as the contract says.

## Deviations from the design
None. The chapter follows "### 1. The contract" and the `guide-protocols` bullet of `.agents/tasks/server-guide/PLAN.md`. The skeleton has no "Not verified" heading, so it is a third-level heading under "Verification".

## Found along the way
- `docs/design/260924/userspace-filesystem.md` section 2.5 (lines 163-164) and line 858 say an unknown or malformed message gets an epitaph; the code and the 261004 design close without one, and `wire.hpp:19-20` cites the older document. Recorded in `docs/repairs/261029/wire.md`, item 1.
- `system/lib/wire/test/ordinals_test.py` is run by nothing, so a wrong or copied ordinal passes the boot check. `docs/repairs/261029/wire.md`, item 2.
- The README, the `BUILD.gn` comment and the header comment of `ours.resource` name three protocols; the header declares five. `docs/repairs/261029/ours-resource.md`, item 1.
- The `cpp_library` template generates a target named after the library and a test that depends on the directory label, which do not match for any protocol library. `docs/repairs/261029/tools-templates.md`, item 1.
- Smaller gaps, not worth a repair item of their own:
  - `ours_examples_echo.hpp:44` says "(@discoverable)", an attribute that nothing in this tree defines.
  - `system/lib/wire/README.md:5` lists three codec libraries built on `wire` and leaves out the echo protocol.
  - The generated test names its suite with `name.capitalize()`, so `ours_examples_hello` becomes `Ours_examples_hello`, where echo's suites are `OursExamplesEchoStop` and the like.
  - `wire::names_a_status` (`wire.cpp:19-21`) accepts every value below `MaxCount`, including the statuses `status.hpp` marks "never returned to a program" (`Delayed`, `InternalIrqKilled`, `InternalIrqRetry`). Harmless on the wire as far as read, but the comment on that function does not say it is deliberate.

## Not done
- The chapter does not teach the server side of a protocol (the Loop, `ioservice::Outgoing`, the callback's rules): chapter 2, which waits for step 4 of `.agents/tasks/loop-waiter/PLAN.md`, and chapter 6 hold that.
- The four repair items are unclaimed; they are listed in `docs/repairs/261029/`.

## Revision

**Date**: 2026-10-29

The revision pass after the cold read (`.agents/tasks/server-guide-review/REVIEW.md`), under the rulings of `.agents/tasks/server-guide/PLAN.md` section 3a. The sections above describe the first draft; the figures here replace theirs. The chapter now has 783 lines (675 before), 20 verbatim snippets of at most 15 lines (19 before), 5,178 words outside code fences become 6,408, and it ends "Checked against commit `3cb0d8a5` on 2026-10-29", the hash of `git log -1 --format=%h -- system examples tools scripts build kernel/products`. It grew, because the findings ask for code shown and gaps explained; what it gained for a first reader is the "In short" and the Reference marks, not brevity.

Finding by finding (the review's ids):

- M12 (order): fixed. A ten-bullet "In short" follows the opening; "A message on the wire", "How large a message can be" and "What is versioned and what is not" begin with a *Reference* line saying when to read them. The recipe ("How to add a new protocol library") now follows "Methods and their numbers", not "Names and the protocol type", because the recipe's last step and "Adding a method" need the method numbers first. No heading was renamed or deleted; "In short" is the only heading added.
- M8 (recipe described, not shown): fixed. Step 3 shows the `BUILD.gn` the template writes (the real output of copier for the example's answers), explains `source_set`, `public_deps` and `cpp_interface("headers")`, and lists five changes (the old fourth item held two). Step 4 says what changes for a method with no arguments (`StopRequest`), a reply that can fail (`EchoStringReply`) and a method that returns nothing (`encode_stop_reply`). Step 5 shows echo's request encoder and decoder and the Ok path of its reply encoder, and cites the reply decoder. The reason for the closed protocol is given in full from the code's own comment, and then the layout argument, labelled as an inference from the layouts. The old `:168` sentence is rewritten as three bullets.
- M6 (snippets that do not show): fixed. `release` is shown beside the branch that calls it. The 12-line layout table of the header is shown, with `size@16`, `vmo@20 (handle)`, `(nothing)` and the shifted offsets of an Ok reply explained.
- M4 (words): fixed. *Method number* and *ordinal* are equated once, in "Words used throughout this chapter", and prose says method number; *selector* is defined there too; *client entry* (echo's `Client`, which is a `loop::Waiter`) and *slot* (the Loop's table entry) are told apart under "Closed protocols"; *connection* is gone from the prose (a code comment in a quoted snippet keeps it); *tree* is "repository" or qualified ("component tree", "directory tree"); the root's program is `init`.
- M5 (undefined terms): VMO and `ustl` link `01-concepts.md#words-used-elsewhere`; "memory object" became VMO.
- M9 (insider language): fixed. `CLAUDE.md` is gone (the GN formatting rule is cited at `scripts/format_gn.py:6-11`); `.agents/` appears on one line, the script's path; `SKILL.md`, the change record, every `docs/design` and `docs/changes` citation and the history words ("written by hand today", "before the protocol type existed") are gone. The facts they backed are restated from code: that a type string says nothing of layout (`route.hpp:352-354`), why a mixed-up route shows as a silent close, and that an image is built from one checkout.
- M10 (density): fixed in part. The paragraph on names is two sentences and a table; "Adding a method" is a numbered list; `write()` is linked to chapter 1, as is `close_with_epitaph`; the boot-check description is a link to the README's "Build and boot the dev image". Citations stay, but beside snippets and non-obvious claims, and the rest sit in a "Where this is stated" paragraph at the end of a section. Their count did not fall (151 cites, 142 distinct, against 156 and 146). Wiring a test into `//:default` has its home in chapter 5 ("Adding a host test of your own", which already says "the wiring above"), so step 7 is now a pointer with echo's entry named, and the snippet is gone.
- H3 (the Loop's registration): fixed. The chapter says that echo's `Client` is a `loop::Waiter` whose `on_signals` calls `serve`, links `01-concepts.md#the-loop-and-its-slots` (which shows the `Waiter` declaration) and `02-first-server.md`. "This chapter does not describe it" is gone.
- H2 (manifest snippets begin inside a structure): fixed for echo's manifest, which is shown from `component_manifest("echo.cm") {` to the closing brace in two blocks (`examples/echo/BUILD.gn:33-35` and `:36-49`; together 17 lines, over the limit as one). The `use` entry of `routing_checks` still begins at its `{`, because the list it sits in is 36 lines long; the text names the list and the line where it opens (`:53`). The root manifest's child entry and the offer are chapter 2's, not this chapter's.
- Minor `03:465` (what `ours.io`, `ours.process` and `ours.resource` are): fixed, one clause each at the first mention.

Not fixed, and why:

- The heading "### 8. Compute the ordinals and run everything" keeps the word *ordinals*: the rulings forbid renaming a heading. Its first step says what the word means.

Verification of the revision (scripts in the session scratchpad, not in the tree):

- `python3 scripts/lint_docs.py --changed docs/guide/03-protocols.md`: no violations.
- 151 `file:line` cites in prose: every file exists and every range lies inside it. The 35 cited files are unchanged since the first draft's commit (`git diff c2b7a75a HEAD` is empty) except `examples/echo/echo_server.cpp` and `system/service/init/realm.cpp`; every cite into those two was re-read and renumbered, and 26 substring checks on cited ranges (those two files and the new cites) found the words the text relies on.
- 20 snippets compared line by line with the lines they cite: identical, none over 15 lines. The generated `BUILD.gn` block equals the file `python3 -m copier copy --defaults` wrote into the scratchpad for the example's answers; echo's own `.clang-format` equals what the template writes for its name.
- 30 links: every relative link and anchor resolves, including `01-concepts.md#words-used-elsewhere`, `02-first-server.md`, `05-testing-and-debugging.md#adding-a-host-test-of-your-own`, `README.md#build-and-boot-the-dev-image` and `00-setup.md#python`.
- Run, not only read: `python3 system/lib/wire/test/ordinals_test.py` (5 tests, OK); `out/bringup.x86/host/obj/examples/echo/protocol/test/ours_examples_echo` run from `out/bringup.x86` (7 tests passed; a build of 2026-10-23, which confirms the path form of step 8, not that today's source passes); the failure line of "Adding a method" taken from the real test run on a throwaway tree holding the `Hello.Greet` declaration with `.ordinal = 0x0` (the number it names, `0x0b34f151d5b5de80`, is the one chapter 2 uses); GN's `Unresolved dependencies` for the generated name reproduced in a minimal GN project of that shape; `gn help visibility` of the repository's `prebuilt/gn/gn` says the default is public; the four hex dumps recomputed with Python's `struct`.

Not verified: that a client reads `PeerClosed` after a silent close on a booted image, and that `init` writes `BadType` on a booted image (both read in code, not booted, as before); the sentence "this guide found no written policy in the repository for changing a protocol" is an absence claim, searched only as the first pass did; "why no answer" is the chapter's inference from the layouts, and says so; nothing was built or booted for this revision.

Found along the way: no new defect. The four repair items of the first pass stand.
