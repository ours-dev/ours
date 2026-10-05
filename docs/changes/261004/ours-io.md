# ours-io changes

## Title: the protocol libraries move into ours's own namespace: ours.io, ours.process and ours.kernel, with ordinals hashed from the new selectors (2026-10-04)

**Files**
- `system/lib/fuchsia-io/` -> `system/lib/ours-io/` — directory, GN target, include prefix (`<ours_io/ours_io.hpp>`), source and test names (`src/ours_io.cpp`, `test/ours_io_test.cpp`, gtest target `test:ours_io`), namespace `ours_io`; every selector is now `ours.io/<Protocol>.<Method>`, Open's is `ours.io/Directory.Open`, and every ordinal is recomputed
- `system/lib/fuchsia-process/` -> `system/lib/ours-process/` — the same moves, namespace `ours_process`, selectors `ours.process/Launcher.<Method>`, `LAUNCHER_NAME` is `ours.process.Launcher`
- `system/lib/fuchsia-kernel/` -> `system/lib/ours-kernel/` — the same moves, namespace `ours_kernel`, selector `ours.kernel/DebugResource.Get`, `DEBUG_RESOURCE_NAME` is `ours.kernel.DebugResource`
- `BUILD.gn` — the host test list names `//system/lib/ours-io:tests`, `ours-process:tests` and `ours-kernel:tests`; `//benches:benches` is written `//benches`, the layout gn gives it, which the pre-commit check needs before it accepts any edit of this file
- `system/lib/vfs/`, `system/lib/fsio/`, `system/lib/launch/`, `system/lib/svc/`, `system/lib/component/` — includes, dependencies and every `fuchsia_io::`, `fuchsia_process::` use renamed
- `system/service/init/` and `test/routing_checks/` — the root's built-in table, its servers, routing_checks and its manifest use the new names
- `examples/shell/` (`shell.cpp`, `shell_tests.cpp`, `BUILD.gn` with `shell.cm`), `examples/echo/protocol/BUILD.gn` — the shell's /svc paths and manifest
- `kernel/products/image/dev/BUILD.gn` — root.cm offers `ours.process.Launcher` and `ours.kernel.DebugResource`
- `system/lib/component/test/fixtures/BUILD.gn`, `test/decl_test.cpp`, `test/route_test.cpp`, `system/test_component_manifest.py` — fixtures `fuchsia.example.*` become `ours.example.*`
- `system/lib/wire/include/wire/wire.hpp`, `system/lib/wire/test/ordinals_test.py`, `system/lib/wire/README.md` — the ordinal rule is stated as ours's own; the test's hasher is `method_ordinal()`
- `system/component_manifest.py` — comments only, and one error message that named a foreign constant
- `docs/design/261004/component-framework.md` — D20's Decided cell records the revision; the open question on library names is closed with the new names
- `.agents/tasks/microkernel-interrupts/DESIGN.md`, `PLAN.md` — the planned built-ins are `ours.kernel.IrqResource` and `ours.kernel.IoportResource`, in `system/lib/ours-kernel`
- `.agents/skills/ours-kernel-design/references/boot.md` — the protocol names it states
- the READMEs of the three libraries, of vfs and of fsio — names and the ordinal rule

**Symptom**
None; this carries out the user's decision of 2026-10-04 evening. Nothing failed before it: the three libraries spoke Fuchsia's protocol names with Fuchsia's ordinals, and every peer on the target was built from this tree.

**Cause**
Two decisions of the user, one about names on the wire and one about comments.

