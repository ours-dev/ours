# component-framework: the steps from today's init and standalone launcher to a root that starts components from manifests

**Date**: 2026-10-04
**Status**: Proposed

## Request
On 2026-10-04 the user approved three things:
- research the minimal part of Fuchsia's component manifests: programs and children; how directory and protocol capabilities are declared, offered and exposed; how a manifest is compiled; how /svc is assembled;
- rewrite DESIGN.md with the objects, the events, the mapping to Fuchsia, what is cut, and the open decisions;
- list the later steps here.

Each step below is introduced to the user, with its background, before work on it starts, and waits for approval. The scope ends at "basically usable":
- the root starts manifest-declared components, serves /boot and /svc, and hosts the Launcher;
- the shell spawns commands through the Launcher;
- one sibling route works (step 17, which D3 (c) would drop).

Performance work is out of scope; the choices that cap it are DESIGN decisions D11, D13-D18 and D27.

## Decisions
The S and D decisions live in DESIGN.md's table only and are decided there; the steps below follow their recommendations, and a different choice changes the steps it names. This table holds the plan's own decisions.

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| P1. Where `vma_unmap` and `vmo_get_size` land | (a) steps 9 and 10 of this plan; (b) a kernel task first | (a): they exist only because the Launcher takes a VMO. Under D14 (c), step 9 goes and step 11 changes | | |
| P2. How the switch is cut | (a) four commits: the root with a test component while the shell still uses the old launcher (13), /svc and the Launcher (14), the shell (15), the deletions (16); (b) one switch commit | (a), so that each failure points at one piece | | |
| P3. Functions the user writes by hand (CLAUDE.md, one per milestone, recorded in `docs/authored.md`) | (a) `route()` for the libraries milestone (step 8) and the Loop's dispatch for the root milestone (step 12); (b) CleanPath (step 5) instead of `route()`; (c) none | (a): `route()` is the walk this design is about, and the Loop's dispatch is where I3 and I4 are kept. The AI gives the spec and a failing test, the user writes it under a time limit, and the AI reviews it as a strict interviewer | | |

## Steps
Every step:
- lands as one commit, made with `git commit -- <paths>`;
- builds both images and boots dev and dev.dbg_1 under `python3 .agents/skills/ours-verify/scripts/boot_check.py`;
- runs `cpp-write-and-verify` on the C++ it touches;
- writes its change record in `docs/changes/<YYMMDD>/<module>.md` in the same commit, with the module the step names;
- scaffolds a new C++ library with `python3 -m copier copy tools/templates/cpp_library system/lib/<name>`, and adds its host tests to the host test list in the root `BUILD.gn`.

### 1. Record the model
- What:
  - Move the accepted DESIGN.md to `docs/design/<YYMMDD>/component-framework.md`, as `.agents/tasks/TEMPLATE.md` asks of an accepted design.
  - Rewrite the "userboot and init" and "Filesystems" sections of `.agents/skills/ours-kernel-design/references/boot.md` (`:54-73`) for the root model.
  - Mark `.agents/tasks/vfs/DESIGN.md` and `.agents/tasks/launcher-service/DESIGN.md` as superseded by this task.
  - Add English notes saying what is superseded to FS §4.3 (the service list) and §4.5 (the revision of 2026-09-26, FS:372-376).
- Why now: every later step is reviewed against that skill, and it still mandates a Linux-style VFS.
- Verified: `python3 scripts/lint_docs.py --changed <paths>`; `ours-verify` output unchanged.
- Module: `component-framework`.
- Commit: `docs(design): record the component-framework model`.

