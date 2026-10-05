# shell changes

## Title: the shell runs as a component of the root: /boot and /svc from its manifest, commands through fuchsia.process.Launcher, the Debug resource from DebugResource (component-framework step 15) (2026-10-04)

**Files**
- `examples/shell/BUILD.gn` — new `shell.cm`, the shell's manifest: `boot` at /boot with rx*, and `fuchsia.process.Launcher` and `fuchsia.kernel.DebugResource` in its /svc; fsio, wire and the two codecs in the shell's deps; routing_checks added to `programs.hpp`
- `examples/shell/shell.cpp` — `main()` installs the namespace with fsio and tells the images apart by a /boot in it; on dev the Debug resource comes from `DebugResource.Get`, `ls [directory]` lists a namespace directory with ReadDirents, and a command runs `/boot/bin/<name>` through `fsio::spawn()`; a command that exits with a code other than 0 says so; `answer_init()`, the ping's other half, is gone
- `examples/shell/shell_launcher.hpp` — `Starter` holds `component` instead of the launcher-service connection
- `examples/shell/shell_tests.hpp`, `shell_tests.cpp` — `Inputs` loses `peer`, `connection` and `startup_to_close` and gains `component`; `check_launcher_service()` rewritten against fuchsia.process.Launcher and moved before `check_handle_close()`, which now closes the job on both images; new `check_spawn()`; the probes gated on the BootFS and VMEX
- `kernel/products/image/dev/BUILD.gn` — root.cm gains the eager child `shell`, offered `boot` (rx*) and the two built-in protocols; `bin/launcher` is no longer packaged
- `system/service/init/init.cpp` — init no longer starts the standalone launcher or the shell and drops the ping; the autorun program is started with `launch::start()` by direct call
- `system/service/init/pools.hpp` — `INIT_OWN_SLOTS` and `INIT_OWN_HANDLES` gone
- `system/service/init/BUILD.gn`, `realm.cpp` — comments that described init starting the shell
- `docs/repairs/261002/launch.md` — item 3 rewritten: a file VMO on dev is the root's slice, no longer the shell's slice of an executable BootFS
- `docs/repairs/261004/init.md` — items 1, 4, 8 and 9 say what the shell now holds and uses

