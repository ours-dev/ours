# server-guide-testing: chapter 5 of the server guide, testing and debugging a server

**Date**: 2026-10-29

## Delivered
Drafted, not committed, as `.agents/tasks/server-guide/PLAN.md` step 2 asks (bullet `guide-testing`):

- `docs/guide/05-testing-and-debugging.md`: how host tests are found, built and run (`scripts/host_tests.py`); what a fake kernel is here and how `system/lib/ioservice/test` and `system/lib/loop/test` use one; a negative case on the host; how to add a host test; booting the dev image with `boot_check.py` and reading its log; how `routing_checks` makes a negative case; `kshell_run.py` in one paragraph; and "What went wrong", with the four triage questions, a 13-row table (what the client sees, what happened, the root's log line, the code that produces both), a table of the root's other log lines, and two worked cases. It links `docs/development/debugging.md` for the debugger and does not copy it.
- `docs/repairs/261029/cpp-library-template.md` and `docs/repairs/261029/routing-checks.md`: the two defects below.

Checked against commit `693260d0`; none of the files the chapter cites was modified in the working tree.

## Verification
Commands I ran, all read-only: `python3 scripts/lint_docs.py --changed` on the chapter and on the two repair files (no violations); `python3 scripts/lint_docs.py --help`; `python3 scripts/kshell_run.py --help`; `python3 scripts/qemu_boot.py --help` (this prints the script's "booting" banner and then QEMU's own help text, and no machine starts: the script hands `--help` to QEMU); `prebuilt/gn/gn outputs out/bringup.x86 <label>` for the three ioservice test labels, the loop test label and `//system/lib/loop:tests`, and `prebuilt/gn/gn ls out/bringup.x86 --testonly=true --type=executable`, to confirm where a host test's binary lands and that the labels end in `(//build/toolchain/ours:host)`; `grep -n "^default" out/bringup.x86/build.ninja` (`default default`, so the target the build makes when none is named is `//:default`). A small Python checker in the session scratchpad confirmed that every relative link and anchor resolves (12), that every one of the 170 `file:line` citations lies inside its file, and that the 14 code and log blocks are verbatim copies of the lines they cite (at most 15 lines each). I read the first line of every cited range, and the whole range for the ones the table depends on.

Only read, never run: `boot_check.py` (its verdict rules, `REQUIRED`, flags and log directory), `.agents/skills/ours-verify/SKILL.md`, `kshell_run.py`, and the outputs the change records print. I did not build anything, did not boot an image and did not run a host test binary: the binaries in `out/bringup.x86` are older than the sources (the ioservice test binary is dated 2026-10-23), and the checkout holds other sessions' uncommitted kernel edits.

Facts checked against the code, with where:

- How host tests are found: the rule and its three GN queries, `scripts/host_tests.py:4-13`, `:22`, `:24`, `:44-50`; `//:default` is `//BUILD.gn:12`; each test runs from the build directory and passes on exit 0, `boot_check.py:214-221`, `.agents/skills/ours-verify/SKILL.md:29-30`; the library tests are wired by a `tests` group (`system/lib/loop/BUILD.gn:28-31`) named in the root list (`//BUILD.gn:190`, `:210`, `:214`, `:219`).
- What the fake kernel is: it defines the vDSO's own symbols (`system/lib/ioservice/test/fake_kernel.cpp:208-251`), the library has no seam (`system/lib/loop/BUILD.gn:9-11`), the rules it keeps and leaves out (`system/lib/ioservice/test/fake_kernel.hpp:15-40`), the loop fake's helpers and its `BadState` answer (`system/lib/loop/test/fake_kernel.hpp:15-35`, `:57-76`, `:97-100`), and that `//kernel/tests/fake-kernel` is another thing (`kernel/tests/fake-kernel/README.md:1-8`).
- How the tests use one: `system/lib/ioservice/test/loop_server_test.cpp:177-190` (fixture), `:222-225` (`run()` is `m_loop->run(0)` expecting `TimeOut`), `:293-303`; `system/lib/loop/include/loop/loop.hpp` (`run()`: deadline 0 returns once the port is empty); `system/lib/ioservice/test/outgoing_test.cpp:204-217`; `system/lib/ioservice/test/server_test.cpp:289-297`.
- Booting and reading the log: the pass rules (`boot_check.py:15-20`, `:75`, `:90-100`, `:153`, `:408-409`, `:507-508`), the two images and that only `rel` has a root (`:66-73`, `:81-89`), the log directory (`:560`, `:588`), `ort::debug_line` (`system/lib/ort/include/ort/debug.hpp:16`, `:30-52`), the serial port (`scripts/qemu_boot.py:231`).
- `routing_checks`: the manifest uses (`system/service/init/test/routing_checks/BUILD.gn:75-84`), the offers that make `denied` and the unoffered protocol fail (`kernel/products/image/dev/BUILD.gn:132-155`), `check_unoffered()` (`routing_checks.cpp:527-542`), `check_denied()` (`:317-321`), `fail()` (`:137-141`), the epitaph reader (`:343-357`), the lost-Open race (`:805-825`), the final line (`:906-913`).
- Each row of the table, status and line: `system/service/init/realm.cpp` (`route` and reply `:740-745`, log `:201-203` and `:188-199`, provider gone `:782-785`, forward `:809-821`, built-in pool `:770-775`, `/svc` `:549-553` and `:689-691`), `system/lib/component/include/component/route.hpp` (`:307-317`, `:412-467`), `system/lib/ioservice/src/tree.cpp:203` and `server.cpp:96-102`, `:248-250`, `:335-337`, `:529-532`, `:561-565`, `loop_server.cpp:163-169`, `examples/echo/echo_server.cpp:132-141`, `:172-191`, `system/service/init/pools.hpp:32`, `:46`, `:55`, `launcher_server.cpp:79`, `resource_server.cpp:55`, `system/lib/fsio/include/fsio/fsio.hpp:52-74` and `namespace.hpp:124`, `system/lib/ours.resource/src/client.cpp:28-29` and `:75-79`, `system/public/ours/syscalls.gni:197-208`, `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28`.
- The verbatim log lines are from `docs/changes/261029/init.md:126-129` and `:137-138` (reaches another protocol) and `docs/changes/261004/init.md:42` and `:438` (routes nowhere, with `at .`); the shape of both is the code at `realm.cpp:188-203`.

**Not verified**

- No boot, so I saw no log line of the current tree. The two `routes nowhere` lines come from an older change record (`docs/changes/261004/init.md`); they agree with the format string in `realm.cpp:201-203` read now, and `docs/changes/261029/init.md:146` says the current rel log still holds them.
- Run times are the scripts' own figures: "about 25 seconds" (`SKILL.md:21-22`) and "about 10s" for the host tests (`boot_check.py:174`).
- `--gtest_filter` and `--gtest_list_tests` are read in the vendored googletest documentation, not run on a test here.
- That a failed `channel_write` closes the handles it was given (row 11) is read from the system call's documentation in `syscalls.gni`; I did not read the kernel's code for it.
- Row 12 rests on the comment in `routing_checks.cpp:805-825`, which cites `HandleTable::clean()` for the order a dying process's handles close; I did not read that function.
- Whether the debugger setups of `docs/development/debugging.md` and `.agents/skills/ours-verify/references/debugging.md` work for a user-space server: both are written for the kernel, I read the first 80 lines of the first and all of the second, and the chapter says nothing about it.
- Row 9 gives one cause (more than 15 protocol uses). `serve_svc()` has others (`add_svc_directory()` and `add_service()` failures, a use path that does not start with `/svc/`); I read them and did not exercise them.
- Rows 1, 2, 5 and 13 are read from the code and its tests; none was reproduced in a boot.

## Deviations from the design
- The brief lists AccessDenied among the ways a connection to a protocol fails. For a protocol it does not come out of routing: a protocol use states no rights, and `route()` answers AccessDenied only for a hop that states rights (`route.hpp:271-282`, `:318`). Row 6 is therefore the directory case, `denied`, marked "on a directory entry only", and a note under the table gives the one way a hand-written Open of a service meets it (`server.cpp:248-250`).
- The table has 13 rows, not five: NotFound has five causes and PeerClosed three, and the reader tells them apart by the root's line.
- The chapter says nothing about debugging a user-space server with a debugger (see Not verified).

## Found along the way
- Defect, `docs/repairs/261029/cpp-library-template.md` item 1: a library made from `tools/templates/cpp_library` gets a test target and a README that says its tests run on the host, but no `tests` group and no line in `//:default`, so `host_tests.py` never finds the test. The step is written only in Chinese and as optional (`tools/templates/README.md:37`). `docs/guide/03-protocols.md` already tells the reader to add the group. The protocols chapter's author wrote a different defect of the same template (the target name) into `docs/repairs/261029/tools-templates.md`; mine is a separate item about the unwired test, in its own file so that two agents did not edit one file, and the orchestrator may fold it into that one as item 2.
- Defect, `docs/repairs/261029/routing-checks.md` item 1: the comments at `routing_checks.cpp:24-27` and `:38` and at `routing_checks/BUILD.gn:38-48` count five or six protocol uses and omit `ours.resource.Sampling`; the manifest has seven uses.
- Already recorded, still true: a route to a provider that has exited logs nothing (`realm.cpp:782-785`), `docs/repairs/261004/init.md` item 10.
- A gap in debuggability, not a defect, for the user to decide: a client whose `/svc` lacks the name it connects to (the commonest mistake, a protocol missing from its own `use` list) gets `NotFound` and the root prints nothing (row 2). The refusal is made by the generic ioservice code (`server.cpp:529-532`), which has no log hook, so the root never sees it. A line there would need the root's `BootServer` to be told about refusals on `/svc`.
- Line numbers move while other sessions land the Loop's `Waiter` work. `loop_server.cpp`, `loop_server_test.cpp`, `launcher_server.cpp` and `resource_server.cpp` changed in `c2b7a75a` and `94a62227` while I wrote, and I re-read them. The chapter also cites `examples/echo/echo_server.cpp` at `:52-54`, `:132-141` and `:172-191`; step 4 of `.agents/tasks/loop-waiter/PLAN.md` moves `EchoServer` to a `Waiter` and will shift them, so the citations need one re-check after it.

## Not done
- Nothing was committed or staged, as instructed.
- No host test of a server's own, nor a boot, was run; the chapter's commands are the repository's, read from their documentation and source.
- A host test for `examples/echo/echo_server.cpp` does not exist; the chapter says so and points at `routing_checks` as the test of the example. Writing one is outside this chapter.