### 2. The page rule in bootfs::View
- What: View refuses an entry whose data offset is not page-aligned, and two entries whose page-rounded ranges overlap without being identical. An empty range overlaps nothing, since the Builder gives a zero-size file the next file's offset (`system/lib/bootfs/include/bootfs/builder.hpp:61-65`). Identical ranges stay allowed, as BootfsSvc shares one VMO between them (`cm/bootfs.rs:262-288`) and refuses only an unaligned offset (`:88-95`). A new host test in `system/lib/bootfs/test` covers each refusal, a zero-size entry, identical ranges and a Builder round trip.
- Why now: each `bin/` file becomes its own page-rounded executable slice (D15). If a file's last page held a neighbour's bytes, those bytes would become executable. Today's images are safe only because the Builder pads every file.
- Verified: the new host test; both images boot, since every View user parses with the rule: userboot, init, the dbg_1 shell, phys's kernel package loader (`kernel/phys/kernel_package_loader.cpp`) and obi's tree command (`kernel/lib/obi/cli/tree.cpp`).
- Module: `bootfs`.
- Commit: `feat(bootfs): refuse file data that is unaligned or overlaps another`.

### 3. Framing and the fuchsia.io codec
- What:
  - `system/lib/wire`: the 16-byte header with the direction byte, the epitaph, handle placeholders, and the 24-byte error reply (FS:57-79).
  - `system/lib/fuchsia-io`: the six fuchsia.io messages and the epitaph of DESIGN's "The protocol subset", each ordinal declared beside its exact selector string.
  - Host tests: a round trip for every message, every malformed form refused.
  - `system/lib/wire/test/ordinals_test.py`: reads every `{selector, ordinal}` pair from the codec headers and recomputes it with hashlib, since the tree has no SHA-256 in C++.
- Why now: everything after this speaks it; it needs no kernel.
- Verified: the host tests; the Python test prints OK; boot unchanged.
- Module: `fuchsia-io`.
- Commit: `feat(fuchsia-io): hand-written codec for the fuchsia.io subset`.

### 4. The vfs server library
- What: `system/lib/vfs` with the three node kinds (pseudo-directory, VmoFile, service), the connection table, the Open checks in the Rust vfs's order (CONNECT for PROTOCOL_SERVICE included), and the handlers. Its core takes in a message and returns a reply plus new bindings, without touching a channel, so host tests can drive it.
- Why now: BootServer and /svc are built on it.
- Verified: host tests for the check order, rights shrinking, the EXECUTE gate, ReadDirents with "." and BufferTooSmall, OutOfRange, GetBackingMemory's rights with and without PRIVATE_CLONE, Clone and Close, and a malformed message closing the connection.
- Module: `vfs`.
- Commit: `feat(vfs): fuchsia.io server library`.

### 5. fsio
- What: `system/lib/fsio` with:
  - the namespace table, with fdio's bind rules: absolute paths, no overlap, no shadowing;
  - CleanPath;
  - open, read, read_dirents, get_backing_memory and connect;
  - install from the NsDir handles.

  Export and spawn wait for step 14, since spawn speaks the Launcher codec of step 6 and is first used there.
- Why now: routing_checks, from step 13, is its first user.
- Verified: host tests for CleanPath, bind and unbind, and lookup; the calls that do I/O are tested on the target from step 13.
- Module: `fsio`.
- Commit: `feat(fsio): client namespace and file calls`.

### 6. Launcher and DebugResource codecs, and the Launcher's connection state
- What:
  - `system/lib/fuchsia-process`: AddArgs, AddHandles, AddNames and Launch.
  - `system/lib/fuchsia-kernel`: DebugResource.Get.
  - In `system/lib/launch`: the per-connection state, the counterpart of `ProcessLauncherState`. It holds an arena and up to 62 handles; an Add* that does not fit sets an overflow mark, and Launch answers InvArgs, as Fuchsia answers INVALID_ARGS (`src/lib/process_builder/src/process_args.rs:24-28`). It refuses Fuchsia's reserved handle types, ProcSelf, ThreadSelf, VmaRoot, VmaLoaded, VmoStack and VmoExecutable, and an NsDir outside AddNames (`process_builder.rs:319-337`). It resets after Launch. A malformed message closes the connection with no epitaph (`process_launcher.rs:92`; FS:524).
  - The old name-based protocol stays until step 16.