**Symptom**
None; this is step 15 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`). Before it, on dev, init started the standalone launcher and handed it a VMEX duplicate, then started the shell by name through it, with a ping channel (User0), the raw BootFS VMO, a launcher connection (User1) and a duplicate of the Debug resource. The shell listed flat BootFS names from its own mapping of the image, and ran commands by BootFS name through a launcher that resolved the name for it. init also blocked outside its Loop, in `launch::Client::launch()`, for the shell and for autorun.

**Cause**
The objects of a typed command on dev, `routing_checks child`:
- the shell, a component of root.cm, whose namespace holds `/boot` (the root's built-in `boot`, rx*) and `/svc` (a directory the root serves, with one entry per protocol shell.cm uses);
- the root, which serves both, routes every /svc Open, and hosts the Launcher;
- the program's file, `bin/routing_checks`, a slice of the read-only BootFS that the root made executable at ingest because it lies under `bin/`;
- the Launcher connection, whose state collects the Add* calls until Launch.

The events, in fdio_spawn's order (design D12):
1. The shell builds `/boot/bin/routing_checks` from the word it was typed (`command_path()`), and `fsio::spawn()` opens it with READ_BYTES and EXECUTE, then asks GetBackingMemory for READ, EXECUTE and PRIVATE_CLONE. The VMO comes back with Execute because the node is executable and the connection holds EXECUTE.
2. fsio connects to `/svc/fuchsia.process.Launcher`; the root routes that use up to its built-in and binds the channel to a Launcher connection.
3. AddArgs `["routing_checks", "child"]`, AddHandles with a duplicate of the shell's job as JobDefault, AddNames with a Clone of `/boot` and of `/svc`.
4. Launch with the VMO: the root's `launch::start()` maps it with Execute into the root, plans and binds from that mapping, starts the process and answers with its handle.
5. The shell waits for TERMINATED and reads the exit code.

Why each piece is shaped the way it is:
- **Why the shell must stop holding the BootFS.** Before this step the shell's authority came from its position: init handed it the whole image, manifests and init itself included, and a launcher that resolved any BootFS name on its behalf. Under S5 a process's authority is what its manifest uses and its parent offers, and nothing else. The shell's is now exactly shell.cm's three uses, a duplicate of the root job (D6's gap, init repair item 2), the vDSO's VMO and its own region. Taking an entry out of root.cm's offers takes it away from the shell, which the mutation check below shows: the route is the only gate.
- **Why the shell can run a program without VMEX.** What makes a VMO executable moved from "who holds VMEX" to "does the handle carry Execute". The root holds VMEX, made every file under `bin/` executable at ingest (D15), and hands Execute out only through GetBackingMemory on a connection that holds EXECUTE (step 4's vfs). The Launcher takes whatever VMO it is given and maps it with Execute before reading a byte, so a VMO without the right is refused there (D14, I6). The Launcher resolves no name: the shell's own namespace decided what `routing_checks` is. With the standalone launcher no longer started, the root is the only process on dev that holds VMEX, which is the premise I6 states and which the step 13 entry noted held only in substance between steps 13 and 16.
- **Why the Debug resource comes through a protocol.** The shell needs the System Debug resource for `debug_read`, the console's input. A startup handle would be the root deciding in code who gets it; a routed protocol puts the decision in root.cm like every other capability (D19 a). Fuchsia's console asks `fuchsia.kernel.DebugResource` the same way. The shell asks once, at start, before its checks, so that a broken route fails the boot with a FAILED line instead of after `--- end ---`.
- **Why the shell tells the images apart by what it is handed.** It used to read "no User0" as "userboot started me". The ping is gone (D19 a), so User0 is missing on both images, and dev would have taken dbg_1's branch: `make_launcher()` with no BootFS and no VMEX, and `launcher checks FAILED`. The real difference is the capability: a namespace with `/boot` means "I am a component; programs are under /boot/bin and the Launcher starts them"; the BootFS and VMEX mean "I start programs myself". `main()` installs the namespace first and asks for `/boot`; the library launcher is made only for a shell that is not a component, and the probes run only where the BootFS and VMEX are both there.
- **Why `check_launcher_service()` and `check_spawn()` run before `check_handle_close()`, and why that closes the job on both images.** `check_handle_close()` gives up one startup handle and checks the kernel then refuses it. On dev it used to close the ping channel, which no longer exists; the job is the one startup handle both images have. Every check that starts a process in the job, or hands a duplicate of it to the Launcher, must come before it, so both new checks moved up beside `check_vma_unmap()`. The shell's commands use the duplicate `main()` made before the checks.
- **Why the raw-BootFS checks skip on dev.** They read the BootFS VMO itself: its header, slices of it, `vmo_replace_as_executable` with VMEX. A component has no BootFS handle, which is the point of this step; it has /boot as a directory. Each check already skipped when handed none, so nothing in them changed.
- **Why `check_spawn()` starts routing_checks.** It is the one program in dev's BootFS that ends on its own and checks something: with `child` it installs the namespace it was handed, reads its own first bytes through `/boot`, and exits 0. Spawning it walks the whole path a command takes, AddNames' clones included, so a broken export fails the boot, not only a typed command. Its path comes from its install name through `user_program_names()`, so no program name is spelled in the shell.
- **Why autorun moved to a direct call in this step.** The standalone launcher was autorun's only path, and init stops starting it here. D25 (a) keeps autorun in the root, resolved in its own /boot and started with `launch::start()` as the ELF runner starts a component, with argv and the root region as before. That also removes init's last wait outside `port_wait`: `launch::Client::launch()` wrote a request and blocked in `object_wait_one` for the reply, for the shell and then for autorun, before the Loop ran. Now `Loop::run()` is the only place the root waits (I1).
- **Why the pools shrank.** `INIT_OWN_SLOTS` were the ping channel and the shell's process, which init waited on by hand; the shell is now supervised like any component, its TERMINATED slot among the three per component. `INIT_OWN_HANDLES` were the 14 handles init made for the launcher and the shell; the shell's are now among `HANDLES_PER_COMPONENT`. `HANDLES_FOR_CLIENTS` goes from 449 to 463.

**Change**
The shell's start, before and after:

```cpp
// Before: who started it was read from User0.
auto const peer = ort::take_startup_handle(ours::HandleType::User0, 0);
auto const connection = ort::take_startup_handle(launch::protocol::CONNECTION, 0);
answer_init(peer);
if (job != ours::Uandle::Invalid && peer == ours::Uandle::Invalid) {
    starter.library = make_launcher(bootfs, vmex, vdso_vmo, vma);
}