- **Why the names had to change, not only the directories.** The codecs were built under design D20 option (a): ours's own 16-byte header with a direction byte, flat bodies at fixed offsets, and Fuchsia's protocol names and ordinals. The bodies already made the protocols ours's own: a Fuchsia peer could not decode a single message, since FIDL's wire format has envelopes, out-of-line objects and alignment rules the flat bodies do not follow, and FIDL's header has no direction byte. So the borrowed names promised a compatibility that the bytes did not keep. Worse, they made a reader look up a foreign protocol to understand ours's: the selector of Close was `fuchsia.io/Node.Close` although the method was declared in `fuchsia.unknown.Closeable`, Open hashed `fuchsia.io/Directory.Open3` because it had been renamed from Open3, and ReadDirents hashed `fuchsia.io1/...` after a protocol that no longer exists. Each of those is history of another system, carried in ours's code. With ours's own library names every selector is simply `<library>/<Protocol>.<Method>` as ours declares it.
- **Why every ordinal changes, and why that is safe here.** An ordinal is not a number someone picks: it is the first 8 bytes of SHA-256 of the selector, little-endian, top bit cleared. The library name is part of the selector, so renaming the library changes every ordinal, all at once. That is the point of hashing: two libraries can never be given the same number by hand, and a method's number follows from its name. It is also the risk: a client and a server built with different selectors disagree on every ordinal, and the server answers each message as an unknown method by closing the connection, which looks exactly like any other refusal. Two things make the switch safe. First, every peer is built from this tree in one build, so there is no old binary left to talk to. Second, nothing types an ordinal by hand without a check: `ordinals_test.py` recomputes each `wire::Method` declaration from its selector, and the boot test exercises every method on the target (routing_checks opens, reads, lists, clones and closes; the shell spawns through the Launcher and gets the Debug resource). A mismatch on either side would fail one of the two.
- **Why the /svc names change with them.** A protocol's discoverable name is the library and the protocol joined by a dot, and it is both a capability name in the manifests and a file name in every component's /svc. The root's built-in table, root.cm's offers, shell.cm's and routing_checks.cm's uses, the shell's and routing_checks' `/svc/...` paths and `LAUNCHER_NAME` and `DEBUG_RESOURCE_NAME` all name the same string, and routing compares names byte for byte. Missing one would not fail the build: the route would answer NotFound at boot. That is why the change is one commit, verified by a boot in which routing_checks passes.
- **Why comments were rewritten in every file this touched.** The user's second decision (CLAUDE.md, 2026-10-04): a code comment says what a thing is and why, in this codebase's own terms, and never names Zircon, Fuchsia or Linux as a design reference. A comment that says "as fdio's CleanPath does" asks the reader to know fdio to understand fsio, and it goes stale the moment either side changes. The comparison is still worth keeping, for learning, but it belongs in the change record's Reference and in the design documents, which say when and against which revision it was made. So where a comment explained a choice by pointing at Fuchsia, the rewrite states the reason itself: the Add* calls are one-way and cannot carry a refusal, so a Launcher that cannot hold one more argument sets a mark and refuses at Launch; a route that fails to resolve a child answers NotFound because to the component using it, the route leads nowhere; an unknown method closes the connection because the header has no bit that would mark a call as one an older server may answer "unknown" to.

**Change**
A method declaration, before and after:

```cpp
// before: system/lib/fuchsia-io/include/fuchsia_io/fuchsia_io.hpp
inline constexpr auto READ = wire::Method{
    .selector = "fuchsia.io/File.Read",
    .ordinal = 0x057e'419a'298c'8ede,
    .kind = wire::MethodKind::TwoWay,
};

// after: system/lib/ours-io/include/ours_io/ours_io.hpp
inline constexpr auto READ = wire::Method{
    .selector = "ours.io/File.Read",
    .ordinal = 0x2348'9a6f'12eb'b0f0,
    .kind = wire::MethodKind::TwoWay,
};
```

Every method, old selector and ordinal, new selector and ordinal:

| Method | Before | After |
|---|---|---|
| Directory.Open | `fuchsia.io/Directory.Open3` `0x568ddcb9a9cbb6d9` | `ours.io/Directory.Open` `0x3d1ddd821049bce2` |
| Node.Close | `fuchsia.io/Node.Close` `0x5ac5d459ad7f657e` | `ours.io/Node.Close` `0x67bfe5a2cd64acda` |
| Cloneable.Clone | `fuchsia.unknown/Cloneable.Clone` `0x20d8a7aba2168a79` | `ours.io/Cloneable.Clone` `0x151b66f76607428b` |
| File.Read | `fuchsia.io/File.Read` `0x057e419a298c8ede` | `ours.io/File.Read` `0x23489a6f12ebb0f0` |
| File.GetBackingMemory | `fuchsia.io/File.GetBackingMemory` `0x00a6a9e654cbf62b` | `ours.io/File.GetBackingMemory` `0x13b03c8a57b06719` |
| Directory.ReadDirents | `fuchsia.io1/Directory.ReadDirents` `0x3582806bf27faa0a` | `ours.io/Directory.ReadDirents` `0x3acfc05d99efc34b` |
| Launcher.AddArgs | `fuchsia.process/Launcher.AddArgs` `0x3be445d3e4fd6512` | `ours.process/Launcher.AddArgs` `0x617727c98e66bde8` |
| Launcher.AddHandles | `fuchsia.process/Launcher.AddHandles` `0x51025267a537a615` | `ours.process/Launcher.AddHandles` `0x329251399bdcbf52` |
| Launcher.AddNames | `fuchsia.process/Launcher.AddNames` `0x2579ee2c7be28662` | `ours.process/Launcher.AddNames` `0x0987826fa9690f15` |
| Launcher.Launch | `fuchsia.process/Launcher.Launch` `0x11335a9928afbfa4` | `ours.process/Launcher.Launch` `0x48425238011ea49e` |
| DebugResource.Get | `fuchsia.kernel/DebugResource.Get` `0x1d79d77ea12a6474` | `ours.kernel/DebugResource.Get` `0x01f00d3fa678897f` |