- Why now: the Launcher server (step 14) and fsio's spawn speak it, and it needs no kernel.
- Verified: host tests, including the 62/63 handle boundary and too many bytes, both answered InvArgs at Launch.
- Module: `launch`.
- Commit: `feat(launch): fuchsia.process.Launcher codec and connection state`.

### 7. Manifest template, compiler and decoder
- What:
  - `system/component_manifest.gni`, which expands into five targets: source JSON, program name, children, compile, group. It adds the user toolchain to program and child labels and refuses a label that names one, as `user_bootfs` does (`kernel/products/image/dev/BUILD.gn:102-111`), so manifest targets resolve their programs in the user toolchain from any BUILD.gn.
  - `system/component_manifest.py` (Python 3.9, standard library only) and its unit test `system/test_component_manifest.py`. It writes child URLs as D26 says and fixes the name length (DESIGN's open question).
  - `system/lib/component` with the decl format and its decoder, which checks structure only.
  - A gtest that decodes `.cm` files the template built from fixture targets, one of them declared in a BUILD.gn loaded in the default toolchain with a child in the user toolchain.
- Why now: the router and the root read what it writes.
- Verified: `python3 system/test_component_manifest.py` prints OK; the decoder gtest passes; a dangling child label fails gn gen (a manual check, recorded in RESULT.md).
- Module: `component`.
- Commit: `feat(component): manifest template, compiler and decoder`.

### 8. The routing walk
- What: `route()` in `system/lib/component`, written by the user by hand (P3) from DESIGN's spec ("The routing walk") against failing host tests the AI writes first; the AI then reviews it as a strict interviewer, and `docs/authored.md` gets a row.
- Why now: the root needs it in step 13, and it is pure, so it is tested on the host.
- Verified: host tests over the compiled fixtures:
  - a built-in source;
  - a sibling reached through expose;
  - an offer that narrows rights, answering AccessDenied;
  - subdir joining;
  - NotFound naming the verb and the moniker;
  - a provider that has no program.
- Module: `component`.
- Commit: `feat(component): route a use to its source`.

### 9. vma_unmap
- What: the system call, Zircon's `zx_vmar_unmap`, over the kernel's `VmArea::unmap` (`.agents/tasks/vm-unmap/RESULT.md:1-20`). Touches `system/public/ours/syscalls.gni` and `kernel/lib/syscall/src/vma.cpp`.
- Why now: under D14 (b), `start()` maps a client's VMO into the root and has to let go of it.
- Verified: a new shell check on both images: unmap a mapping, map again at the same place, refuse ranges outside the VMA, refuse unmapping all or part of the sealed vDSO, and refuse a range that straddles a sealed mapping. The last two were checked only in the kernel until now, "since no program can unmap or protect yet" (`examples/shell/shell_tests.cpp:3625-3628`).
- Module: `syscall`.
- Commit: `feat(syscall): vma_unmap`.

### 10. vmo_get_size
- What: the system call, Zircon's `zx_vmo_get_size`. Touches `syscalls.gni`, `kernel/lib/syscall/src/vmo.cpp`, and `kernel/object/include/ours/object/vmo_dispatcher.hpp`, which gains `VmoDispatcher::size()`: a syscall calls dispatcher methods only and never reaches `mm::` (`.agents/skills/ours-kernel-design/references/layering.md:8-12`).
- Why now: `Plan::build` needs the file's size (`system/lib/launch/include/launch/plan.hpp:123-129`), and a VMO is all the Launcher receives.
- Verified: a new shell check on the BootFS VMO, on a slice, whose size is page-rounded (`syscalls.gni:848`), and on a created VMO.
- Module: `syscall`.
- Commit: `feat(syscall): vmo_get_size`.

### 11. start() maps the program with Execute and reads it in place
- What:
  - `launch::Request` loses `file_bytes` and gains `vma`, the caller's own root VMA, not consumed (`launch.hpp:18-56` has neither). `start()` maps the file VMO with PermRead and PermExecute into that VMA, which fails without Execute before anything is parsed (D14 b, I6), plans and binds from that mapping at the size `vmo_get_size` gives (page-rounded, which Plan::build's bounds checks accept, since the rounding is zero padding), then unmaps it.
  - `BootfsLauncher` keeps the VMA `make_bootfs_launcher` is given (`system/lib/launch/include/launch/bootfs.hpp:36-51`), since ort hands VmaRoot out once. userboot, init, the standalone launcher and the dbg_1 shell adapt.
  - In `docs/repairs/261002/launch.md`, item 2 is removed, and item 4's premise that the launcher maps nothing into itself and needs no `vma_unmap` (`:78`) is rewritten.