// After: what it was handed decides.
if (auto const status = fsio::install(); status != ours::Status::Ok) { ... FAILED ... }
auto const component = is_bound(BOOT_PATH);                        // "/boot"
if (component) {
    debug_resource = debug_resource_from_svc().unwrap();           // DebugResource.Get
}
if (job != ours::Uandle::Invalid && !component) {
    starter.library = make_launcher(bootfs, vmex, vdso_vmo, vma);  // dbg_1
}
```

A command, before and after:

```cpp
// Before: a BootFS name, resolved by the standalone launcher.
launch::Client(starter.connection).launch(job, "bin/x", args, {}, {}, {});

// After: a namespace path, spawned as fdio_spawn spawns it.
fsio::spawn(job, "/boot/bin/x", args);
```

root.cm gains the child and offers the same `boot` and protocols to it, with CML's list form of `to`:

```gn
children = [
  { name = "routing_checks" ... },
  {
    name = "shell"
    manifest = "//examples/shell:shell.cm"
    startup = "eager"
  },
]
offer = [ { directory = "boot" from = "parent" to = [ "#routing_checks", "#shell" ] rights = [ "rx*" ] }, ... ]
```

The dev log now starts:

```text
init: started with the root job
init: /boot has 8 files, 3 executable
init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")
init: started routing_checks
init: started shell
shell: started by the root, with /boot and /svc
shell: started without vmex
shell: started without a BootFS
```

and the shell's new checks print:

```text
shell: spawn(a program /boot does not have) = NotFound
shell: Launch(a VMO without Execute) = AccessDenied
shell: Launch(after AddHandles with a VmaRoot) = InvArgs
shell: Launch(after AddHandles with 63 handles) = InvArgs
shell: the Launcher's answer to a malformed message = PeerClosed
shell: launcher service checks passed
shell: routing_checks child exited with 0
shell: spawn checks passed
```

The lines that went: `init: launched the launcher`, `init: launched shell`, `init: shell says "pong"`, `init: shell closed its channel`, `shell: init says "ping"`. dbg_1's log is unchanged but for `shell: started by userboot` (it was `...; no one to answer`) and two `skipped (no namespace)` lines.

Rejected:
- Keeping User0's absence as the test and adding a second test for dev: two tests for one fact, and the one that broke would be the one nobody read.
- Getting the Debug resource lazily, at the first `debug_read`: a broken route would then show only after `--- end ---`, where boot_check gives it one second.
- A new required line in `boot_check.py` for the spawn check: every failure of the new checks prints FAILED, and a dev shell without a namespace prints `launcher checks FAILED` from `make_launcher()`, so a silent skip on dev cannot pass.

**Reference**
- Fuchsia's serial shell is started by console-launcher, whose manifest uses the same three capabilities: `directory: "boot"`, rx*, at `/boot` (`src/bringup/bin/console-launcher/meta/console-launcher.cml:80-84`), `fuchsia.process.Launcher` (`:143`) and `fuchsia.kernel.DebugResource` (`:163`); it spawns the shell with `fdio_spawn_etc()` (`ConsoleLauncher::LaunchShell()`, `src/bringup/bin/console-launcher/console_launcher.cc:70-135`), and autorun the same way (`LaunchAutorun()`, `main.cc:106-196`).
- Fuchsia's console gets the Debug resource it reads the serial port with from the protocol: `get_debug_resource()` (`src/bringup/bin/console/src/main.rs:48-52`), then `zx_debug_read` in `debug_reader_thread()` (`:54-104`).
- fdio's spawn path: `fdio_spawn_etc()` (`sdk/lib/fdio/spawn.cc`: AddArgs `:896-897`, AddHandles `:638`, AddNames `:655`, Launch `:989`) and `fdio_get_vmo_exec()`'s flags (`sdk/lib/zxio/get_vmo.cc:142-144`).
- Design: `docs/design/261004/component-framework.md`, "Events: the shell running a program", "Events: dev.dbg_1", I1, I6, D6, D12, D19, D22, D25, D29.

**Verification**
- `boot_check.py`: dbg PASS (28 passed, 2 skipped, 0 failed, 0 error lines), rel PASS (25 passed, 7 skipped, 0 failed, 1/1 required, 0 error lines).
- The dev console, typed into after `--- end ---` (a scratch script driving `scripts/qemu_boot.py`'s serial port): `ls` lists `bin/ config/ meta/ vdso`; `ls /boot/bin` lists `init routing_checks shell`; `ls /svc` lists the two protocols; `routing_checks child` and `/boot/bin/routing_checks child` return to the prompt with nothing printed, that is with code 0; `routing_checks` alone prints eight FAILED checks and `shell: routing_checks exited with 1`, as the plan expects of a copy started with the shell's namespace instead of its manifest's; `no_such` answers `shell: launch failed: NotFound`; `ls bin` answers NotFound, there being no working directory (D29).
- Mutation check on the rel image, reverted: the two protocols offered to `#routing_checks` only. The root logs `init: shell's use of "fuchsia.kernel.DebugResource" routes nowhere: NotFound ...` and one such line per Launcher connect, the shell prints `DebugResource.Get FAILED: NotFound`, `launcher service checks FAILED` and `spawn checks FAILED`, and, holding no Debug resource, exits after its checks: `init: shell exited with 0`.
- Autorun, on a build configured with `tools/set.py bringup.x86 --out out/autorun --autorun "bin/routing_checks+child"`: `init: autorun bin/routing_checks+child`, then the program runs, without a namespace, so its read answers NotFound, as an autorun program has no namespace.
- Host tests: init's autorun 5, fsio 34, process_launcher 15, the component decoder 16 and router 32, all passed; `python3 system/test_component_manifest.py` OK.
- clang-format clean on every changed line; clang-tidy reports nothing new on `shell.cpp`, `shell_tests.cpp`, `init.cpp` and `realm.cpp` (only the existing `bootfs/view.hpp` member names and `shell_tests.cpp`'s older local `H`).

**Deliberately not done**
- Deleting the standalone launcher, the name-based protocol (`launch/protocol.hpp`, `launch/client.hpp`), `init_startup()` and `config/init/startup`: step 16. The launcher is still built, only not packaged into dev's BootFS, and init's `programs` list still names it and the shell.
- The shell serving its outgoing directory: it exposes nothing, so its DirectoryRequest stays unclaimed in ort's table, as routing_checks' does; `svc::Outgoing` is step 17.
- A job per command: commands run under a duplicate of the root job (D6, init repair item 2).
- `ls` with columns, sorting or a working directory (D29).
- One shared helper for the Launcher checks: the shell's `Received`, `launch_answer()` and `add_handles()` repeat routing_checks' `Incoming`, `launch_status()` and `add_handles()`. Two small copies in two test programs; a test-helper library would be a change of its own.
- Departures from the plan's text, each forced by it or small: autorun moves to a direct call in this step, since the standalone launcher it went through is no longer started (D25 a); "no executable" among the Launcher checks is `fsio::spawn()` of a program /boot does not have, as in routing_checks; the shell prints a command's exit code when it is not 0, which the console check needs to tell 0 from anything else.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- On dev after this step, which processes hold VMEX, and what property of the VMO the shell hands to Launch lets it run a program without VMEX?
- The shell used to tell dev from dbg_1 by User0 being absent. Why does that no longer work, and what would dev have printed had it been kept?
- Why must the Launcher and spawn checks run before `check_handle_close()`, and which handle do the shell's commands run under after the job it was given is closed?