The ordinals were recomputed with the rule `ordinals_test.py` checks, by a script that rewrote each `.ordinal` from its `.selector`, not typed. The echo example's `ours.examples.echo` ordinals did not change.

Names that reach /svc and the manifests:

```text
fuchsia.process.Launcher       ->  ours.process.Launcher
fuchsia.kernel.DebugResource   ->  ours.kernel.DebugResource
fuchsia.example.*  (fixtures)  ->  ours.example.*
```

The test suites follow the namespaces (`FuchsiaIoOpen` -> `OursIoOpen`, `FuchsiaProcessAddArgs` -> `OursProcessAddArgs`, `FuchsiaKernel*` -> `OursKernel*`), and `ordinals_test.py`'s hasher is `method_ordinal()`, its known-answer test `test_hasher_known_answer`.

The comment rewrite covered every file this change touched. Where a comment carried only a comparison (a foreign function, a source path, "as Fuchsia does"), the comparison was dropped; where it carried a reason, the reason was kept in ours's terms. For example, in `system/lib/ours-process/include/ours_process/ours_process.hpp`:

```cpp
// before
/// A message is exactly as long as its fields. A failed Launch is the 24-byte
/// status form, as Fuchsia's reply carries no process unless the status is
/// ZX_OK.

// after
/// A message is exactly as long as its fields. A failed Launch is the 24-byte
/// status form: a reply carries a process only when its status is Ok.
```

Rejected: keeping the old selectors through a selector override, as Fuchsia's `@selector` does, so that the ordinals would not move. The ordinals have no reader outside this tree, so keeping them would preserve nothing, and it would keep exactly the foreign history the rename removes.

**Reference**
The ordinal rule is fidlc's method hasher: `Sha256MethodHasher` in Fuchsia's `tools/fidl/fidlc/src/compiler.cc`. The known answer `ordinals_test.py` checks the hasher against, `test.anonymous/SomeProtocol.SomeMethod` = 2515914517457749720, is from fidlc's golden output `tools/fidl/fidlc/goldens/anonymous.json.golden` at Fuchsia `51f05104c13`. The old selectors' history: `fuchsia.io/Directory.Open3` (`sdk/fidl/fuchsia.io/directory.fidl`, `openable.fidl`), Close declared in `fuchsia.unknown.Closeable` with `@selector("fuchsia.io/Node.Close")` (`sdk/fidl/fuchsia.unknown/unknown.fidl`), ReadDirents kept from `fuchsia.io1`.