- Why now: the Launcher in the root has nothing but a VMO.
- Verified: the launch host tests; every launch on both images goes through the new path; a new dbg_1 shell check that `start()` refuses a VMO without Execute with AccessDenied.
- Module: `launch`.
- Commit: `refactor(launch): start() reads the program from its VMO`.

### 12. The Loop
- What: `system/lib/loop`, a port plus a fixed slot table with generations (FS:308-315). Its dispatch, `run()`, is written by the user by hand (P3) from the spec in FS:310-315 against a failing test.
- Why now: the root's first wait on many handles is step 13, and `svc::Outgoing` (step 17) runs on it too.
- Verified: a new shell check (`check_loop`) on both images: two channels, re-arming, a stale generation dropped, a callback closing its own slot.
- Module: `loop`.
- Commit: `feat(loop): one port and a fixed slot table`.

### 13. The root serves /boot and starts its first component
- What:
  - init ingests the BootFS into BootServer (a read-only slice per file, made executable under `bin/`), keeps VMEX, and starts the standalone launcher with `start()` on the file from its own tree instead of `BootfsLauncher`.
  - Its three blocking waits become Loop slots: the pong (`init.cpp:441`), the shell's PeerClosed (`:463`) and TERMINATED on the shell (`report_exit`, `:306`).
  - It resolves `meta/root.cm`, routes directory uses, runs the ELF runner on `launch::start()`, and supervises TERMINATED. A root that cannot start logs one line and halts (D28).
  - Its pools are the ones D27 settles, with static_asserts against PORT_MAX_OBSERVERS and the handle arithmetic of DESIGN's "Handle charges".
  - dev's root manifest gets one eager child, `routing_checks`, which uses `boot` in four ways: rx* at `/boot`; an r* copy renamed with `as`; the `bin` subdir; and one use whose route narrows rights the wrong way.
  - routing_checks checks Read, ReadDirents, GetBackingMemory with and without EXECUTE, and the AccessDenied entry. It then prints `routing_checks: passed` and exits 0.
  - init still starts the standalone launcher and the shell as before.
  - `boot_check.py` gains required lines per image: the dev image (tag `rel`, `boot_check.py:34-41`) requires `routing_checks: passed`, and the reader keeps going until both the end marker and every required line have appeared or the timeout runs out, since the line may come after `--- end ---` (`:68`, `:163-166`).
  - `docs/repairs/<YYMMDD>/init.md` records: one thread's head-of-line stall (D16); the root handle quota (D18); dropped writes on full channels (I1); no job per component and nothing that can stop a component (D6); the root's own death going unnoticed; the root never running under DEBUG_ASSERT (D22 a).
- Why now: every library this needs is in place, and the shell is untouched, so a failure points at the root.
- Verified: `ours-verify`. The dev log shows `init: /boot has <n> files, <m> executable`, `init: started routing_checks`, `routing_checks: passed`, `init: routing_checks exited with 0`. dbg_1 is unchanged.
- Module: `init`.
- Commit: `feat(init): serve /boot and start components from meta/root.cm`.

