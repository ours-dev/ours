# Testing and debugging a server

A server is checked in two places. Most of its logic can be tested on the machine you build on, quickly, without starting an image. What only the real kernel and the real root can decide, such as whether a client can reach the server at all, is checked in a booted image. This chapter shows both, and ends with a lookup table to keep open while you debug: for each way a client's connect to a protocol can fail, what the client sees, what the root prints, and what to change.

## What you will be able to do

- Decide which part of a server is tested on the host and which part waits for a boot.
- Find, build and run the host tests, and add one of your own.
- Boot the dev image, find your server's lines in its log, and tell a passing boot from a failing one.
- Write a negative case, a test that passes only when a request is refused for the right reason, the way `routing_checks` does.
- Read a failed connect from the status the client got and the line the root logged, and know what to change.

## Words used in this chapter

The kernel objects (handle, channel, port, signals, Loop, slot), the component words (component, manifest, use, offer, expose, epitaph) and [route](01-concepts.md#routes) are defined in [01-concepts.md](01-concepts.md), and so is the moniker that the root's log lines carry. The names that this chapter meets and does not explain (the vDSO, `ort`, built-in) are in [its list of words used elsewhere](01-concepts.md#words-used-elsewhere). The words below are the ones this chapter adds.

- *host*: the machine you build on. The *target* is the machine an image runs on, which here is the emulated machine that QEMU, an emulator the scripts start, provides.
- *host test*: a test program built for the host and run there directly. It uses googletest, a C++ test library: the program runs every test it holds and exits with 0 only if all of them pass.
- *fake kernel*: a stand-in for the kernel's channel and port calls that follows only the rules a test needs. The section "What a fake kernel is here" defines it exactly.
- *negative case*: a check that passes only when a request is refused, and only for the reason you expect.

## Two places to test

Which place a part of a server is tested in follows from whether it needs the kernel to be right.

| What you want to check | Where | How |
|---|---|---|
| The bytes of a message: encoding, decoding, what a decoder refuses | host | the codec makes no system call, so its test feeds it bytes: `examples/echo/protocol/test` |
| What a server decides: which answer a message gets, which connections it holds, what it refuses | host | `ioservice::Server` takes one message and returns what to do, and makes its one system call through a function it is given (`system/lib/ioservice/README.md:12`): `system/lib/ioservice/test/server_test.cpp` |
| A server on a Loop: what it reads, writes and closes | host, with a fake kernel | `system/lib/ioservice/test/loop_server_test.cpp`, `outgoing_test.cpp`, and `system/lib/loop/test/loop_test.cpp` |
| Whether the manifests route, the process starts, and a client reaches the server | a boot of the [dev image](README.md#build-and-boot-the-dev-image) | `routing_checks`, the dev image's test component |

The example server `examples/echo/echo_server.cpp` has no host test of its own. Its `BUILD.gn` asserts a user toolchain (`examples/echo/BUILD.gn:13-15`), so its program is built for the image only, and its behaviour is checked there: `routing_checks` sends it a string, asks it to stop, and checks that the next connect is refused. The parts it is made of are tested on the host: its codec, and `ioservice::Outgoing`, which holds its protocol.

Keep the same split in a server of your own. Put what you want to test on the host in a library target that builds in the host toolchain, and keep the program for the image. Echo is split so: the codec is a library in `examples/echo/protocol`, built for every toolchain and tested on the host, and the program in `examples/echo/echo_server.cpp` is built for the image only.

## Tests on the host

For a server of your own, host testing means its codec: the library of chapter 3, tested as that chapter's steps 6 and 7 describe. "How the host tests are found, built and run" and "Adding a host test of your own" are what you need to run it. The three sections between them, "What a fake kernel is here", "How the `ioservice` and `loop` tests use one" and "A negative case on the host", explain how the libraries a server is made of are tested, with fake kernels that belong to those libraries' test directories and cannot be reused elsewhere. Read them when you change one of those libraries, or to see what a negative case looks like; skip them for a first server, whose other tests are boots.

### How the host tests are found, built and run

To run the tests of one library, build its `tests` group and run its binary. `system/lib/loop/README.md:22-23` gives the commands for the Loop:

```text
prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/loop/tests
out/bringup.x86/host/obj/system/lib/loop/test/loop
```

The binary of `//system/lib/<library>/test:<name>` is `host/obj/system/lib/<library>/test/<name>` under the build directory, and `prebuilt/gn/gn outputs <out> '<label>(//build/toolchain/ours:host)'` prints it. The binary is an ordinary googletest program, so `--gtest_filter=<pattern>` runs the tests whose names match and `--gtest_list_tests` lists them. A test's name is `<fixture>.<test>`, for example `LoopServerTest.OpenOfAMissingNameAnswersNotFound`.

To run all of them, run the verify script, `boot_check.py`, which [README.md](README.md#build-and-boot-the-dev-image) describes. While the images boot it runs every host test, and a passing run prints a line like this one (your count and time will differ):

```text
host-tests PASS  148 passed, 0 failed · 0.7s · log <dir>/host-tests.log
```

The count is the number of test programs, one for each executable. A failing test fails the check, and its last lines are printed. `host-tests.log` holds all of its output, under a line `=== <label>: <status>` for each program. To run one failing program alone, run its binary from the build directory: `cd out/bringup.x86 && ./host/obj/<path>`.

How a program comes to count as a host test is one rule, in `scripts/host_tests.py`, shared by every script that runs tests, so a test is run by all of them or by none:

1. A host test is an executable that GN, the tool that generates the build files, marks `testonly = true` and builds in the host toolchain, `//build/toolchain/ours:host`. `testonly` is GN's own mark: only another `testonly` target may depend on such a target. It is not a label that says "test", so benchmarks carry it too, and `//benches` is left out because a benchmark reports timings and does not pass or fail.
2. Of those, only the ones `//:default` builds count. `//:default` is the target the build makes when you name none. Its list of dependencies in the root `BUILD.gn` is written by hand, so a test runs only when it is reachable from that list.
3. The list of tests is asked of GN on every run and kept nowhere. It takes three queries, with `prebuilt/gn/gn`: `gn ls <out> --testonly=true --type=executable` for the candidates, `gn desc <out> //:default deps --all` for what the default build reaches, and `gn outputs <out> <labels>` for the file each test links.

Each test runs from the build directory and passes when it exits with 0.

### What a fake kernel is here

A server calls `ours::channel_read`, `ours::channel_write` and the other system calls as functions. Each is declared in `<ours/vdso.hpp>` with an assembler label that sets the symbol the call refers to, `ours_<name>`. In an image nothing defines those symbols in the program: the loader binds them to the vDSO before the program runs (`system/public/ours/vdso.hpp:18-22` and `:47-56`).

On the host nobody binds them, so the test executable defines them itself. A *fake kernel* here is a small model of ports and channels held in ordinary memory, plus those definitions, which forward each call to the model. `system/lib/ioservice/test/fake_kernel.cpp` ends with them:

`system/lib/ioservice/test/fake_kernel.cpp:233-235`:

```cpp
auto ours::channel_create(ChannelCreateOptions, Uandle *const out0, Uandle *const out1) -> Status {
    return fake::kernel().channel_create(out0, out1);
}
```

The library under test is compiled unchanged and linked against these definitions, "so the library's own source is what runs" (`system/lib/ioservice/test/BUILD.gn:19-21`). There is no interface to inject and no `#ifdef`: "which of the two answers is fixed by what the executable links, so the library has no seam of its own" (`system/lib/loop/BUILD.gn:10-11`). A call the library is not meant to make has no definition in the fake, and the test fails to link (`system/lib/ioservice/test/fake_kernel.hpp:23-24`). That link error is a useful alarm: it says your code made a system call the model does not have.

Do not confuse this with `//kernel/tests/fake-kernel`. That is a different thing, a stand-in kernel image that proves the boot chain reached it (`kernel/tests/fake-kernel/README.md:1-8`).

The model keeps the rules of the real kernel that the libraries depend on, and nothing more (`system/lib/ioservice/test/fake_kernel.hpp:26-41`):

- A channel end is readable while messages are queued on it, and reports its peer closed once the peer is closed.
- A write queues the message on the peer, and fails with `PeerClosed` once the peer is gone. The handles in the message are consumed either way.
- A wait is one-shot and level-triggered: armed on an object that already asserts one of its signals, its packet is queued at once; otherwise when the object first asserts one.
- Closing a handle ends its armed waits without a packet, and closes the channel for its peer.
- A port hands its packets out in the order they were queued.
- A wait that a test has made fail answers the status it was given and arms nothing (`fail_next_wait_async()`, `system/lib/ioservice/test/fake_kernel.hpp:53-56`), which is how a test reaches a `Binding`'s `on_rearm_failed`.
- A `debug_write` longer than the kernel allows answers `OutOfRange` and writes nothing, so that a library that does not cut or split its line shows as a line that is missing (`system/lib/ioservice/test/fake_kernel.hpp:39-41`). The writes that were accepted can be read back.

It leaves out rights, and the moving of handles between tables: a handle written into a message keeps its value and stays open in the model (`system/lib/ioservice/test/fake_kernel.hpp:43-46`). So a test against the fake cannot show that the real kernel agrees. That is what the boot is for.

A test can also look inside the model: whether a handle is still open, how many messages are queued on a channel end, which handles were closed and in what order, what each accepted `debug_write` wrote (`system/lib/ioservice/test/fake_kernel.hpp:58-72`).

### How the `ioservice` and `loop` tests use one

There are two fakes, one per test directory, and they differ in how much they model.

- `system/lib/loop/test/fake_kernel.*` models only the three calls the Loop makes, `port_wait`, `object_wait_async` and `handle_close`. Because its tests have no real client, it gives them ways to make things happen: queue messages on an object, take one off, close the peer, queue a packet by hand, and make the next wait fail (`system/lib/loop/test/fake_kernel.hpp:57-76`). That last one produces a failure the real kernel does not produce on demand. It also answers `BadState` to a wait with no deadline on an empty port, which no real `port_wait` does, so that a test that would block for ever fails instead (`system/lib/loop/test/fake_kernel.hpp:97-100`).
- `system/lib/ioservice/test/fake_kernel.*` models the channel and port calls, `handle_close` and `debug_write`; its `handle_duplicate` only answers `NotSupported` (`system/lib/ioservice/test/fake_kernel.cpp:254-258`). Its tests need no helpers to move state, because they act as a client through the same calls the server uses: `ours::channel_create`, `ours::channel_write` and `ours::channel_read`, which land in the fake.

A test of a server on a Loop, such as `system/lib/ioservice/test/loop_server_test.cpp`, goes in five steps:

1. Reset the model, so that no object of an earlier test is left (`fake::kernel().reset()`).
2. Build the pieces over it: make a port with `ours::port_create`, a Loop over a table of slots, and the server on the Loop.
3. Act as the client: make a channel pair, hand the server end to the server, and write a request on the client end.
4. Run the Loop with the deadline 0, which dispatches whatever is queued and returns `TimeOut` once the port is empty (the comment on `run()` in `system/lib/loop/include/loop/loop.hpp`).
5. Read the answer off the client end, and check the state of the model.

If a host test never comes back from that run, some slot's wait keeps firing and the port never empties. The rules a slot's waiter has to keep so that this does not happen are stated in [the Loop and its slots](01-concepts.md#the-loop-and-its-slots) and in the comment on `Waiter` in `system/lib/loop/include/loop/loop.hpp`.

### A negative case on the host

A server is as much what it refuses as what it answers, and a refusal is easy to break without noticing, so test refusals as carefully as answers. A negative case names the exact status, and where the order of checks matters it pins the order. `system/lib/ioservice/test/server_test.cpp` does this for a connection to a protocol:

`system/lib/ioservice/test/server_test.cpp:289-297`:

```cpp
TEST_F(IoserviceServer, OpenChecksConnectForProtocolService) {
    auto const no_connect = bind(m_svc, Operations::Enumerate | Operations::Traverse);
    EXPECT_EQ(refused(no_connect, "echo", Flags::ProtocolService), Status::AccessDenied);
    // Before the lookup: a missing name answers the same.
    EXPECT_EQ(refused(no_connect, "nope", Flags::ProtocolService), Status::AccessDenied);

    auto const svc = bind(m_svc, ours_io::R_STAR_DIR);
    EXPECT_EQ(refused(svc, "nope", Flags::ProtocolService), Status::NotFound);
}
```

Read it as a table. A connection to the `/svc` directory that holds the ours.io rights Enumerate and Traverse but not Connect (rights of a connection, a different set from the handle rights of chapter 1) is refused with `AccessDenied` for a name that exists and for one that does not, because the right is checked before the name is looked up. With the right, a name that is not there is `NotFound`. `refused()` reads the status back from the epitaph the server wrote on the new channel. The assertions on a missing name, with and without the right, also pin the order: a server that looked the name up first would answer `NotFound` to the second assertion, and the test would fail.

The same refusal, through a real Loop and the fake kernel, also checks what was not done:

`system/lib/ioservice/test/loop_server_test.cpp:335-345`:

```cpp
TEST_F(LoopServerTest, OpenOfAMissingNameAnswersNotFound) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    open(directory, "missing", Flags::ProtocolService, service_end);
    run();

    EXPECT_TRUE(calls().connects.empty());
    EXPECT_EQ(epitaph(client), Status::NotFound);
    EXPECT_FALSE(fake::kernel().is_open(service_end));
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
}
```

Four things are pinned. No connect function was called. The client end holds an epitaph with `NotFound`. The channel the Open carried is closed. The directory's own connection is still open for the next request. A server that answered `NotFound` but also dropped the directory connection would pass the first three lines and fail the last. `system/lib/ioservice/test/outgoing_test.cpp:206-219` does the same for the provider side: an Open of an `svc/<name>` the outgoing directory does not serve is answered `NotFound`.

### Adding a host test of your own

A host test takes four things: the test, an executable that builds it, a `tests` group that names the executable, and a line in the default build that names the group. The last is the one that is easy to forget.

**The test.** Write it with googletest's `TEST` or `TEST_F`, in a file under the library's `test/` directory.

**The executable.** Declare it in the `BUILD.gn` of that `test/` directory. `system/lib/loop/test/BUILD.gn` is the smallest real one:

`system/lib/loop/test/BUILD.gn:8-21`:

```gn
executable("loop") {
  testonly = true
  sources = [
    "fake_kernel.cpp",
    "fake_kernel.hpp",
    "loop_test.cpp",
  ]
  deps = [
    "//system/lib/loop",
    "//vendor/googletest:gtest",
    "//vendor/googletest:gtest_main",
  ]
  visibility = [ "*" ]
}
```

**The `tests` group.** Name the executable in the library's `tests` group, with the host toolchain's label:

`system/lib/loop/BUILD.gn:28-31`:

```gn
group("tests") {
  testonly = true
  public_deps = [ "test:loop($host_toolchain)" ]
}
```

**The default build.** Name the group in the `deps` of `group("default")` in the root `BUILD.gn`:

`BUILD.gn:220`:

```gn
    "//system/lib/loop:tests",
```

A `tests` group is not found by anything on its own: a test that is not reachable from `//:default` is not built by the default build and not found by `scripts/host_tests.py`, so it is never run, and the boot check says nothing. [03-protocols.md](03-protocols.md) scaffolds a library with its test target; the wiring above is what makes that test run.

To see whether your test is found, ask GN what the rule asks (`prebuilt/gn/gn ls out/bringup.x86 --testonly=true --type=executable`) and look for your label ending in `(//build/toolchain/ours:host)`, or watch the count of the `host-tests` line rise by one.

Where this is stated:

- How a test is found: `scripts/host_tests.py:4-13`, `:22`, `:24` and `:44-50`; `build/config/BUILDCONFIG.gn:159` (the host toolchain); `BUILD.gn:12` (`//:default`).
- The boot check runs each test from the build directory and passes it on exit 0: `.agents/skills/ours-verify/scripts/boot_check.py:214-221`.
- `--gtest_filter` and `--gtest_list_tests`: googletest's own documentation, `vendor/googletest/docs/advanced.md`.

## Tests in a boot

### Boot the dev image

Build and boot as [README.md](README.md#build-and-boot-the-dev-image) says, from the root of the repository, once the tools of [00-setup.md](00-setup.md) are in place. The command is the verify script:

```bash
python3 .agents/skills/ours-verify/scripts/boot_check.py
```

When you are checking a server, boot only the dev image with `--only rel`. The dev image is `kernel.dev.obi`, which the script calls `rel`: it runs `init`, the program of the root, and the root starts the example components. The other image, `kernel.dev.dbg_1.obi`, is the debug image, which the script calls `dbg`. It starts the shell with no root, so it has no routing and none of the example components (`.agents/skills/ours-verify/scripts/boot_check.py:86-87`).

A boot passes when all of these hold (`.agents/skills/ours-verify/scripts/boot_check.py:15-20`):

- the shell printed its closing `--- end ---` line;
- every line the image requires appeared. For `rel` there are six, which the `rel` line of the output counts as `6/6 required`: the two that this guide's example produces, `routing_checks: passed` and `init: echo exited with 0`, and four that other components of the image print (the root's line that it holds the interrupt and I/O port resources, and three from the keyboard driver);
- no line has the word `FAILED` in it;
- nothing logged at error level, which a panic and a failed assertion both do.

If the check fails, its summary says why. The summaries that concern a server are these; the verify script's documentation, named in [the README](README.md#build-and-boot-the-dev-image), has the full table:

| The summary says | It means | What to do |
|---|---|---|
| `no "<line>" within Ns` | the shell finished, but a line the image requires never came, such as `routing_checks: passed` | read the `init:` lines in `rel.log`: a component that did not start, or a use that routes nowhere or cannot be served, is logged there |
| `<N> FAILED line(s)` | a check printed a line with `FAILED` in it | `grep -a FAILED` in the log: the line names the check, the status it got and the one it expected |
| `error-level line` | a panic or a failed assertion | read the context the script prints; a failed assertion shows on `dbg`, which compiles assertions in, and may not on `rel` |
| `host-tests FAIL` | a host test exited with a non-zero status, or GN could not list them | the failing program's last lines are printed; run its binary alone, as above |
| `no "--- end ---" within Ns` or `console silent for Ns` | the boot hung | see "Debugging further" |

### Read the boot log

The last line of the output names the directory of the logs, `<dir>/{dbg,rel}.log`. The directory is `ours-verify-<name of the build directory>-<ten hex digits>` under the temporary directory (`.agents/skills/ours-verify/scripts/boot_check.py:560` and `:588`; `scripts/boot_logdir.py:30-31`). The digits come from the whole path of the build directory, so two checkouts of the repository that both use `out/bringup.x86` have directories of their own. `host-tests.log` is beside them.

A log holds the kernel's lines and the shell's self-checks as well, and can hold a stray NUL byte that makes `grep` treat it as a binary file, so search it for markers with `grep -a`. The root's lines start with `init:`, and a server's start with whatever prefix it prints, as echo's start with `echo:`:

```bash
grep -a -n 'init:\|echo:\|routing_checks:' <dir>/rel.log
```

On a passing boot that command prints lines like these, in this order (the `/boot` counts can differ, and the lines for the shell and the keyboard driver are left out here):

```text
init: started with the root job
init: holding the IRQ and I/O port resources
init: /boot has 12 files, 6 executable
init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")
init: started routing_checks
init: started echo
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
init: launch failed: mapping the program's file into this process: AccessDenied
echo: stopping, as a client asked
init: echo exited with 0
routing_checks: passed
init: routing_checks exited with 0
```

Two kinds of line in it look like failures and are not. The two `routes nowhere` lines are the refusals that the next section sets up, and the `launch failed` line is `routing_checks` asking the Launcher (`ours.process.Launcher`, which starts programs) to start something it must refuse. A failure has `FAILED` in it.

A server prints with `ort::debug_line`, which writes one line to the kernel's debug console, which the scripts read from QEMU's serial port (`system/lib/ort/include/ort/debug.hpp:16-40`; `scripts/qemu_boot.py:265`). The runtime has no formatted printing: the C library a program links is only the memory and string functions, so a line is built from pieces, and a number goes through `ort::decimal` (`system/lib/ort/include/ort/debug.hpp:45-52`). A line longer than one system call may carry can be split by another program's output, so keep a line short (`system/lib/ort/include/ort/debug.hpp:38-39`).

`examples/echo/echo_server.cpp` shows the habit worth copying: one `report(what, status)` function that prints `echo: <what>: <Status>`, called wherever the server gives up (`examples/echo/echo_server.cpp:63-66`). With the program's name first, `grep` finds every line of one component, and with the status last the line says why.

The root prints two lines about every component's life. It starts the children of `root.cm` in the order they are listed (`system/service/init/realm.cpp:444-455`):

- `init: started <moniker>` when it started the component's process.
- `init: <moniker> exited with <code>` when the process ended. A server meant to keep running that has this line has stopped, and the lines it printed just before are the first place to look.

If there is no `init: started <your component>` line, the component did not start; look for `init: cannot resolve`, `init: cannot start` and `init: launch failed` above its place in the log.

To make a boot depend on your server, give a component the job of connecting to it and checking what it answers, the way `routing_checks` does; [02-first-server.md](02-first-server.md) shows a client's exchange with echo. Have that component print a line when all of its checks passed, and add the line to the `REQUIRED` list of the `rel` image (`.agents/skills/ours-verify/scripts/boot_check.py:90-100`). Then a server that does not start, or does not answer, fails the check.

### How `routing_checks` makes a negative case

`routing_checks` is a component the root starts as one of its children. Its job is to try things that must be refused, and to fail loudly when one is not. A negative case there has four parts.

First, the manifest asks for something that must fail. The protocol uses of `routing_checks` include `ours.test.Unoffered` (`system/service/init/test/routing_checks/BUILD.gn:82`), which the root's manifest does not offer to it. The directory `denied` is made in the same way, with rights: the use asks for `rights = [ "rx*" ]`, and the offer in the root's manifest gives only `rights = [ "r*" ]`, so the route narrows the wrong way (`kernel/products/image/dev/BUILD.gn:132-139`). `r*` and `rx*` are the manifest's rights words: `r*` stands for the ours.io rights that read a directory, and `rx*` for those plus Execute ([01-concepts.md](01-concepts.md#routes)).

Second, the program connects and reads what comes back, and expects the failure. A connect is one-way, so it succeeds before the root has decided anything, and the refusal shows up as the first message on the channel:

`system/service/init/test/routing_checks/routing_checks.cpp:531-544`:

```cpp
    auto const connection = connect_or_fail(UNOFFERED);
    if (connection == Uandle::Invalid) {
        return;
    }
    Incoming closed;
    if (expect("the epitaph on an unoffered protocol", closed.receive(connection), Status::Ok)) {
        auto const epitaph = closed.epitaph();
        if (epitaph.is_none()) {
            fail("an unoffered protocol answered something other than an epitaph");
        } else {
            expect("an unoffered protocol", *epitaph, Status::NotFound);
        }
    }
    auto _ = ours::handle_close(connection);
```

Notice that the check is exact on both counts. It requires an epitaph, not a bare close, and it requires `NotFound`, not any error.

Third, a failed check prints a line with `FAILED` in it (`system/service/init/test/routing_checks/routing_checks.cpp:140-144`), and the program ends by printing `routing_checks: passed` and exiting with 0 only if none failed (`system/service/init/test/routing_checks/routing_checks.cpp:920-927`). The root logs the exit as `init: routing_checks exited with 0`.

Fourth, the boot check turns those lines into a verdict. Any `FAILED` line fails the image, and `routing_checks: passed` is among the required lines, so a component that never runs, or stops before the end, fails too.

The root prints a line for each deliberate refusal, and a passing boot holds them. In your `rel.log` you will see these four. The first two are the ones the first part sets up; the third is `check_undeclared`'s, which asks for a name the manifest does not use (cause 2); the fourth is the route to echo after echo has been stopped on purpose (cause 4):

```text
init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
init: routing_checks's /svc refuses an open of "ours.test.Undeclared": NotFound
init: routing_checks's use of "ours.test.Echo" routes nowhere: NotFound at echo (exited)
```

A negative case you have never seen fail proves nothing, because a check that cannot fail looks the same as one that passes. Break the thing it checks, reboot, and watch it fail, then restore it. For example, change the status that `check_unoffered` expects, `Status::NotFound` in the snippet above, to `Status::Ok`, and boot: the check prints a line of the shape `routing_checks: <what> = <got> (expected <expected>) FAILED`, which `fail()` builds, and `rel` fails with a `FAILED line(s)` summary. A check that prints nothing when broken guards nothing.

### The kernel shell and `kshell_run.py`

The kernel has a shell of its own, with commands that test the kernel, such as `vmosnapshot.test`. They do not run at boot. Only the `dbg` image has this shell, since the user shell holds the serial port on `rel` (`scripts/kshell_run.py:9-11`). `scripts/kshell_run.py` boots the `dbg` image in `--out`, types the commands you name once the boot ends, and exits with 0 if each passed (`scripts/kshell_run.py:1-35`). You need it when you change the kernel. A server is checked with the verify script.

## What went wrong: what a client sees

### How a client learns the outcome

Opening a channel to a protocol is one-way. `fsio::connect` writes an `Open` for `/svc/<name>` and returns without waiting, so it answers `Ok` before the root has routed anything (`system/lib/fsio/include/fsio/fsio.hpp:52-60`):

`system/lib/fsio/include/fsio/fsio.hpp:87`:

```cpp
auto connect(std::string_view path, ours::Uandle server) -> ours::Status;
```

The result arrives on the channel. The client reads its end and finds one of three things:

- an epitaph ([01-concepts.md](01-concepts.md#epitaphs)). Its status is the reason the connect was refused. The server wrote it as the channel's last message and closed;
- nothing, and `PeerClosed`. The other end closed without saying why;
- ordinary protocol messages. The channel works.

A client has to wait for the channel to be readable or its peer to be closed, and only then decide which of the first two it has. `routing_checks` does it in one small function, and every check above is built on it. The whole exchange of a client with echo, from encoding a request to decoding the reply, is in [02-first-server.md](02-first-server.md); this is the part that tells the outcome of a connect:

`system/service/init/test/routing_checks/routing_checks.cpp:346-360`:

```cpp
    auto receive(Uandle const channel) -> Status {
        auto observed = ours::Signals::None;
        if (auto const status =
                ours::object_wait_one(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
            status != Status::Ok) {
            return status;
        }
        if (!(observed & ours::Signals::Readable)) {
            return Status::PeerClosed;
        }
        return ours::channel_read(channel, ours::ChannelReadOptions::None, m_bytes.data(),
                                  m_handles.data(), static_cast<u32>(m_bytes.size()),
                                  static_cast<u32>(m_handles.size()), &m_num_bytes, &m_num_handles);
    }
```

A message that is readable may be an epitaph or a reply: the client looks at the header before it decodes (`system/lib/wire/include/wire/wire.hpp:285-295`). Two details matter when you read a client's report:

- A write on a channel whose peer closed answers `PeerClosed`, but the epitaph the peer left is still there to read, so a client that writes first and then reads still gets the reason (`system/lib/ours.resource/src/client.cpp:28-29`).
- An epitaph whose status is `Ok` says only that the server closed with nothing wrong, and the client libraries read it as `PeerClosed` (`system/lib/ours.resource/src/client.cpp:75-79`).

### Four questions, in order

When a connect fails, ask these in order. The causes are numbered in "The table" below.

1. Did `fsio::connect` itself return an error? Then no message was sent. Cause 1 or 9.
2. If it returned `Ok`, what did the first read give? An epitaph: its status picks the causes. No epitaph, `PeerClosed`: causes 11 to 13.
3. Does the root's log hold a line for it? Search for `use of "<name>"`, `refuses an open of "<name>"`, `cannot forward` and `cannot serve`.
4. If the root's log is silent, who else could have answered? The provider. Its library prints one `ioservice:` line when its outgoing directory refuses an Open (cause 5); its own program prints what it prints (causes 10 and 13). Cause 12 is a race that leaves no line.

### The table

Each cause has a number. Find the client's symptom in the first column of the lookup table, compare the root's line in the second, and read the causes the third column names. The root's lines have the shape shown; `<Status>` is the name of a status, such as `NotFound`; `<moniker>` is a component's path from the root, which [01-concepts.md](01-concepts.md#routes) defines.

| The client sees | The root's log has | Causes |
|---|---|---|
| `fsio::connect` returns `NotFound`, nothing sent | nothing | 1 |
| `fsio::connect` returns `NoResource`, for every protocol of that component | `init: <moniker>'s /svc cannot be served: <Status>` | 9 |
| an epitaph `NotFound` | `... routes nowhere: NotFound at <where> (<verb> of "<name>")` | 3 |
| an epitaph `NotFound` | `init: <moniker>'s /svc refuses an open of "<name>": NotFound` | 2 |
| an epitaph `NotFound` | `... routes nowhere: NotFound at <provider> (exited)` | 4 |
| an epitaph `NotFound` | nothing from the root; the provider logs `ioservice: the outgoing directory refused an Open of "svc/<name>": NotFound` | 5 |
| an epitaph `AccessDenied`, on a directory entry only | `... routes nowhere: AccessDenied at <where> ...` | 6 |
| an epitaph `BadType` | `... reaches another protocol: ...` | 7 |
| an epitaph `NoResource` | `init: cannot serve <name> to <moniker>: <Status>` | 8 |
| an epitaph `NoResource` | nothing | 10 |
| `PeerClosed`, no epitaph | `init: cannot forward <name> to <provider>: <Status>` | 11 |
| `PeerClosed`, no epitaph | nothing | 12, 13 |

The causes, each with what to change:

1. **`fsio::connect` returns `NotFound`; the root logs nothing.** The component has no protocol `use` at all, so the root made it no `/svc`, and the client's namespace has nothing to resolve `/svc/<name>` against. Fix: declare the protocol in the component's `use` list.
2. **An epitaph `NotFound`; the root logs `init: <moniker>'s /svc refuses an open of "<name>": NotFound`.** `<name>` is not in this component's `/svc`, which holds one entry per protocol `use`. Fix: spell the name as the `use` does, and add the `use` to the component's manifest if it is missing. The name in the line is the path as the client sent it, with every byte that is not printable ASCII shown as `?`, and a long path cut and ended with `...`: a client chooses those bytes, and they must not be able to end the line and write another of the root's.
3. **An epitaph `NotFound`; the root logs `init: <moniker>'s use of "<name>" routes nowhere: NotFound at <where> (<verb> of "<name>")`.** The route leads nowhere: the parent offers nothing of that name to this component, a child that should provide it exposes nothing of that name or cannot be resolved, the provider has no program or declares no such capability, or no built-in (a protocol the root itself provides) has the name. Fix: add what the line says is missing, read as the notes below explain: the `offer` at `<where>`, the `expose` of the child, or the capability in the provider's manifest.
4. **An epitaph `NotFound`; the root logs `init: <moniker>'s use of "<name>" routes nowhere: NotFound at <provider> (exited)`, and an earlier line `init: <provider> exited with <code>` or `init: cannot start <provider>: <Status>` is there.** The provider is not running: it exited, or could not be started, and the root does not start it again, so every later route to it ends here. The line has cause 3's shape, with the provider after `at` and `(exited)` where cause 3 has the verb. Fix: find why it stopped, in the lines it printed before the `exited with` line. A provider that stops on purpose, as echo does when it is asked to, gives the same answer afterwards: `routing_checks` makes that connect on purpose, and every dev boot holds `init: routing_checks's use of "ours.test.Echo" routes nowhere: NotFound at echo (exited)`. The code has a second wording, `(its outgoing directory is closed)`, for a provider whose outgoing directory is gone while its process has not been seen to end; no boot here printed it.
5. **An epitaph `NotFound`; the root logs nothing, and the provider logs `ioservice: the outgoing directory refused an Open of "svc/<name>": NotFound`.** The provider runs, but its outgoing directory has no `svc/<name>`: its manifest declares the protocol and its program never added it under that name. The root forwards the Open and does not wait, so only the provider's directory answers, and it says so with that one line. The line has no program name, since the library does not know it and the console adds none; a boot holds none until a name goes wrong, so a search for `ioservice:` finds it. For `hello` with `HELLO_NAME` spelled `ours.examples.hello.Hallo` in the header the log held `ioservice: the outgoing directory refused an Open of "svc/ours.examples.hello.Hello": NotFound`, once for each Open. Fix: add it in the program under the name the manifest declares, as echo's `main` does with `add_protocol`.
6. **An epitaph `AccessDenied`, on a directory entry only; the root logs `init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")`.** A directory `use` asks for rights that a hop of its route does not give, such as `rx*` against an offer of `r*`. A protocol use states no rights, so a protocol never meets this. The root closed the entry before the component ran, and the first `fsio::open` under it reads the epitaph and keeps it. Fix: give the offer the rights the use asks for, or ask for fewer.
7. **An epitaph `BadType`; the root logs `init: <moniker>'s use of "<name>" reaches another protocol: it wants <want>, the route ends at <got> (<where>)`, where `<where>` is `the built-ins` or `provided by <moniker>`.** The route was found, and it ends at a capability of another protocol than the use means. The names agree and the protocols differ, usually after an offer renamed the capability with `as`. The build refuses a mismatch it can see, so this appears when the type check is off or the image was not built from these manifests. Fix: offer a capability of the protocol the use means, under that name, or give the use the `type` it means.
8. **An epitaph `NoResource`; the root logs `init: cannot serve <name> to <moniker>: <Status>`.** A protocol the root itself provides has no free place for another client: the Launcher and each resource protocol serve four clients at a time. Fix: close the channel to it once you have what you asked for.
9. **`fsio::connect` returns `NoResource`, for every protocol of that component; the root logs `init: <moniker>'s /svc cannot be served: NoResource`.** The root could not make the component's `/svc`. The plainest cause is more than 15 protocol uses in its manifest; the status in the log line says which. The root closed the entry with an epitaph, and the client's first call reads it and keeps it. Fix: keep the component's protocol uses to 15.
10. **An epitaph `NoResource`; the root logs nothing, and the provider prints only what its program prints.** The provider's own table is full: it has no free place for another client. Echo keeps four. Fix: size the table for the clients you expect (`MAX_CLIENTS` in `examples/echo/echo_server.cpp`), and have clients close channels they are done with.
11. **`PeerClosed`, no epitaph; the root logs `init: cannot forward <name> to <provider>: <Status>`.** The root could not forward the Open. Its write into the provider's outgoing directory failed, because that channel already holds 64 unread messages (`NoResource`) or its other end is gone (`PeerClosed`). A failed write still consumes the handles, so the client's channel, which the Open carried, closes with nothing written on it. Fix: make the provider read its outgoing directory: serve it, and keep every `on_signals` short so that the Loop gets back to it.
12. **`PeerClosed`, no epitaph; the root logs nothing.** The Open reached a provider that was exiting and was lost with it. A race, rare, and the next connect gets `NotFound`. Fix: connect again, once, as `routing_checks` does.
13. **`PeerClosed`, no epitaph; the root logs nothing.** The server closed the channel on a message it does not accept: a method number it does not define, or a message its decoder refuses. A closed protocol does this on purpose ([03-protocols.md](03-protocols.md#closed-protocols)), and so do the servers of the protocols the root itself provides, for a message of another protocol. Fix: call a method the protocol defines, with a request its decoder accepts: compare your request's bytes with the protocol's message layout.

Notes on the causes:

- Cause 3's `<where>` is the moniker of the instance whose declaration the walk stopped at, or `the built-ins` when it stopped above the root. `<verb>` is `offer`, `expose`, `declaration` or `use`. `at .` means the root: the parent had no offer. Read the verb and the name together. For `NotFound`, `offer of "X"` at the parent `P` means `P`'s manifest has no offer of `X` to this component; the same words after `AccessDenied` mean the offer exists and gives too little.
- A client that built its own Open by hand meets two more statuses on a `/svc`. A flag other than `PROTOCOL_SERVICE` on a service is `InvArgs`, and a right the `/svc` connection does not hold is `AccessDenied`, from the Open's checks, in their fixed order, and the root logs each of them as it logs cause 2, with the status after the name. `fsio::connect` sends `PROTOCOL_SERVICE` alone, so it does not meet them. The order of the checks decides which error a request that is wrong in several ways gets, and [04-tree-servers.md](04-tree-servers.md) walks through it.
- A channel that worked and then reads `PeerClosed` is not a refusal: the server exited or released it. `routing_checks` expects exactly that after it asks `echo` to stop.
- The statuses are the names `ours::to_string` gives, and the same names appear in the root's lines.

Where each cause is produced, for when you read the code:

- Cause 1: `system/lib/fsio/include/fsio/namespace.hpp:124`; the root makes a `/svc` only for a component with a protocol use, `system/service/init/realm.cpp:567`.
- Cause 2: the lookup, `system/lib/ioservice/src/tree.cpp:204`; the refusal, `system/lib/ioservice/src/server.cpp:538-541` and `:95-110`; written on the client's channel, `system/lib/ioservice/src/loop_server.cpp:177-195`; the entries, `system/service/init/realm.cpp:723-745`; the line, `system/service/init/realm.cpp:753-776`, which the hook `Delegate::open_refused` reaches (`system/lib/ioservice/include/ioservice/loop_server.hpp:84-85`, called at `system/lib/ioservice/src/loop_server.cpp:191`).
- Cause 3: the status, `system/lib/component/include/component/route.hpp:412-415`, `:427-430`, `:444-451` and `:458-464`, listed at `:307-317`; the reply, `system/service/init/realm.cpp:792-796`; the line, `system/service/init/realm.cpp:217-219`.
- Cause 4: the check and the line, `system/service/init/realm.cpp:838-848`; the state is set at `system/service/init/realm.cpp:436` and `:917`, and the lines are printed at `system/service/init/realm.cpp:431` and `:910-911`.
- Cause 5: `system/lib/ioservice/src/tree.cpp:204` in the provider's own node tree; the line, `system/lib/ioservice/src/outgoing.cpp:99-115`; `system/lib/ioservice/test/outgoing_test.cpp:206-219`; `system/service/init/realm.cpp:861-885`.
- Cause 6: `system/lib/component/include/component/route.hpp:271-282` and `:318`; `system/service/init/realm.cpp:530-551`; the client, `system/lib/fsio/include/fsio/fsio.hpp:66-74`.
- Cause 7: `system/lib/component/include/component/route.hpp:301-305`, `:421-423` and `:465-467`; the reply, `system/service/init/realm.cpp:795`; the line, `system/service/init/realm.cpp:204-215`.
- Cause 8: `system/service/init/launcher_server.cpp:83`, `system/service/init/resource_server.cpp:59`, `system/service/init/pools.hpp:32` and `:46`; `system/service/init/realm.cpp:822-827`.
- Cause 9: `system/service/init/realm.cpp:716-718` and `:567-579`; `system/service/init/pools.hpp:55`; the client, `system/lib/fsio/include/fsio/fsio.hpp:66-74`.
- Cause 10: `examples/echo/echo_server.cpp:54` and `:168`; when the Loop cannot take the channel, `examples/echo/echo_server.cpp:159-165`.
- Cause 11: `system/service/init/realm.cpp:877-885`; the system call, `system/public/ours/syscalls.gni:197-208`; the limit, `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28`.
- Cause 12: `system/service/init/test/routing_checks/routing_checks.cpp:818-838`, which retries once.
- Cause 13: `examples/echo/echo_server.cpp:207-226`; `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40`; `system/lib/ioservice/src/server.cpp:570-574`.
- The verbs and the `at` field of cause 3: `system/service/init/realm.cpp:172-184` and `:198-203`. The Open checks behind the hand-built Open: `system/lib/ioservice/src/server.cpp:257-259` and `:344-346`, and `system/lib/ioservice/test/server_test.cpp:641-645`.

### Other lines the root prints

| The line | What it says |
|---|---|
| `init: started with the root job` | the root started |
| `init: holding the IRQ and I/O port resources` | it holds the roots of the interrupt lines and of the I/O ports |
| `init: /boot has <N> files, <M> executable` | it ingested the boot files |
| `init: cannot resolve meta/root.cm = <Status>` | the root manifest could not be read; the root halts |
| `init: cannot resolve <moniker>: <Status>` | a child's manifest could not be read or decoded, or the instance pool is full |
| `init: cannot start <moniker>: <Status>` | the program could not be run; routes to it answer `NotFound` from then on |
| `init: launch failed: <what>` | a step of loading a program failed |
| `init: cannot watch <moniker>: <Status>` | the root could not wait for the process to end |
| `init: /boot cannot answer a client: <Status>` | a reply of the root's own file server could not be written |
| `init: <moniker>'s /svc refuses an open of "<name>": <Status>` | an Open on a component's `/svc` was refused (cause 2 for `NotFound`) |

The lines about a component's life (`started`, `exited with`) are described in "Read the boot log", and the lines of causes 2 to 4, 8, 9 and 11 in the list of causes. A provider's library prints one more kind of line, `ioservice:` (cause 5); it is not the root's. The root prints more than these; `grep -rn '"init: ' system/service/init` lists them all, with the file and line of each. The ones above are printed by `system/service/init/init.cpp` and `system/service/init/realm.cpp`.

### Two cases worked through

**A route to a protocol nobody offers.** This is cause 3, and the line is the second of the pair in "How `routing_checks` makes a negative case": `init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")`. Read it as: the use is `routing_checks`'s, the walk stopped at the root (`.`), and what was missing was an offer of that name from the root to `routing_checks`. The fix is an `offer` in the parent's manifest, to this child, under that name. If the line is not there while the client got `NotFound`, look for the other lines that end in `NotFound`: cause 2's `refuses an open` and cause 4's `(exited)` are the root's, and cause 5's `ioservice:` line is the provider's. The four questions above say in which order to look.

**A route that ends at the wrong protocol.** This is cause 7, and its line names the two protocols that were mixed up. Suppose the use of `ours.test.Echo` in `routing_checks` asked for the type `ours.examples.echo.Echo`, and the root's offer under that name had given it a capability of the Launcher. The line would carry `it wants ours.examples.echo.Echo, the route ends at ours.process.Launcher (the built-ins)`. The names agree; the protocols do not, and the client reads `BadType`. The fix is in the manifest: offer a capability of the protocol the use means, under that name, or give the use the `type` it means.

## Debugging further

- **A boot that hangs or goes silent.** `boot_check.py` sends QEMU an `nmi` after 15 seconds without a line, and the kernel prints a report of what every CPU was doing and panics (`.agents/skills/ours-verify/scripts/boot_check.py:27-39` and `:125`). The report is kept in the log and printed under the verdict. The verify script's documentation, named above, explains how to read it.
- **A debugger.** `docs/development/debugging.md` describes starting the machine under the debugger from the editor with F5. It is written in Chinese, and this guide does not copy it. The verify script's documentation links an English note on attaching `lldb` to a machine halted before its first instruction, setting breakpoints and inspecting the machine. The one-click session and the manual one both start the machine through `scripts/qemu_debug.py` (`scripts/qemu_debug.py:1-9`). Both are written for the kernel; this guide has not tried them on a server's own program.
- **Your own lines.** The most direct debugging of a server is a line at each place it gives up, in the style above, and a boot of the dev image (`--only rel`).

## Where to look next

- [02-first-server.md](02-first-server.md): the whole exchange of a client with echo, and a server built stage by stage with its tests.
- [06-checklist.md](06-checklist.md): the checks to run before a server is called done, grouped by the phase you are in, with the test and boot items among them.
- [03-protocols.md](03-protocols.md#closed-protocols): why an unknown method closes the channel with no epitaph, and what that costs the client.
- [04-tree-servers.md](04-tree-servers.md): the Open checks a tree server runs, in order, which decide the statuses of causes 2 and 6.
- `docs/development/debugging.md`: the debugger, in Chinese.

## Sources

- `scripts/host_tests.py`, `scripts/kshell_run.py`, `scripts/qemu_boot.py`, `scripts/qemu_debug.py`, `build/config/BUILDCONFIG.gn`, `BUILD.gn` (the root)
- the verify script, `.agents/skills/ours-verify/scripts/boot_check.py`, and the documentation beside it
- `system/lib/ioservice/` (`README.md`, `BUILD.gn`, `src/server.cpp`, `src/tree.cpp`, `src/loop_server.cpp`, `src/outgoing.cpp`, `test/BUILD.gn`, `test/fake_kernel.hpp`, `test/fake_kernel.cpp`, `test/server_test.cpp`, `test/loop_server_test.cpp`, `test/outgoing_test.cpp`)
- `system/lib/loop/` (`README.md`, `BUILD.gn`, `include/loop/loop.hpp`, `test/BUILD.gn`, `test/fake_kernel.hpp`, `test/loop_test.cpp`)
- `system/service/init/` (`realm.cpp`, `init.cpp`, `pools.hpp`, `launcher_server.cpp`, `resource_server.cpp`, `BUILD.gn`, `test/routing_checks/BUILD.gn`, `test/routing_checks/routing_checks.cpp`)
- `kernel/products/image/dev/BUILD.gn`, `examples/echo/` (`echo_server.cpp`, `BUILD.gn`, `protocol/`)
- `system/lib/component/include/component/route.hpp`, `system/lib/fsio/include/fsio/` (`fsio.hpp`, `namespace.hpp`), `system/lib/ours.resource/src/client.cpp`, `system/lib/wire/include/wire/wire.hpp`, `system/lib/ort/include/ort/debug.hpp`
- `system/public/ours/vdso.hpp`, `system/public/ours/syscalls.gni`, `kernel/object/include/ours/object/channel_dispatcher.hpp`, `kernel/tests/fake-kernel/README.md`
- `docs/architecture/component-framework/glossary.md`, `docs/development/debugging.md`, `vendor/googletest/docs/advanced.md`
- a real `rel.log` of a passing boot, for the lines quoted in "Read the boot log"

Checked against commit `10dd8149` on 2026-10-30.
