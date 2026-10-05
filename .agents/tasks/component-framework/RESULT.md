# component-framework: init is the root of a component tree that serves /boot and /svc, starts components from manifests and routes their capabilities

**Date**: 2026-10-05

## Delivered
All seventeen steps of PLAN.md are on baseline/20260824, landed on 2026-10-04 up to `1736f670`. On dev, userboot starts `bin/init`, the root. It serves /boot from the BootFS, with each file under `bin/` made executable at ingest, resolves `meta/root.cm`, starts its eager children routing_checks, echo and the shell one after another, builds each one's namespace by routing its uses, and serves each one's /svc, where `ours.process.Launcher` and `ours.resource.Debug` are built in. The shell runs its commands through the Launcher, and routing_checks reaches echo, a sibling, through an expose. dev.dbg_1 is unchanged: userboot starts the shell there.

Hashes are those on baseline/20260824. mvp/integration holds the same commits under other hashes, one for one by subject (`git log --format='%h %s' 116c12ca..mvp/integration`).

| Step | Commits |
|---|---|
| 1 | `116c12ca` docs(design): record the component-framework model |
| 2 | `a0558616` feat(bootfs): refuse file data that is unaligned or overlaps another |
| 3 | `26eb6f63` feat(fuchsia-io): hand-written codec for the fuchsia.io subset; `aed7128a` fix(fuchsia-io): test the length checks no test reached |
| 4 | `3dbbdd81` feat(vfs): fuchsia.io server library; `c6fefb35` fix(vfs): generation 0 never names a live connection |
| 5 | `3d9fc94a` feat(fsio): client namespace and file calls; `08e9e848` fix(fsio): read the epitaph when the request meets a closed peer; `c3b6d136` feat(fsio): keep a closed namespace entry's epitaph, and look for epitaphs first |
| 6 | `501c82b0` feat(launch): fuchsia.process.Launcher codec and connection state; `65205d8b` fix(launch): test that AddNames checks the string arena; `976a15d7` fix(launch): ProcessLauncherState can be constinit |
| 7 | `b44e3900` feat(component): manifest template, compiler and decoder; `c63b63db` fix(component): Records iterators hold the view, not the Records |
| 8 | `792eabd3` test(component): spec and failing tests for route(); `9350136c` fix(component): pin route()'s declaration lookup by name; `78b5df7f` feat(component): route a use to its source; `8e8466f9` docs(component): say which hop a rights error names, and whose router that matches |
| 9 | `2fd29844` feat(syscall): vma_unmap |
| 10 | `a3802fc9` feat(syscall): vmo_get_size; `00b55c44` docs(syscall): cite BootfsSvc's content size where it is set |
| 11 | `8dddeac8` refactor(launch): start() reads the program from its VMO |
| 12 | `182f4edf` test(loop): spec and failing tests for Loop::run(); `36d3128e` fix(loop): pin run()'s step 1 with tests that do not pass by luck; `dccf48a0` feat(loop): one port and a fixed slot table; `3ca8035b` fix(loop): say when a callback may answer Rearm, drop a stale stub note; `8a20b346` fix(loop): a duplicate of the port can forge a SignalOne packet too; `3d2ffcee` docs(loop): repair item for a failed re-arm its callback never hears of |
| 13 | `8792b01b` feat(init): serve /boot and start components from meta/root.cm; `717b8ab7` fix(init): count the handles init makes for the launcher and the shell; `b071b260` fix(init): log a namespace entry the root routed but could not serve; `f1bc1cf7` fix(ours-verify): stop once a FAILED line decides a boot with required lines; `c8d9a3b7` docs(init): repair item for namespace entries filling /boot's connections; `738963aa` docs(init): say which step 13 claims wait for the standalone launcher to go |
| 14 | `0acff140` feat(init): /svc, the Launcher and DebugResource; `4eca7be9` fix(init): comments and repair item 4 catch up with step 14; `5bdc9949` fix(fsio): README names export_namespace() and spawn() |
| 15 | `8917bb7d` feat(shell): run as a component of the root |
| 16 | `5d9b1bbf` refactor(launch): remove the standalone launcher and launch-by-name; `1736f670` fix(launch): comments the launcher's removal left stale |
| 17 | `9127a9c1` feat(svc): outgoing directory, and an echo provider; `0d42c105` fix(init): repair item 4 counts echo among the components after integration |

Also on the run's branch, outside this plan:
- The protocol libraries took ours's own names on the user's decision of 2026-10-04 (D20 revised): `1cdc01aa` refactor(ours-io): the protocol libraries take ours's own names; `0bde80fc` fix(ours-io): comments the rename left naming process_builder, or ragged; `4874fb55` fix(build): the ACPI tests' comment sits above its own label again; `1fe5bef2` refactor(lib): the protocol libraries live in ours.io, ours.process and ours.resource.
- The comment rule and the whole-repo sweep the user asked for the same evening: `5fbf398f` docs: comments say what a thing is, without naming other systems (the rule in CLAUDE.md and AGENTS.md); 23 sweep commits from `2ab0e8b1` docs(mm) to `e0cb246d` fix(build); and 11 review fixes on 2026-10-05 from `3c6717bc` fix(mm) to `93bb2ca2` fix(build). Each group's record is in `docs/changes/261004/comment-rule.md`.
- The interrupt plan's steps, in `.agents/tasks/microkernel-interrupts/RESULT.md`.
- 31 of the prof session's commits, cherry-picked onto mvp/integration so that the interrupt steps built on the Sampler work. Their originals reached baseline through that session; they are in `.agents/tasks/prof-sampler-object/RESULT.md`.