### 14. /svc and the built-in protocols
- What:
  - The root serves each component's /svc, with the built-ins `fuchsia.process.Launcher` and `fuchsia.kernel.DebugResource`.
  - fsio gains export by Clone and spawn (moved from step 5).
  - routing_checks spawns itself with the argument `child` through fsio's spawn and expects exit code 0.
  - It also checks Launch's refusals: a VMO without Execute (AccessDenied); a missing executable; a reserved handle type and too many handles (InvArgs); a malformed message, which closes the connection with no epitaph.
  - It checks that DebugResource.Get works, and that a protocol use nobody offers answers NotFound.
  - `docs/repairs/<YYMMDD>/init.md` gains the route re-walk on every Open (D11) and the Launcher's fixed per-connection state (D13).
- Why now: /boot works (step 13); /svc is the other half of a namespace, and the shell needs the Launcher in step 15.
- Verified: `ours-verify` with the new routing_checks lines.
- Module: `init`.
- Commit: `feat(init): /svc, the Launcher and DebugResource`.

### 15. The shell becomes a component
- What:
  - `examples/shell` gets `shell.cm`, and root.cm gains the eager child `shell` with its offers.
  - init stops starting the standalone launcher and the shell itself, and drops the ping.
  - The dev BootFS no longer packages `bin/launcher` (`kernel/products/image/dev/BUILD.gn:38-40`).
  - The shell installs fsio. `ls` uses ReadDirents, a command runs `/boot/bin/<cmd>` through fsio's spawn, and the Debug resource comes from DebugResource (D29).
  - The shell tells the images apart by what it is handed, an NsDir "/boot" on dev against VmoBootfs and VMEX on dbg_1, instead of by User0 being absent. This changes the library-launcher gate (`examples/shell/shell.cpp:549-551`) and the probes' gate (`shell_tests.cpp:3663-3665`) together; without it, dev would take the dbg_1 branch and print "launcher checks FAILED" (`shell.cpp:203-207`).
  - Shell tests: `check_launcher_service` (`shell_tests.cpp:1215`) is rewritten against fuchsia.process.Launcher and moved before `check_handle_close` in `run()`, which now closes the job on both images (`:3671`, `:3677`). The checks that need a raw BootFS (`:3681-3687`) skip on dev. A new `check_spawn` spawns `/boot/bin/routing_checks child`, named through the shell's `user_program_names` list, and expects 0, so boot_check covers fsio's spawn.
  - `docs/repairs/261002/launch.md` item 3 is rewritten: the shell's file VMO is no longer a slice of an executable BootFS (`:52`).
- Why now: every root piece was checked by routing_checks first (steps 13 and 14).
- Verified: `ours-verify`. On the dev serial console, `ls /boot/bin` lists the programs and `routing_checks child` exits 0. Run without `child`, routing_checks would fail: started by the shell it gets only clones of the shell's namespace, not its manifest's entries.
- Module: `shell`.
- Commit: `feat(shell): run as a component of the root`.

### 16. Remove the standalone launcher and launch-by-name
- What: delete
  - `system/service/launcher`;
  - `launch/protocol.hpp`, `src/protocol.cpp`, `launch/client.hpp`, `src/client.cpp` and `test/protocol_test.cpp`, with their targets in `system/lib/launch/BUILD.gn` (`:17-28`, `:42`, `:47`, `:62`, `:66`, `:79`) and `system/lib/launch/test/BUILD.gn` (`:29-40`);
  - `system/service/init/init_startup.gni`, `include/init/startup.hpp` and `test/startup_test.cpp`, with the `startup` interface (`system/service/init/BUILD.gn:25-39`) and its test executable (`system/service/init/test/BUILD.gn:18-28`);
  - the `init_startup` parameter of `user_bootfs()` (`kernel/user_bootfs.gni:35-54`), and dev's `init_startup("shell")`, `init_startup = true` and `":shell"` dependency (`kernel/products/image/dev/BUILD.gn:54-67`, `:94`, `:98`);
  - the startup test's entry in the root `BUILD.gn` (`:156-158`) only; `//system/lib/launch:tests` (`:149`) and the autorun test (`:154`) stay.

  init's `programs` target stays, now listing init itself, because it is where `user_programs::DIRECTORY` comes from and the template refuses an empty list (`system/user_program_names.gni:39-40`). Stale comments are updated: `system/user_executable.gni:8`, `launch/bootfs.hpp:17-21` and `examples/shell/BUILD.gn:24-28`. `BootfsLauncher` stays for userboot and the dbg_1 shell.