The comparisons taken out of the comments, kept here so that they can still be followed: fsio follows fdio (`sdk/lib/fdio`: `namespace/local-filesystem.cc` for the namespace rules and export, `cleanpath.cc` for path cleaning, `spawn.cc` for `fdio_spawn()` and its CLONE_JOB and CLONE_NAMESPACE flags, `fdio_get_vmo_exec()` for READ, EXECUTE and PRIVATE_CLONE); vfs follows the Rust vfs (`src/storage/lib/vfs/rust`: `directory/connection.rs` `handle_open` for the order of Open's checks, `protocols.rs` for rights and node-kind options, `file/common.rs` and `file/vmo.rs` for GetBackingMemory); the Launcher state follows `src/sys/lib/elf_runner/src/process_launcher.rs` and `src/lib/process_builder`; the router follows component_manager's legacy router (`src/sys/lib/routing/src/legacy_router.rs` at `9e015f591c4^`) with the error naming of `intermediate_router.rs` at `51f05104c13`; the root's built-ins and boot directory follow component_manager's `builtins/src/debug_resource.rs` and `src/bootfs.rs`; the manifest compiler follows `cmc` and `cm_types`. The earlier entries of `component.md`, `fsio.md`, `vfs.md`, `launch.md`, `init.md` and `fuchsia-io.md` in this directory, and the design's "Where ours departs from Fuchsia", give the line numbers.

**Verification**
- Build: `ninja -C out/bringup.x86` from a fresh output directory, so no object of the old names could satisfy a link.
- Host tests, every one `[  PASSED  ]`: ours_io 21, ours_process 21, ours_kernel 5, vfs 42, fsio 34, launch plan 36, process_launcher 15, program 8, component decl 16, route 32, svc 9, wire 12, ours_examples_echo 7, init autorun 5.
- `python3 system/lib/wire/test/ordinals_test.py`: 5 tests OK; before the ordinals were recomputed it failed 11 subtests, one per method, as it should.
- `python3 system/test_component_manifest.py`: 48 tests OK.
- clang-format on the changed lines (`scripts/format_cpp.py`); gn layout (`scripts/format_gn.py`); clang-tidy on the 27 changed sources against `out/bringup.x86`: no finding on a changed line (the findings left are in `bootfs/view.hpp`, gtest's headers and an untouched line of `shell_tests.cpp`).
- boot_check:

```text
dbg        PASS  shell finished at 40.5s · 29 passed, 2 skipped, 0 failed · 0 error line(s) · reached: end
rel        PASS  shell finished at 3.8s · 26 passed, 7 skipped, 0 failed · 2/2 required · 0 error line(s) · reached: end
```

The rel log shows `shell: connect(/svc/ours.process.Launcher) = Ok`, `shell: spawn(routing_checks child) = Ok` and `routing_checks: passed`, so the new names route end to end and every new ordinal is spoken by both sides. (dbg took 40.5 s because clang-tidy ran beside it; an earlier boot of this change, before the comment rewrite, finished at 14.7 s.)

**Deliberately not done**
- The child URL scheme `fuchsia-boot:///#meta/<name>.cm` (D26) still carries Fuchsia's name. It is a value the manifest compiler writes and the root parses, not a protocol name, and the user's decision did not name it; renaming it is a question for the user, recorded in the run's report.
- Comments in files this change did not otherwise touch, such as `system/lib/wire/src/wire.cpp` and `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp`: they belong to the repository-wide sweep the user decided on, which runs as its own change.
- Documents keep their references, as the rule allows: the wire README still says the header is shaped like Zircon's, and the component-framework PLAN keeps the old names as the history of the plan. Earlier change records, `fuchsia-io.md` among them, are append-only and keep the names they were written with.
- The design's own text, other than D20's Decided cell and the open question, is unchanged, as is its table of ordinals: it records what was designed on 2026-10-04 morning, and D20 now says it was revised.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The ordinal of a method is a hash of its selector. If the client were rebuilt with the new names and the root were not, what would the client see on its first Open, and why would it look like an ordinary refusal rather than a version mismatch?
2. The flat bodies of D20 already made the protocols unreadable to a Fuchsia peer. Why does that make the borrowed names a false promise rather than a useful one?
3. A /svc entry name, a manifest capability name and `LAUNCHER_NAME` must all be the same string. Which component would notice first if one of them were missed, and with which status?

## Title: review of the rename: two comments that still named Fuchsia's process_builder, comments left ragged by the rewrite, and the URL scheme question recorded (2026-10-04)

**Files**
- `system/lib/launch/src/process_launcher.cpp` — `add_handles()` and `add_names()` explained themselves by Fuchsia's `process_builder`; both now give the reason in this code's terms; two doc comments the rename left ragged are refilled
- `system/lib/vfs/src/server.cpp`, `system/lib/vfs/test/server_test.cpp`, `system/lib/wire/include/wire/wire.hpp`, `system/lib/svc/include/svc/outgoing.hpp`, `system/service/init/init.cpp`, `system/service/init/test/routing_checks/routing_checks.cpp`, `examples/shell/BUILD.gn`, `system/lib/wire/test/ordinals_test.py` — comment paragraphs whose lines the rewrite cut short in the middle are refilled; no wording changes
- `system/component_manifest.py` — the error for a name over 100 bytes read "is longer than 100 bytes, MAX_NAME_LENGTH" once the foreign constant was cut from it; it now ends at "bytes", as the path errors do
- `docs/repairs/261004/component.md` — item 2: the child URL scheme `fuchsia-boot:` is a question for the user

**Symptom**
None at run time. Two comments in a file the rename touched still offered a Fuchsia function as the reason for this code's behavior, which the comment rule of 2026-10-04 forbids. Several paragraphs had a line of three or four words in the middle where a phrase had been deleted, and one compiler error message ended in a bare constant name.

**Cause**
- The rename's sweep searched for "Fuchsia", "fdio", "fidl" and similar words, but `process_builder` is the name of a Fuchsia library, not a word that says "Fuchsia", so `// All of the message or none of it, as process_builder checks every handle before it adds any.` and `// ..., as process_builder's add_namespace_entries() numbers them.` survived. Both comments used the other system as the reason: a reader who does not know process_builder learns nothing about why ours checks every handle first. The real reasons are local: checking all handles before adding any means a refused AddHandles leaves the state as it was; and an NsDir's argument indexes the names in the order they arrived, counted across every AddNames call, because that is how `add_names()` stores them.
- Deleting a clause from the middle of a wrapped comment leaves the line it was on short, and clang-format only breaks lines that are too long; it never joins a short line with the next. So every such deletion has to be refilled by hand.
- The comment rule also covers what a tool prints, by the same reasoning: an error message that ends in "MAX_NAME_LENGTH" after the library name in front of it was removed reads as a fragment.
- The URL scheme was left unchanged by the rename, correctly, since the user's decision named only the protocol libraries. But the question was recorded only in the run's report, which nobody reads later; an open question belongs in `docs/repairs`, where the next session finds it.

**Change**

```cpp
// before
    // All of the message or none of it, as process_builder checks every
    // handle before it adds any.
// after
    // All of the message or none of it: every handle is checked before any
    // is added, so a refused message leaves the state as it was.

// before
        // The argument of an NsDir is the index of its path among the
        // processargs' names, which lie in the order they came, as
        // process_builder's add_namespace_entries() numbers them.
// after
        // The argument of an NsDir is the index of its path among the
        // processargs' names, which lie in the order they came, counted
        // from 0 across every AddNames call.
```

No code changed: every other hunk is a comment refilled to the file's width, or the one error message.

**Reference**
The two comparisons taken out: Fuchsia's `ProcessBuilder::add_handles()`, which validates every handle before adding any, and `ProcessBuilder::add_namespace_entries()`, which numbers namespace entries in arrival order (`src/lib/process_builder/src/process_builder.rs`).

**Verification**
- `python3 system/lib/wire/test/ordinals_test.py`: 5 tests OK. `python3 system/test_component_manifest.py`: 48 tests OK.
- clang-format clean on the changed lines (`scripts/format_cpp.py --check`), gn layout (`scripts/format_gn.py --check`), `scripts/lint_docs.py` on both documents.
- Host gtests and boot_check: see the commit; the build has to recompile the touched sources, though no generated code changes.

**Deliberately not done**
- The URL scheme itself is not renamed: it is the user's decision (repair item 2 in `docs/repairs/261004/component.md`).
- Comments in files the rename did not touch still name other systems (`system/public/ours/status.hpp:88-90`, the echo protocol header); they belong to the repository-wide sweep.
- The short Chinese comment lines at the top of `kernel/products/image/dev/BUILD.gn` stay: `scripts/format_gn.py` lays them out that way, since gn measures a line in bytes and a CJK character is three of them.

**How it was found**
Review of the rename: `git diff --name-only mvp/integration..HEAD` piped into a grep for the names of Fuchsia's libraries and tools (`process_builder`, `elf_runner`, `cm_types`, `cmc`, `fdio`, `zxio`, `component_manager`) besides the plain words, and a script that lists comment lines in changed hunks that end far short of the column while the next line's first word would fit.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `add_handles()` check every handle before it adds any, and what would a client see if it added the good ones and refused the rest?
2. Why can clang-format not repair a comment whose middle line was shortened by a deletion?
3. Why is an open design question recorded in `docs/repairs` rather than in a run's report?

## Title: the root BUILD.gn's ACPI comment sits above its own label again (2026-10-04)

**Files**
- `BUILD.gn` — in `group("default")`, the two-line comment on `//kernel/lib/acpi:tests` moves from above the benchmarks' comment to directly above its own label, with a blank line between the two entries

**Symptom**
The ACPI comment, then the benchmarks' comment, then `"//benches"` and `"//kernel/lib/acpi:tests($host_toolchain)"` with no blank line between them: read as one block, the ACPI sentence describes `//benches`.

**Cause**
When the benchmarks moved to `//benches` (`28953811`), their entry and comment were inserted between the ACPI comment and the ACPI label. A blank line still separated the two labels until the rename wrote `//benches:benches` as `//benches` and dropped that blank line, which joined both comments into one block over both labels.

**Change**
Only the comment moves; the list keeps its order (`//benches`, then the ACPI tests), so what the build produces does not change.

**Reference**
None, this project's own build file.

**Verification**
`scripts/format_gn.py --check BUILD.gn` clean; boot_check dbg and rel PASS.

**Deliberately not done**
None.

**How it was found**
Review of the rename's `BUILD.gn` hunk.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must a GN list edit here keep its entries' order, and which tool would have reordered them?
2. In a GN list, which entry does a comment belong to?
3. Why does a blank line matter to how a reader groups the comments in a list?

## Title: the protocol libraries' directories take their libraries' names, ours.io, ours.process and ours.resource, and ours.kernel.DebugResource becomes ours.resource.Debug (2026-10-04)

**Files**
- `system/lib/ours-io/` -> `system/lib/ours.io/` — the directory and the library target are `ours.io`, so the label is `//system/lib/ours.io`; the namespace, the include prefix (`<ours_io/ours_io.hpp>`) and the gtest target `test:ours_io` do not change
- `system/lib/ours-process/` -> `system/lib/ours.process/` — the same, label `//system/lib/ours.process`, namespace `ours_process` unchanged
- `system/lib/ours-kernel/` -> `system/lib/ours.resource/` — directory, target, include prefix (`<ours_resource/ours_resource.hpp>`), namespace `ours_resource`, `src/ours_resource.cpp`, `test/ours_resource_test.cpp`, gtest target `test:ours_resource`, `.clang-format`'s first include group; the protocol is `Debug`: `DEBUG_NAME` is `ours.resource.Debug`, `DEBUG_GET`'s selector is `ours.resource/Debug.Get` and its ordinal is recomputed; the codec's names follow the protocol (`DebugGetRequest`, `DebugGetReply`, `encode_debug_get`, `decode_debug_get`, `encode_debug_get_reply`, `decode_debug_get_reply`) and so do the test suites (`OursResourceDebugGet`, `OursResourceDebugGetReply`, `OursResource`)
- `BUILD.gn` — the host test list names `//system/lib/ours.io:tests`, `ours.process:tests` and `ours.resource:tests`
- `system/lib/fsio/BUILD.gn`, `system/lib/launch/BUILD.gn`, `system/lib/svc/BUILD.gn`, `system/lib/vfs/BUILD.gn`, `system/lib/vfs/test/BUILD.gn`, `system/service/init/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn`, `examples/shell/BUILD.gn` — dependency labels
- `kernel/products/image/dev/BUILD.gn` (root.cm), `examples/shell/BUILD.gn` (shell.cm), `system/service/init/test/routing_checks/BUILD.gn` (routing_checks.cm) — the offered and used protocol is `ours.resource.Debug`
- `system/service/init/realm.cpp` — the root's built-in table and its dispatch compare against `ours_resource::DEBUG_NAME`
- `system/service/init/debug_resource_server.cpp`, `debug_resource_server.hpp`, `init.cpp`, `pools.hpp` — the server uses the renamed codec; comments and one log line name `ours.resource.Debug`
- `examples/shell/shell.cpp`, `system/service/init/test/routing_checks/routing_checks.cpp` — the `/svc/ours.resource.Debug` path, the codec calls, and the method's name in their messages (`Debug.Get`)
- `system/lib/component/include/component/route.hpp`, `system/lib/component/test/route_test.cpp` — the built-in's name in a comment and in the test's built-in table
- `system/lib/wire/test/ordinals_test.py` — the selector the scan must find is `ours.resource/Debug.Get`
- the READMEs of the three libraries, of fsio, of vfs and of wire — paths, and the sentence on why the directory and the C++ name differ
- `docs/design/261004/component-framework.md` — the library-name line in the open questions, and D20's Decided cell
- `.agents/tasks/microkernel-interrupts/PLAN.md`, `DESIGN.md` — the planned built-ins are `ours.resource.Irq` and `ours.resource.Ioport`, in `system/lib/ours.resource`
- `docs/repairs/261004/init.md`, `docs/repairs/261004/component.md` — the method and library names their items mention

**Symptom**
None; this carries out two decisions the user made on 2026-10-04 evening, after the rename to ours's own names: the libraries' directories use a dot, as `system/lib/boost.container` does, and the kernel-resource library is `ours.resource`, not `ours.kernel`.

**Cause**
- **Why a directory with a dot.** After the first rename one name had three spellings: `ours.io` for the protocol library (in every selector and /svc name), `ours-io` for the directory and the GN label, and `ours_io` for the C++ namespace and include prefix. Only the last one is forced: a C++ identifier cannot hold a dot or a hyphen. A directory name and a GN label can hold a dot, as `system/lib/boost.container` shows, so the hyphen was a third spelling with no reason behind it. With the directory named exactly as the library, the selector `ours.io/File.Read` and the label `//system/lib/ours.io` say the same thing, and a reader goes from a message's selector to the code that declares it without translating.
- **Why the library target is renamed with the directory.** A GN label with no `:name`, such as `//system/lib/ours.io`, means the target named after the last path component: `//system/lib/ours.io:ours.io`. Had the `source_set` kept its old name, `ours-io`, every dependant's short label would point at a target that does not exist, and `gn gen` would fail on the first one. The test executables (`test:ours_io` and so on) are named separately and do not move; only the library's own target follows its directory.
- **Why `ours.resource`, and why its protocol is `Debug`.** The library holds the protocols through which the root hands out kernel resource objects: the Debug resource today, and the IRQ and ioport roots in step 13 of the interrupt plan. `kernel` said where those objects live, which is true of every handle a component holds; `resource` says what kind of object the library hands out, a resource. Once the library is named for that, the word in each protocol name repeats it: `ours.resource.DebugResource` would say "resource" twice. So the protocol is named by the kind of resource alone, `ours.resource.Debug`, and the planned ones become `ours.resource.Irq` and `ours.resource.Ioport`.
- **Why the ordinal changes, and why that is safe.** An ordinal is the first 8 bytes of SHA-256 of the selector, `<library>/<Protocol>.<Method>`, little-endian, top bit cleared; both the library and the protocol part changed, so `ours.kernel/DebugResource.Get` (`0x01f00d3fa678897f`) becomes `ours.resource/Debug.Get` (`0x226cd5bb18218fda`). A client and a server that disagree on it would not fail loudly: the server treats the message as an unknown method and closes the connection, which the client sees as PeerClosed, like any other refusal. The switch is safe for the same two reasons as the first rename: the clients (the shell, routing_checks) and the server (the root) are built in one build, and nothing types an ordinal unchecked, since `ordinals_test.py` recomputes every declared ordinal from its selector and routing_checks calls Debug.Get on the target at every boot.
- **Why the /svc name changes everywhere in one commit.** `ours.resource.Debug` is one string in five places: root.cm's offer, shell.cm's and routing_checks.cm's uses, the root's built-in table (`DEBUG_NAME`), and the file name each client opens under `/svc`. Routing compares capability names byte for byte, so a place that was missed does not fail the build: the route answers NotFound at boot. routing_checks' `check_debug_resource()` reports that as a failure, so the `routing_checks: passed` line boot_check requires on rel never comes.
- **Why the codec's C++ names changed and the root's server's did not.** The codec's names are built from the protocol and the method: `DEBUG_RESOURCE_GET` was DebugResource.Get, `DebugResourceGetReply` its reply. After the rename they would name a protocol that no longer exists, which misleads a reader the way a stale name always does, so they follow it: `DEBUG_GET`, `DebugGetReply`. `DebugResourceServer`, `debug_resource_server.cpp`, `MAX_DEBUG_RESOURCE_CONNECTIONS` and the shell's `debug_resource` variables name something else, the Debug resource itself, the object the server hands out and the shell holds. That is still exactly what they are; renaming the server `DebugServer` would make it read as a server for debugging.

**Change**
The codec header, before and after:

```cpp
// before: system/lib/ours-kernel/include/ours_kernel/ours_kernel.hpp
namespace ours_kernel {
inline constexpr auto DEBUG_RESOURCE_NAME = std::string_view{"ours.kernel.DebugResource"};

inline constexpr auto DEBUG_RESOURCE_GET = wire::Method{
    .selector = "ours.kernel/DebugResource.Get",
    .ordinal = 0x01f0'0d3f'a678'897f,
    .kind = wire::MethodKind::TwoWay,
};

// after: system/lib/ours.resource/include/ours_resource/ours_resource.hpp
namespace ours_resource {
inline constexpr auto DEBUG_NAME = std::string_view{"ours.resource.Debug"};

inline constexpr auto DEBUG_GET = wire::Method{
    .selector = "ours.resource/Debug.Get",
    .ordinal = 0x226c'd5bb'1821'8fda,
    .kind = wire::MethodKind::TwoWay,
};
```

A library target, before and after:

```text
# before: system/lib/ours-io/BUILD.gn
source_set("ours-io") {        # //system/lib/ours-io
# after: system/lib/ours.io/BUILD.gn
source_set("ours.io") {        # //system/lib/ours.io
```

Every name the move touches:

| What | Before | After |
|---|---|---|
| ours.io directory and label | `system/lib/ours-io`, `//system/lib/ours-io` | `system/lib/ours.io`, `//system/lib/ours.io` |
| ours.process directory and label | `system/lib/ours-process`, `//system/lib/ours-process` | `system/lib/ours.process`, `//system/lib/ours.process` |
| resource library directory and label | `system/lib/ours-kernel`, `//system/lib/ours-kernel` | `system/lib/ours.resource`, `//system/lib/ours.resource` |
| its namespace and include prefix | `ours_kernel`, `<ours_kernel/ours_kernel.hpp>` | `ours_resource`, `<ours_resource/ours_resource.hpp>` |
| its gtest | `//system/lib/ours-kernel/test:ours_kernel` | `//system/lib/ours.resource/test:ours_resource` |
| the protocol, in /svc and the manifests | `ours.kernel.DebugResource` | `ours.resource.Debug` |
| the method's selector and ordinal | `ours.kernel/DebugResource.Get` `0x01f00d3fa678897f` | `ours.resource/Debug.Get` `0x226cd5bb18218fda` |
| the planned built-ins | `ours.kernel.IrqResource`, `ours.kernel.IoportResource` | `ours.resource.Irq`, `ours.resource.Ioport` |

The new ordinal was computed with the rule `ordinals_test.py` checks, by script, not typed. The GN dependency lists keep each entry where it stood, so `//system/lib/ours.resource` sits where `ours-kernel` did, between `ours.io` and `ours.process`.

**Reference**
None for the directory layout: it is this project's own, and `system/lib/boost.container` and `boost.intrusive` are the precedent for a dot in a library directory. The ordinal rule is fidlc's method hasher, `Sha256MethodHasher` in Fuchsia's `tools/fidl/fidlc/src/compiler.cc`, as in the first entry of this file. The protocols the root hands resources through follow component_manager's built-ins for `fuchsia.kernel.DebugResource`, `fuchsia.kernel.IrqResource` and `fuchsia.kernel.IoportResource` (`sdk/fidl/fuchsia.kernel/`, `src/sys/component_manager/lib/builtins/src/debug_resource.rs`, `irq_resource.rs`); ours groups them in a library named for the kind of object they give and names each protocol by its resource.

**Verification**
- Build: `boot_check.py` configured and built a fresh `out/bringup.x86` in a new worktree, so no object of the old names could satisfy a link.
- Host tests, every one `[  PASSED  ]`: ours_io 21, ours_process 21, ours_resource 5, vfs 42, fsio 34, launch plan 36, process_launcher 15, program 8, component decl 16, route 32, svc 9, wire 12, ours_examples_echo 7, init autorun 5.
- `python3 system/lib/wire/test/ordinals_test.py`: 5 tests OK, its scan finding `ours.resource/Debug.Get` in `system/lib/ours.resource/include/ours_resource/ours_resource.hpp`. `python3 system/test_component_manifest.py`: 48 tests OK.
- clang-format on the changed lines (`scripts/format_cpp.py`), gn layout (`scripts/format_gn.py`), `scripts/lint_docs.py` on the changed documents; clang-tidy against `out/bringup.x86` on the codec, its test, the shell, routing_checks, realm.cpp, the Debug server, init.cpp and route_test.cpp: no finding outside googletest's headers and the untouched `bootfs/view.hpp`.
- boot_check:

```text
dbg        PASS  shell finished at 31.6s · 29 passed, 2 skipped, 0 failed · 0 error line(s) · reached: end
rel        PASS  shell finished at 3.8s · 26 passed, 7 skipped, 0 failed · 2/2 required · 0 error line(s) · reached: end
```

The rel log has `routing_checks: passed`, so Debug.Get was routed through `/svc/ours.resource.Debug` and answered with the new ordinal on both sides, and no `shell: Debug.Get FAILED` line, so the shell got its resource the same way.

**Deliberately not done**
- The namespaces and include prefixes `ours_io` and `ours_process` stay: a C++ identifier cannot hold a dot, and they already are the library's name with its dot made an underscore.
- The root's `DebugResourceServer`, `DebugResourceConnection`, `debug_resource_server.cpp`, `MAX_DEBUG_RESOURCE_CONNECTIONS`, `BuiltinServers::debug_resource`, and the shell's `debug_resource` functions and variables keep their names: they name the Debug resource, not the protocol (see Cause).
- The GN dependency lists are not re-sorted: their order sets the include path and the link line, and a rename should not change what the build produces.
- Earlier change records, this file's first entry among them, keep the names they were written with. The component-framework PLAN keeps its history, and the design's own text from 2026-10-04 morning (the built-ins row, the ordinals table, the D19 and D29 discussion) keeps the names it was designed with; only the library-name line and D20's Decided cell, which record the user's later decisions, say `ours.resource`.
- `ours_kernel_has_code_patches` in `build/config/BUILDCONFIG.gn` is a build argument about the kernel image, unrelated to the protocol library, and is untouched.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The label `//system/lib/ours.io` names which target, and what would `gn gen` say if the directory moved but `source_set("ours-io")` kept its name?
2. Why is `ours.resource.Debug` a better protocol name than `ours.resource.DebugResource`, and why does `DebugResourceServer` keep "Resource" in its name?
3. If root.cm were updated to `ours.resource.Debug` but routing_checks.cm still used `ours.kernel.DebugResource`, where would the boot first say so, and with which status?