## Verification
- `boot_check.py --out out/results` from a fresh out dir on mvp/integration at `44924c04`: build ok (31.2 s); dbg PASS (34 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (30 passed, 10 skipped, 0 failed, 3/3 required, 0 error lines). The same on baseline/20260824 at `2f14dc0c`, also from a fresh out dir: build ok (31.1 s), dbg 34 passed and 2 skipped, rel 30 passed and 10 skipped with 3/3 required.
- dev's log starts with `init: started with the root job`, `init: holding the IRQ and I/O port resources`, `init: /boot has 9 files, 4 executable`, `init: started routing_checks`, `init: started echo`, `init: started shell`. Later, in no fixed order: `routing_checks: passed`, `init: routing_checks exited with 0`, `echo: stopping, as a client asked`, `init: echo exited with 0`, and the shell's `spawn(routing_checks child) = Ok`, `routing_checks child exited with 0`, `spawn checks passed`, `launcher service checks passed`, `loop checks passed`, `vma_unmap checks passed` and `vmo_get_size checks passed`.
- Lines on dev that read like failures are checks passing: `init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")`, the same with NotFound for `ours.test.Unoffered`, and twice `init: launch failed: mapping the program's file into this process: AccessDenied`, the Launcher refusing a VMO without Execute.
- dev.dbg_1: `shell: start() Execute checks passed`, `shell: loop checks passed`, `shell: vma_unmap checks passed`, `shell: vmo_get_size checks passed`; `launcher service checks skipped (no namespace)` and `spawn checks skipped (no namespace)`, as userboot starts that shell.
- Host tests built into `out/results`, every one `[  PASSED  ]`: bootfs `view` 9, launch `plan` 36, `process_launcher` 15 and `program` 8, `wire` 12, `ours_io` 21, `ours_process` 21, `ours_resource` 6, `vfs` 42, `fsio` 34, component `decl` 16 and `route` 32, `loop` 18, `svc` 9, `ours_examples_echo` 7, init `autorun` 5. `python3 system/test_component_manifest.py`: 48 tests OK. `python3 system/lib/wire/test/ordinals_test.py`: 5 tests OK.
- Step 7's manual gn gen checks, each made on `system/lib/component/test/fixtures/BUILD.gn` and reverted:
  - a dangling child, `manifest = ":ghost.cm"` with no such target, fails gen:

    ```text
    ERROR Unresolved dependencies.
    //system/lib/component/test/fixtures:realm.cm(//build/toolchain/ours:kernel-x86)
      needs //system/lib/component/test/fixtures:ghost.cm(//build/toolchain/ours:user-x86)
    ```

  - a child label spelled with a toolchain fails the template's assert: "component_manifest(realm.cm): a child's `manifest` takes a plain label without a toolchain suffix";
  - a program label spelled with a toolchain fails the same way: "component_manifest(provider.cm): `program` takes a plain label without a toolchain suffix";
  - packaging: with `fixtures:realm.cm` in dev's `user_bootfs` deps, `bootfs.fini` held `meta/realm.cm`, `meta/provider.cm`, `meta/consumer.cm`, `meta/hollow.cm` and `bin/fixture_program`.
- Step 15's console check, scripted then and not repeated here (`docs/changes/261004/shell.md`): after `--- end ---` on dev, `ls` lists `bin/ config/ meta/ vdso`, `ls /boot/bin` the programs, `ls /svc` the shell's protocols, and `routing_checks child` comes back with code 0.
- Each step's mutation checks are in its change record.
- Not measured: every performance ceiling the design names (D11, D13 to D18, D27), the root's handle quota under exhaustion, and the root under DEBUG_ASSERT (D22 a).

## Deviations from the design
- **P3's two hand-written functions were written by the AI.** On 2026-10-04 the user asked for an unattended MVP, so `route()` (step 8) and `Loop::run()` (step 12) were written by the AI against the failing tests written first. Both exercises stay open, and `docs/authored.md` has no row for either. To take one up, restore the stub from the commit that wrote its tests (`792eabd3` for `route()`, `182f4edf` for `run()`), write the body against the tests under a time limit, and have it reviewed.
- **The concurrency argument was skipped**, by the user's instruction for this run. The design's "Concurrency: one thread and one Loop" (D16) is what the user's argument would be graded against.
- **Every S, D and P decision was taken as recommended**, with no prediction asked ("Not asked" throughout the design's table).
- **D20 was revised by the user on 2026-10-04**: the protocols and their libraries take ours's own names, `ours.io`, `ours.process` and `ours.resource` in `system/lib/ours.io`, `system/lib/ours.process` and `system/lib/ours.resource`, every ordinal recomputed from the new selectors; `fuchsia.kernel.DebugResource` became `ours.resource.Debug`. PLAN.md and the records of steps 3 and 6 keep the old names as history.
- Step 2: "identical ranges" is read as the same offset and the same size, so two files that share a page-rounded range with different sizes are refused; `format.hpp` and `builder.hpp` change too, for the page constant.
- Step 4, within D21: `PERM_INHERIT_WRITE` and `PERM_INHERIT_EXECUTE` are ignored; a service node returns `Connect` to the program instead of calling a callback; a connection record does not hold its channel, whose Loop slot is its only owner.
- Step 7: the compiler also refuses a label that names a toolchain in the JSON it reads, so that the Python unit test covers the rule; a manifest name is at most 100 bytes, closing the design's open question.
- Step 8: `route()` terminates because `resolve()` fails once the root's instance pool (D27) is full, not because of "twice the depth of the tree", which a manifest naming its own URL as a child disproves; a use's own subdir past the limit answers a new `Verb::Use`.
- Step 9: the sealed-vDSO refusals are checked on a process the check makes, not on the shell's own vDSO. `vma_unmap` landed before `docs/repairs/261002/object.md` item 2, whose Acceptance asked to land first (Found along the way).
- Step 12: three choices go past the sketch of `docs/design/260924/userspace-filesystem.md` §4.2: `run()` takes a deadline; the Loop is handed its port rather than making it; `add()` leaves the handle with the caller on failure, so that a full server can still send its NoResource epitaph.
- Step 13: until step 16, two invariants held only in a weaker form, as P2 (a) chose: VMEX had two holders, and init still blocked outside the Loop (`738963aa`). Beyond the plan, the root logs a namespace entry it routed but could not serve (`b071b260`), and boot_check stops once a FAILED line has decided a boot with required lines (`f1bc1cf7`).
- Step 15: autorun moved to a direct `launch::start()` call here, since the launcher it went through was no longer started (D25 a); the Launcher's "no executable" check is `fsio::spawn()` of a program /boot does not have; the shell prints a command's exit code when it is not 0.
- Step 17: echo's protocol, `ours.examples.echo.Echo`, has a Stop method of ours's own, since there is no Lifecycle channel and no `task_kill` (D23).

## Found along the way
Open, Medium:
- `docs/repairs/261002/object.md` item 2: `vma_allocate` and `vma_map` undo a failed call by address range. Since `vma_unmap`, a thread sharing the region can make that undo remove its own object. It must land before a process can run a second thread.
- `docs/repairs/261004/mm.md` item 1: unmapping part of a mapping leaves the mapping's whole range reserved, reachable from ring 3 since `vma_unmap`.
- `docs/repairs/261004/loop.md` item 1: `Loop::run()` frees a slot whose re-arm failed without telling the slot's callback.
- `docs/repairs/261004/init.md` items 1 to 4: nothing notices the root's own death; no job per component and nothing that can stop one (D6); a write to a full channel is dropped (I1); one handle quota shared by every client (D18).

Open, Low:
- `docs/repairs/261004/init.md` items 5 to 10, among them item 6, the root never running with assertions on (D22 a), and item 10, a route to an exited provider answering NotFound with no root log line.
- `docs/repairs/261004/component.md` item 2: whether the child URL scheme `fuchsia-boot:` should keep another system's name, a question for the user.
- Performance observations to measure before anything changes: `docs/repairs/261004/vfs.md` items 1 and 2, `fsio.md` item 1, `component.md` item 1, `launch.md` item 1, `bootfs.md` item 1, `loop.md` items 2 and 3; and `svc.md` item 1, the Outcome-to-channel glue written twice.
- From the comment sweep: `docs/repairs/261004/comment-rule.md` item 1, in `fair.hpp`, which is the user's to edit; `build-dist.md`, `build-toolchain.md` items 1 and 2, `tools-set.md`; `syscall.md` item 1, two system calls that answer a bad option before their handle; `object.md` item 4, a channel-full policy exception now possible.

Fixed in passing: a vfs `ConnectionId` with generation 0 named a live connection (`c6fefb35`); fsio missed an epitaph when a request met a closed peer (`08e9e848`, `c3b6d136`); `ProcessLauncherState` could not be `constinit` (`976a15d7`); init's handle sums were 4 short (`717b8ab7`); a duplicate of the Loop's port could forge a SignalOne packet (`8a20b346`).

Not recorded in `docs/repairs/`: every `*_dist` action declares a depfile it never writes (`ninja -d explain`: "depfile ... is missing"), so ninja re-runs them and repacks the image on every build. It belongs to the distribution templates, outside this task.

## Not done
- The user's exercises: P3's `route()` and `Loop::run()` written by hand, and the concurrency argument for D16 (Deviations).
- Performance work: the measurements the repair items above ask for, and the ceilings D11, D13 to D18 and D27 set.
- The design's non-goals: lazy start, collections and dynamic children, environments, /pkg and package resolution, shared libraries, a job per component, stopping or restarting a component, a security policy on the built-ins, and directories provided by components.