- Why now: step 15 left all of this unused, and keeping it separate keeps step 15 readable.
- Verified: `ours-verify`; `gn gen` reports no dangling label.
- Module: `launch`.
- Commit: `refactor(launch): remove the standalone launcher and launch-by-name`.

### 17. A sibling provider
- What:
  - `system/lib/svc`: `svc::Outgoing` on the DirectoryRequest handle, running on the Loop.
  - `examples/echo`, which exposes an echo protocol and exits when asked to; it joins the `examples` group (`examples/BUILD.gn:16-19`).
  - root.cm offers that protocol from `#echo` to `#routing_checks`, renamed with `as`. routing_checks calls it, asks echo to exit, and expects its next connect to answer NotFound (D23).
- Why now: until this step, routes through `expose` and forwarding into a provider's outgoing directory were tested only on the host. Under D3 (c) this step waits until a real component provides something.
- Verified: `ours-verify`, with routing_checks' echo checks and `init: echo exited with 0`.
- Module: `svc`.
- Commit: `feat(svc): outgoing directory, and an echo provider`.

## Files
Created:
- `docs/design/<YYMMDD>/component-framework.md`: the accepted design (step 1).
- `system/lib/bootfs/test/`: View host tests (step 2).
- `system/lib/wire/`, `system/lib/fuchsia-io/`, `system/lib/wire/test/ordinals_test.py`: framing, the fuchsia.io codec and the ordinal check (step 3).
- `system/lib/vfs/`: the server library (step 4).
- `system/lib/fsio/`: the client library (steps 5 and 14).
- `system/lib/fuchsia-process/`, `system/lib/fuchsia-kernel/`: the Launcher and DebugResource codecs (step 6).
- `system/lib/launch/include/launch/process_launcher.hpp`, `src/process_launcher.cpp`, `test/process_launcher_test.cpp`: the Launcher's connection state (step 6).
- `system/component_manifest.gni`, `system/component_manifest.py`, `system/test_component_manifest.py` (step 7).
- `system/lib/component/`: the decl format, decoder, router, tests and fixtures (steps 7 and 8).
- `system/lib/loop/` (step 12).
- `system/service/init/`: new sources for BootServer, instances, the resolver and runner, the /svc server and the Launcher server (steps 13 and 14).
- `system/service/init/test/routing_checks/`: the test component and its manifest (step 13).
- `docs/repairs/<YYMMDD>/init.md`: the root's repair items (steps 13 and 14).
- `system/lib/svc/`, `examples/echo/` (step 17).
- `docs/changes/<YYMMDD>/<module>.md`: one record per step, modules `component-framework`, `bootfs`, `fuchsia-io`, `vfs`, `fsio`, `launch`, `component`, `syscall`, `loop`, `init`, `shell`, `svc`.

Changed:
- `.agents/skills/ours-kernel-design/references/boot.md`, `.agents/tasks/vfs/DESIGN.md`, `.agents/tasks/launcher-service/DESIGN.md`, `docs/design/260924/userspace-filesystem.md` (step 1).
- `system/lib/bootfs/include/bootfs/view.hpp`, `system/lib/bootfs/BUILD.gn` (step 2).
- `BUILD.gn` at the root: host test entries added (steps 2-8) and the startup test removed (step 16).
- `docs/authored.md`: `route()` and the Loop's dispatch (steps 8 and 12).
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/vma.cpp`, `kernel/lib/syscall/src/vmo.cpp`, `kernel/object/include/ours/object/vmo_dispatcher.hpp` (steps 9 and 10).
- `system/lib/launch/include/launch/launch.hpp`, `src/launch.cpp`, `include/launch/bootfs.hpp`, `src/bootfs.cpp`, `README.md` (step 11).
- `kernel/lib/userabi/userboot/userboot.cpp` (step 11).
- `docs/repairs/261002/launch.md`: item 2 removed and item 4 rewritten (step 11), item 3 rewritten (step 15).
- `examples/shell/shell.cpp`, `shell_launcher.hpp`, `shell_tests.cpp`, `shell_tests.hpp`, `BUILD.gn` (steps 9-12, 15, 16).
- `system/service/init/init.cpp`, `system/service/init/BUILD.gn` (steps 11, 13-16).
- `system/service/init/test/BUILD.gn` (step 16).
- `system/lib/launch/BUILD.gn`, `system/lib/launch/test/BUILD.gn` (step 16).
- `system/service/launcher/launcher.cpp` (step 11 only).
- `kernel/products/image/dev/BUILD.gn` (steps 13, 15-17).
- `kernel/user_bootfs.gni`, `system/user_executable.gni` (step 16).
- `examples/BUILD.gn` (step 17).
- `.agents/skills/ours-verify/scripts/boot_check.py`: required lines per image, waited for (step 13).

Deleted (step 16):
- `system/service/launcher/BUILD.gn`, `system/service/launcher/launcher.cpp`.
- `system/lib/launch/include/launch/protocol.hpp`, `src/protocol.cpp`, `include/launch/client.hpp`, `src/client.cpp`, `test/protocol_test.cpp`.
- `system/service/init/init_startup.gni`, `include/init/startup.hpp`, `test/startup_test.cpp`.

## Test plan
- **Host gtests** (the host test list in the root `BUILD.gn`): bootfs, wire, fuchsia-io, vfs, fsio, the Launcher codec and state, the component decoder and the router. Each ends with `[  PASSED  ]`.
- **Python:** `python3 system/test_component_manifest.py` and `python3 system/lib/wire/test/ordinals_test.py` each end with `OK`.
- **Boot:** `python3 .agents/skills/ours-verify/scripts/boot_check.py` prints PASS for both images. After step 17, dev's log starts with these lines, in this order (exact wording fixed in the steps):

```text
init: started with the root job
init: /boot has <n> files, <m> executable
init: started routing_checks
init: started echo
init: started shell
```

The `init: started` lines follow root.cm's child order, because the root starts its eager children one after another before it serves anything. Every other line comes from another process or from the Loop, in no fixed order: `routing_checks: passed`, `init: routing_checks exited with 0`, `init: echo exited with 0`, the shell's checks, and `--- end ---`. boot_check does not check their order; it waits for `routing_checks: passed`.
- **dev.dbg_1:** its log is unchanged from today, apart from the new shell checks for `vma_unmap`, `vmo_get_size`, the Loop, and `start()` refusing a VMO without Execute.
- **Shell checks that change:**
  - `check_launcher_service`: the checks for an unknown name, an 8-byte malformed header, Connect and an echoed reply are replaced by Launcher checks: no executable, a VMO without Execute, a reserved handle type, too many handles answered InvArgs at Launch, and a malformed message closing the connection. It runs before `check_handle_close`.
  - The checks that need a raw BootFS skip on dev.
  - `check_handle_close` closes the job on both images.
  - New: `check_vma_unmap`, `check_vmo_get_size`, `check_loop`, and `check_spawn` on dev.
  - `run_probes` is unchanged, still on dbg_1 only, now gated on the BootFS and VMEX.
- **Shell commands that change, on dev only (D29):**
  - `ls` lists a namespace directory through ReadDirents, `/boot` by default.
  - `x` runs `/boot/bin/x`.
  - A typed path containing `/` is an absolute namespace path.

  dbg_1 keeps today's BootFS names.
- **init log lines that go:** `init: bootfs has N entries`, `init: launched the launcher`, `init: vmex is the launcher's alone`, `init: launched shell`, `init: shell says "..."` and `init: shell closed its channel` (`init.cpp:123`, `:393`, `:401`, `:416`, `:457`, `:468`).
- **Host tests that go:** `system/service/init/test:startup` and `system/lib/launch/test:protocol` (step 16).
