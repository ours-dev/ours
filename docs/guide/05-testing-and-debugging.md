# Testing and debugging a server

A server is checked in two places. Most of its logic can be tested on the machine you build on, quickly, without starting an image. What only the real kernel and the real root can decide, such as whether a client can reach the server at all, is checked in a booted image. This chapter shows both, and ends with a table to keep open while you debug: for each way a connection to a protocol can fail, what the client sees, what the root prints, and the code that produced each.

## What you will be able to do

- Decide which part of a server is tested on the host and which part waits for a boot.
- Find, build and run the host tests, and add one of your own.
- Boot the dev image, find your server's lines in its log, and tell a passing boot from a failing one.
- Write a negative case, a test that passes only when a request is refused for the right reason, the way `routing_checks` does.
- Read a failed connection from the status the client got and the line the root logged.

## Words used in this chapter

The kernel objects (handle, channel, port, signals, Loop, slot) are defined in [01-concepts.md](01-concepts.md#handles-and-channels), [ports and signals](01-concepts.md#ports-and-signals) and [the Loop and its slots](01-concepts.md#the-loop-and-its-slots). The component words (component, manifest, use, offer, expose, protocol type) are defined in [01-concepts.md](01-concepts.md#components-and-manifests) and in `docs/architecture/component-framework/glossary.md`. The words below are the ones this chapter adds.

- *host*: the machine you build on. The *target* is the machine an image runs on, which here is the emulated machine that QEMU, an emulator the scripts start, provides.
- *host test*: a test program built for the host and run there directly. It uses googletest, a C++ test library: the program runs every test it holds and exits with 0 only if all of them pass.
- *system call* and *vDSO*: a system call is how a program asks the kernel for something, such as reading a channel. A user program does not issue the instruction itself. It calls a function of the vDSO, a small library the kernel maps into every process, and that function traps into the kernel on its behalf (`system/public/ours/vdso.hpp:6-14`).
- *fake kernel*: a stand-in for the kernel's channel and port calls that follows only the rules a test needs. The section "What a fake kernel is here" defines it exactly.
- *negative case*: a check that passes only when a request is refused, and only for the reason you expect.
- *dev image*: the bootable image that holds the root, the example components and the shell. It comes in two builds, called `rel` (`kernel.dev.obi`) and `dbg` (`kernel.dev.dbg_1.obi`); only `rel` has the root (`.agents/skills/ours-verify/scripts/boot_check.py:66-73` and `:81-89`).
- *root* and *moniker*: the root is `init`, the first user program. It starts the components, and it is the only place a connection is routed. A moniker is an instance's path in the component tree, written as its names from the root down joined with `/`, with `.` for the root itself. Logs use it (`system/service/init/realm.cpp:113-114`).
- *route*: the walk the root makes from a component's `use` up through its parents' offers and down through its children's exposes to the component that provides the capability. `route()` in `system/lib/component/include/component/route.hpp` does it, and the root runs it again on every connection.
- *epitaph*: the last message written on a channel before it is closed, saying why. [01-concepts.md](01-concepts.md#epitaphs) defines it. A channel can also close with no epitaph, and then the other end reads only `PeerClosed`.
- *client*: here, a component that connects to a protocol through its `/svc`.

## Two places to test

Which place a part of a server is tested in follows from whether it needs the kernel to be right.

| What you want to check | Where | How |
|---|---|---|
| The bytes of a message: encoding, decoding, what a decoder refuses | host | the codec makes no system call (`examples/echo/protocol/BUILD.gn:3-7`), so its test feeds it bytes: `examples/echo/protocol/test` |
| What a server decides: which answer a message gets, which connections it holds, what it refuses | host | `ioservice::Server` takes one message and returns what to do, and makes its one system call through a function it is given (`system/lib/ioservice/README.md:12`): `system/lib/ioservice/test/server_test.cpp` |
| A server on a Loop: what it reads, writes and closes | host, with a fake kernel | `system/lib/ioservice/test/loop_server_test.cpp`, `outgoing_test.cpp`, and `system/lib/loop/test/loop_test.cpp` |
| Whether the manifests route, the process starts, and a client reaches the server | a boot of the `rel` image | `routing_checks`, the root's test component |

The example server `examples/echo/echo_server.cpp` has no host test of its own. Its BUILD.gn asserts a user toolchain (`examples/echo/BUILD.gn:13-15`), so its program is built for the image only, and its behaviour is checked there: `routing_checks` sends it a string, asks it to stop, and checks that the next connect is refused (`system/service/init/test/routing_checks/routing_checks.cpp:38-44`). The parts it is made of are tested on the host: its codec, and `ioservice::Outgoing`, which holds its protocol.

Keep the same split in a server of your own. Put what you want to test on the host in a library target that builds in the host toolchain, and keep the program for the image. The root does this for the splitting of its `ours.autorun.boot` option: `:autorun` is a target of its own "so that the host test reaches it without init" (`system/service/init/BUILD.gn:12-15`).

## Tests on the host

### How the host tests are found, built and run

The rule is one short Python module, `scripts/host_tests.py`, shared by every script that runs tests, so a test is run by all of them or by none.

1. A host test is an executable that GN, the tool that generates the build files, marks `testonly = true` and builds in the host toolchain, `//build/toolchain/ours:host` (`scripts/host_tests.py:22`, `build/config/BUILDCONFIG.gn:159`). `testonly` is GN's own mark: only another `testonly` target may depend on such a target. It is not a label that says "test", so benchmarks carry it too, and `//benches` is left out because a benchmark reports timings and does not pass or fail (`scripts/host_tests.py:4-9` and `:24`).
2. Of those, only the ones `//:default` builds count. `//:default` is the target the build makes when you name none (`//BUILD.gn:12`; `boot_check.py` runs `ninja -C <out>` with no target). Its list of dependencies in the root `BUILD.gn` is written by hand, so a test runs only when it is reachable from that list.
3. The list of tests is asked of GN on every run and kept nowhere (`scripts/host_tests.py:10-13`). It takes three queries, with `prebuilt/gn/gn`: `gn ls <out> --testonly=true --type=executable` for the candidates, `gn desc <out> //:default deps --all` for what the default build reaches, and `gn outputs <out> <labels>` for the file each test links (`scripts/host_tests.py:44-50`).
4. Each test runs from the build directory and passes when it exits with 0 (`.agents/skills/ours-verify/SKILL.md:29-30`; `boot_check.py:214-221`).

To run all of them, run the boot check, which runs them while the images boot and prints one `host-tests` line (`.agents/skills/ours-verify/SKILL.md:25-31`). The comment in `boot_check.py` puts all of them at about ten seconds (`boot_check.py:174`). A failing test fails the check and its last lines are printed; all of the output is in `host-tests.log` beside the boot logs (`.agents/skills/ours-verify/SKILL.md:119`).

To run the tests of one library, build its `tests` group and run the binary. `system/lib/loop/README.md:22-23` gives the commands for the Loop:

```text
prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/loop/tests
out/bringup.x86/host/obj/system/lib/loop/test/loop
```

The binary of `//system/lib/<library>/test:<name>` is `host/obj/system/lib/<library>/test/<name>` under the build directory, and `gn outputs <out> '<label>(//build/toolchain/ours:host)'` prints it. The binary is an ordinary googletest program, so `--gtest_filter=<pattern>` runs the tests whose names match and `--gtest_list_tests` lists them (`vendor/googletest/docs/advanced.md:1835` and `:1854`). A test's name is `<fixture>.<test>`, for example `LoopServerTest.OpenOfAMissingNameAnswersNotFound`.

### What a fake kernel is here

A server calls `ours::channel_read`, `ours::channel_write` and the other system calls as functions. Each is declared in `<ours/vdso.hpp>` with an assembler label that sets the symbol the call refers to, `ours_<name>`. In an image nothing defines those symbols in the program: the loader binds them to the vDSO before the program runs (`system/public/ours/vdso.hpp:18-22` and `:47-56`).

On the host nobody binds them, so the test executable defines them itself. A *fake kernel* here is a small model of ports and channels held in ordinary memory, plus those definitions, which forward each call to the model. `system/lib/ioservice/test/fake_kernel.cpp` ends with them:

`system/lib/ioservice/test/fake_kernel.cpp:212-214`:

```cpp
auto ours::channel_create(ChannelCreateOptions, Uandle *const out0, Uandle *const out1) -> Status {
    return fake::kernel().channel_create(out0, out1);
}
```

The library under test is compiled unchanged and linked against these definitions, "so the library's own source is what runs" (`system/lib/ioservice/test/BUILD.gn:19-22`). There is no interface to inject and no `#ifdef`: "which of the two answers is fixed by what the executable links, so the library has no seam of its own" (`system/lib/loop/BUILD.gn:10-11`). A call the library is not meant to make has no definition in the fake, and the test fails to link (`system/lib/ioservice/test/fake_kernel.hpp:22-23`). That link error is a useful alarm: it says your code made a system call the model does not have.

Do not confuse this with `//kernel/tests/fake-kernel`. That is a different thing, a stand-in kernel image that proves the boot chain reached it (`kernel/tests/fake-kernel/README.md:1-8`).

The model keeps the rules of the real kernel that the libraries depend on, and nothing more (`system/lib/ioservice/test/fake_kernel.hpp:25-35`):

- A channel end is readable while messages are queued on it, and reports its peer closed once the peer is closed.
- A write queues the message on the peer, and fails with `PeerClosed` once the peer is gone. The handles in the message are consumed either way.
- A wait is one-shot and level-triggered: armed on an object that already asserts one of its signals, its packet is queued at once; otherwise when the object first asserts one.
- Closing a handle ends its armed waits without a packet, and closes the channel for its peer.
- A port hands its packets out in the order they were queued.

It leaves out rights, and the moving of handles between tables: a handle written into a message keeps its value and stays open in the model (`system/lib/ioservice/test/fake_kernel.hpp:37-40`). So a test against the fake cannot show that the real kernel agrees. That is what the boot is for.

A test can also look inside the model: whether a handle is still open, how many messages are queued on a channel end, which handles were closed and in what order (`system/lib/ioservice/test/fake_kernel.hpp:47-57`).

### How the `ioservice` and `loop` tests use one

There are two fakes, one per test directory, and they differ in how much they model.

- `system/lib/loop/test/fake_kernel.*` models only the three calls the Loop makes, `port_wait`, `object_wait_async` and `handle_close`. Because its tests have no real client, it gives them ways to make things happen: queue messages on an object, take one off, close the peer, queue a packet by hand, and make the next wait fail (`system/lib/loop/test/fake_kernel.hpp:57-76`). That last one produces a failure the real kernel does not produce on demand. It also answers `BadState` to a wait with no deadline on an empty port, which no real `port_wait` does, so that a test that would block for ever fails instead (`system/lib/loop/test/fake_kernel.hpp:97-100`).
- `system/lib/ioservice/test/fake_kernel.*` models the channel and port calls and `handle_close`; its `handle_duplicate` only answers `NotSupported` (`system/lib/ioservice/test/fake_kernel.cpp:233-237`). Its tests need no helpers to move state, because they act as a client through the same calls the server uses: `ours::channel_create`, `ours::channel_write` and `ours::channel_read`, which land in the fake.

A test of a server on a Loop, such as `system/lib/ioservice/test/loop_server_test.cpp`, goes in five steps:

1. Reset the model, so that no object of an earlier test is left (`fake::kernel().reset()`).
2. Build the pieces over it: make a port with `ours::port_create`, a Loop over a table of slots, and the server on the Loop.
3. Act as the client: make a channel pair, hand the server end to the server, and write a request on the client end.
4. Run the Loop with the deadline 0, which dispatches whatever is queued and returns `TimeOut` once the port is empty (`system/lib/loop/include/loop/loop.hpp`, the comment on `run()`).
5. Read the answer off the client end, and check the state of the model.

If a host test never comes back from that run, some slot's wait keeps firing and the port never empties. The rules a slot's callback has to keep so that this does not happen are in the comments of `system/lib/loop/include/loop/loop.hpp`.

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

Read it as a table. A directory connection without the right to connect is refused with `AccessDenied` for a name that exists and for one that does not, because the right is checked before the tree is looked at. With the right, a name that is not there is `NotFound`. `refused()` reads the status back from the epitaph the server wrote on the new connection. The assertions on a missing name, with and without the right, also pin the order: a server that looked the name up first would answer `NotFound` to the second assertion, and the test would fail.

The same refusal, through a real Loop and the fake kernel, also checks what was not done:

`system/lib/ioservice/test/loop_server_test.cpp:293-303`:

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

Four things are pinned. No connect function was called. The client end holds an epitaph with `NotFound`. The channel the Open carried is closed. The directory's own connection is still open for the next request. A server that answered `NotFound` but also dropped the directory connection would pass the first three lines and fail the last. `system/lib/ioservice/test/outgoing_test.cpp:204-217` does the same for the provider side: an Open of an `svc/<name>` the outgoing directory does not serve is answered `NotFound`.

### Adding a host test of your own

1. Write the test with googletest's `TEST` or `TEST_F`, in a file under the library's `test/` directory.
2. Declare an executable. `system/lib/loop/test/BUILD.gn` is the smallest real one:

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

3. Name it in the library's `tests` group, with the host toolchain's label:

`system/lib/loop/BUILD.gn:28-31`:

```gn
group("tests") {
  testonly = true
  public_deps = [ "test:loop($host_toolchain)" ]
}
```

4. Name the group in the `deps` of `group("default")` in the root `BUILD.gn` (`//BUILD.gn`):

`//BUILD.gn:210`:

```gn
    "//system/lib/loop:tests",
```

The last step is the one that is easy to forget. The `tests` group is not found by anything on its own: a test that is not reachable from `//:default` is not built by the default build and not found by `host_tests.py`, so it is never run, and the boot check says nothing. [03-protocols.md](03-protocols.md) scaffolds a library with its test target and walks through this wiring.

To see whether your test is found, ask GN what the rule asks (`prebuilt/gn/gn ls out/bringup.x86 --testonly=true --type=executable`) and look for your label ending in `(//build/toolchain/ours:host)`, or count the `host-tests` line before and after.

## Tests in a boot

### Boot the dev image

From the root of the repository, after the setup in [README.md](README.md):

```bash
python3 .agents/skills/ours-verify/scripts/boot_check.py
```

It builds `out/bringup.x86` with `prebuilt/ninja`, boots both dev images side by side, stops QEMU once each verdict is in, and prints one line per image. The whole run takes about 25 seconds, and exit code 0 means both passed (`.agents/skills/ours-verify/SKILL.md:16-23`). The flags you will use are `--out out/<dir>` for a build directory of your own, `--no-build` to boot what is already built, `--only rel` or `--only dbg` for one image, and `--timeout N`. `--no-tests` skips the host tests, for a loop of boots only (`.agents/skills/ours-verify/SKILL.md:49-52`).

Boot the `rel` image when you are checking a server. The `dbg` image has no root, so it has no routing and none of the example components (`boot_check.py:81-89`).

A boot passes when all of these hold (`boot_check.py:15-20`):

- the shell printed its closing `--- end ---` line (`boot_check.py:75`);
- every line the image requires appeared. For `rel` they are `routing_checks: passed`, `init: echo exited with 0`, `init: holding the IRQ and I/O port resources`, `pc-ps2: ready`, `pc-ps2: key a` and `pc-ps2: key A` (`boot_check.py:90-100`);
- no line has the word `FAILED` in it (`boot_check.py:153`);
- nothing logged at error level, which a panic and a failed assertion both do.

If the check fails, `.agents/skills/ours-verify/SKILL.md:108-120` has a table from the summary line to its meaning. The row for a missing required line says: read the `init:` lines in `rel.log`, since a component that did not start, or a use that routes nowhere, is logged there.

### Read the boot log

The last line of the output names the directory of the logs, `<dir>/{dbg,rel}.log` (`boot_check.py:588`). The directory is `ours-verify-<name of the build directory>` under the temporary directory (`boot_check.py:560`), so two checkouts that both use `out/bringup.x86` share it. Give each its own `--out`. `host-tests.log` is beside them.

The `dbg` image logs heavily, so search a log for markers instead of reading it from the top (`.agents/skills/ours-verify/SKILL.md:191-192`). The root's lines start with `init:`, and a server's start with whatever prefix it prints, as echo's start with `echo:`:

```bash
grep -n 'init:\|echo:\|routing_checks:' <dir>/rel.log
```

A server prints with `ort::debug_line`, which writes one line to the kernel's debug console, which the scripts read from QEMU's serial port (`system/lib/ort/include/ort/debug.hpp:16` and `:30-40`; `scripts/qemu_boot.py:231`). The runtime has no formatted printing: the C library a program links is only the memory and string functions, so a line is built from pieces, and a number goes through `ort::decimal` (`system/lib/ort/include/ort/debug.hpp:45-52`). A line longer than one system call may carry can be split by another program's output (`system/lib/ort/include/ort/debug.hpp:38-39`), so keep a line short.

`examples/echo/echo_server.cpp` shows the habit worth copying: one `report(what, status)` function that prints `echo: <what>: <Status>`, called wherever the server gives up (`examples/echo/echo_server.cpp:63-66`). With the program's name first, `grep` finds every line of one component, and with the status last the line says why.

The lines the root prints about a component's life, in the order a normal boot prints them, are these (the full list of the root's lines is in "Other lines the root prints" below):

- `init: started <moniker>` when it started the component's process (`system/service/init/realm.cpp:675`). It starts the children of `root.cm` in the order they are listed (`system/service/init/realm.cpp:418-429`, `kernel/products/image/dev/BUILD.gn:88-109`).
- `init: <moniker> exited with <code>` when the process ended (`system/service/init/realm.cpp:849-850`). A server meant to keep running that has this line has stopped, and the lines it printed just before are the first place to look.

If there is no `init: started <your component>` line, the component did not start; look for `init: cannot resolve`, `init: cannot start` and `init: launch failed` above its place in the log.

To make a boot depend on your server, give a component the job of connecting to it and checking what it answers, the way `routing_checks` does. Have it print a line when all of its checks passed, and add that line to the `REQUIRED` list of the `rel` image in `boot_check.py` (`boot_check.py:90-100`). Then a server that does not start, or does not answer, fails the check.

### How `routing_checks` makes a negative case

`routing_checks` is a component the root starts as one of its children. Its job is to try things that must be refused, and to fail loudly when one is not. A negative case there has four parts.

First, the manifest asks for something that must fail. The protocol uses of `routing_checks` include `ours.test.Unoffered`, which no parent offers:

`system/service/init/test/routing_checks/BUILD.gn:75-84`:

```gn
      {
        protocol = [
          "ours.process.Launcher",
          "ours.resource.Debug",
          "ours.resource.Sampling",
          "ours.resource.Irq",
          "ours.resource.Ioport",
          "ours.test.Unoffered",
        ]
      },
```

The root's manifest offers the others and does not offer that one (`kernel/products/image/dev/BUILD.gn:141-155`). The directory `denied` is made in the same way: the use asks for `rx*` and the offer gives only `r*` (`kernel/products/image/dev/BUILD.gn:132-139`).

Second, the program connects and reads what comes back, and expects the failure. A connect is one-way, so it succeeds before the root has decided anything, and the refusal shows up as the first message on the channel:

`system/service/init/test/routing_checks/routing_checks.cpp:528-541`:

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

Third, a failed check prints a line with `FAILED` in it (`routing_checks.cpp:137-141`), and the program ends by printing `routing_checks: passed` and exiting with 0 only if none failed (`routing_checks.cpp:906-913`). The root logs the exit as `init: routing_checks exited with 0` (`routing_checks.cpp:46-48`).

Fourth, the boot check turns those lines into a verdict. Any `FAILED` line fails the image (`boot_check.py:153`, `:408-409`, `:507-508`), and `routing_checks: passed` is among the required lines (`boot_check.py:93`), so a component that never runs, or stops before the end, fails too.

Expect the root to print a line for each deliberate refusal, and do not mistake it for a failure. The change record of the root's log lists "the two deliberate `routes nowhere` lines of routing_checks (`denied`, `ours.test.Unoffered`)" (`docs/changes/261029/init.md:146`):

`docs/changes/261004/init.md:42` and `:438`:

```text
init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
```

A negative case you have never seen fail proves nothing, because a check that cannot fail looks the same as one that passes. Break the thing it checks, reboot, and watch it fail, then restore it. The change record of the type check did exactly that, by renaming the type a use asked for, and recorded what appeared (`docs/changes/261029/init.md:134-139`), which "What went wrong" below quotes. The same record says why it matters: "silence looks like a pass" (`docs/changes/261029/init.md:27`).

### The kernel shell and `kshell_run.py`

The kernel has a shell of its own, with commands that test the kernel, such as `vmosnapshot.test`. They do not run at boot. Only the `dbg` image has this shell, since the user shell holds the serial port on `rel` (`.agents/skills/ours-verify/SKILL.md:80-93`). `scripts/kshell_run.py` boots the `dbg` image in `--out`, types the commands you name once the boot ends, and exits with 0 if each passed (`scripts/kshell_run.py:1-35`). You need it when you change the kernel. A server is checked with `boot_check.py`.

## What went wrong: what a client sees

### How a client learns the outcome

Opening a connection to a protocol is one-way. `fsio::connect` writes an `Open` for `/svc/<name>` and returns without waiting, so it answers `Ok` before the root has routed anything (`system/lib/fsio/include/fsio/fsio.hpp:52-60`):

`system/lib/fsio/include/fsio/fsio.hpp:87`:

```cpp
auto connect(std::string_view path, ours::Uandle server) -> ours::Status;
```

The result arrives on the channel. The client reads its end and finds one of three things:

- an epitaph. Its status is the reason the connection was refused. The server wrote it as the channel's last message and closed;
- nothing, and `PeerClosed`. The other end closed without saying why;
- ordinary protocol messages. The connection works.

A client has to wait for the channel to be readable or its peer to be closed, and only then decide which of the first two it has. `routing_checks` does it in one small function, and every check above is built on it:

`system/service/init/test/routing_checks/routing_checks.cpp:343-357`:

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

A message that is readable may be an epitaph or a reply: the client looks at the header before it decodes (`system/lib/wire/include/wire/wire.hpp:233-243`). Two details matter when you read a client's report:

- A write on a channel whose peer closed answers `PeerClosed`, but the epitaph the peer left is still there to read, so a client that writes first and then reads still gets the reason (`system/lib/ours.resource/src/client.cpp:28-29`).
- An epitaph whose status is `Ok` says only that the server closed with nothing wrong, and the client libraries read it as `PeerClosed` (`system/lib/ours.resource/src/client.cpp:75-79`).

A client that does not read epitaphs sees `PeerClosed` for every refusal (`docs/changes/261029/init.md:141`). That is why the mismatch of "Two cases worked through" below once read `PeerClosed` with nothing from the root, and now reads `BadType` with a line from the root (`docs/changes/261029/init.md:18-24` and `:132`).

### Four questions, in order

When a connection fails, ask these in order.

1. Did `fsio::connect` itself return an error? Then no message was sent. Row 1 or row 9 below.
2. If it returned `Ok`, what did the first read give? An epitaph: its status picks the rows. No epitaph, `PeerClosed`: rows 11 to 13.
3. Does the root's log hold a line for it? Search for `use of "<name>"`, `cannot forward` and `cannot serve`.
4. If the root's log is silent, who else could have answered? The provider's own program, or the `/svc` tree that never held the name: rows 2, 4, 5, 10 and 13.

### The table

Each row is one cause. The root's lines have the shape shown; `<Status>` is the name of a status, such as `NotFound`; `<moniker>` is defined above. In the last column `realm.cpp` is `system/service/init/realm.cpp`, `route.hpp` is `system/lib/component/include/component/route.hpp` and `fsio.hpp` is `system/lib/fsio/include/fsio/fsio.hpp`. Line numbers are those of the files at the commit given at the end of this chapter.

| # | The client sees | What happened | The root logs | Produced by |
|---|---|---|---|---|
| 1 | `fsio::connect` returns `NotFound`, nothing sent | The component has no protocol `use` at all, so the root made it no `/svc`, and the client's namespace has nothing to resolve `/svc/<name>` against | nothing | `system/lib/fsio/include/fsio/namespace.hpp:124`; the root makes a `/svc` only for a component with a protocol use, `system/service/init/realm.cpp:541` |
| 2 | an epitaph `NotFound` | `<name>` is not in this component's `/svc`: it is mistyped, or missing from the component's own `use` list, which is where to add it. A `/svc` holds one entry per protocol use | nothing | the lookup, `system/lib/ioservice/src/tree.cpp:203`; the refusal, `system/lib/ioservice/src/server.cpp:529-532` and `:96-102`; written on the client's channel, `system/lib/ioservice/src/loop_server.cpp:163-169`; the entries, `realm.cpp:696-718` |
| 3 | an epitaph `NotFound` | The route leads nowhere: the parent offers nothing of that name to this component, a child that should provide it exposes nothing of that name or cannot be resolved, the provider has no program or declares no such capability, or no built-in has the name | `init: <moniker>'s use of "<name>" routes nowhere: NotFound at <where> (<verb> of "<name>")` | status: `system/lib/component/include/component/route.hpp:412-415`, `:427-430`, `:444-451`, `:458-464`, listed at `:307-317`; the reply, `realm.cpp:740-745`; the line, `realm.cpp:201-203` |
| 4 | an epitaph `NotFound` | The provider is not running. It exited, or could not be started, and the root does not start it again, so every later route to it ends here | nothing for this connect. The cause is an earlier line: `init: <provider> exited with <code>` or `init: cannot start <provider>: <Status>` | `realm.cpp:782-785`; the state is set at `realm.cpp:410` and `:855`; the lines are at `realm.cpp:849-850` and `:405-406`. The missing line is recorded as item 10 in `docs/repairs/261004/init.md` |
| 5 | an epitaph `NotFound` | The provider runs, but its outgoing directory has no `svc/<name>`: its manifest declares the protocol and its program never added it under that name | nothing. The root forwards the Open and does not wait, and the provider's directory answers | `system/lib/ioservice/src/tree.cpp:203` in the provider's own tree; `system/lib/ioservice/test/outgoing_test.cpp:204-217`; `realm.cpp:809-813` |
| 6 | an epitaph `AccessDenied`, on a directory entry only | A directory `use` asks for rights that a hop of its route does not give, such as `rx*` against an offer of `r*`. A protocol use states no rights, so routing never answers a protocol this way. The root closed the entry before the component ran, and the first `fsio::open` under it reads the epitaph and keeps it | `init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")` | `route.hpp:271-282` and `:318`; `realm.cpp:504-507` and `:525`; the client, `system/lib/fsio/include/fsio/fsio.hpp:66-74` |
| 7 | an epitaph `BadType` | The route was found, and it ends at a capability of another protocol than the use means. The names agree and the protocols differ, usually after an offer renamed it with `as`. The build refuses a mismatch it can see, so this appears when the type check is off or the image was not built from these manifests | `init: <moniker>'s use of "<name>" reaches another protocol: it wants <want>, the route ends at <got> (<where>)`, where `<where>` is `the built-ins` or `provided by <moniker>` | `route.hpp:301-305`, `:421-423`, `:465-467`; the reply, `realm.cpp:743`; the line, `realm.cpp:188-199` |
| 8 | an epitaph `NoResource` | A built-in server of the root has no free connection: the Launcher and each resource protocol take four at a time | `init: cannot serve <name> to <moniker>: <Status>` | `system/service/init/launcher_server.cpp:79`, `system/service/init/resource_server.cpp:55`, `system/service/init/pools.hpp:32` and `:46`; `realm.cpp:770-775` |
| 9 | `fsio::connect` returns `NoResource`, for every protocol of that component | The root could not make the component's `/svc`. The plainest cause is more than 15 protocol uses in its manifest; the status in the log line says which. The root closed the entry with an epitaph, and the client's first call reads it and keeps it | `init: <moniker>'s /svc cannot be served: NoResource` | `realm.cpp:689-691` and `:549-553`; `system/service/init/pools.hpp:55`; `fsio.hpp:66-74` |
| 10 | an epitaph `NoResource` | The provider's own table is full: it has no free place for another client. `echo` keeps four | nothing from the root. The provider prints only what its program prints | `examples/echo/echo_server.cpp:141`; `:52-54`; when the Loop cannot take the channel, `:132-138` |
| 11 | `PeerClosed`, no epitaph | The root could not forward the Open. Its write into the provider's outgoing directory failed, because that channel already holds 64 unread messages or its other end is gone. A failed write still consumes the handles, so the client's channel, which the Open carried, closes with nothing written on it | `init: cannot forward <name> to <provider>: <Status>` | `realm.cpp:809-821`; `system/public/ours/syscalls.gni:197-208`; the limit, `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28` |
| 12 | `PeerClosed`, no epitaph | The Open reached a provider that was exiting and was lost with it. A race, rare, and the next connect gets `NotFound` | nothing | `routing_checks.cpp:805-825`, which retries once |
| 13 | `PeerClosed`, no epitaph | The server closed the connection on a message it does not accept: a method number it does not define, or a message its decoder refuses. A closed protocol does this on purpose: `ioservice::Server` does it for the ours.io connections it serves, and the root's built-in servers close a message of another protocol the same way | nothing | `examples/echo/echo_server.cpp:172-191`; `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40`; `system/lib/ioservice/src/server.cpp:561-565`; `docs/changes/261004/init.md:458`; see [03-protocols.md](03-protocols.md#closed-protocols) |

Notes on the table:

- Row 3's `<where>` is the moniker of the instance whose declaration the walk stopped at, or `the built-ins` when it stopped above the root. `<verb>` is `offer`, `expose`, `declaration` or `use` (`realm.cpp:156-168`, `:182-187`). `at .` means the root: the parent had no offer. Read the verb and the name together. For `NotFound`, `offer of "X"` at the parent `P` means `P`'s manifest has no offer of `X` to this component; the same words after `AccessDenied` mean the offer exists and gives too little.
- A client that built its own Open by hand meets two more statuses on a `/svc`. A flag other than `PROTOCOL_SERVICE` on a service is `InvArgs`, and a right the `/svc` connection does not hold is `AccessDenied`, from the Open's checks, in their fixed order (`system/lib/ioservice/src/server.cpp:248-250` and `:335-337`; `system/lib/ioservice/test/server_test.cpp:602-606`). `fsio::connect` sends `PROTOCOL_SERVICE` alone, so it does not meet them. The order of the checks decides which error a request that is wrong in several ways gets, and [04-tree-servers.md](04-tree-servers.md) walks through it.
- A connection that worked and then reads `PeerClosed` is not a refusal: the server exited or released it. `routing_checks` expects exactly that after it asks `echo` to stop (`routing_checks.cpp:760-764`).
- The statuses in the table are the names `ours::to_string` gives, and the same names appear in the root's lines (`realm.cpp:202`).

### Other lines the root prints

| The line | What it says | Printed at |
|---|---|---|
| `init: started with the root job` | the root started | `system/service/init/init.cpp:223` |
| `init: holding the IRQ and I/O port resources` | it holds the roots of the interrupt lines and of the I/O ports | `system/service/init/init.cpp:252` |
| `init: /boot has <N> files, <M> executable` | it ingested the boot files | `system/service/init/init.cpp:280-281` |
| `init: cannot resolve meta/root.cm = <Status>` | the root manifest could not be read; the root halts | `system/service/init/init.cpp:322-325` |
| `init: started <moniker>` | a component's process started | `realm.cpp:675` |
| `init: <moniker> exited with <code>` | its process ended | `realm.cpp:849-850` |
| `init: cannot resolve <moniker>: <Status>` | a child's manifest could not be read or decoded, or the instance pool is full | `realm.cpp:424-425` |
| `init: cannot start <moniker>: <Status>` | the program could not be run; routes to it answer `NotFound` from then on | `realm.cpp:405-406` |
| `init: launch failed: <what>` | a step of loading the program failed | `realm.cpp:272-273` |
| `init: cannot watch <moniker>: <Status>` | the root could not wait for the process to end | `realm.cpp:670-671` |
| `init: <moniker>'s /svc cannot be served: <Status>` | row 9 | `realm.cpp:550-551` |
| `init: cannot serve <name> to <moniker>: <Status>` | row 8 | `realm.cpp:772-773` |
| `init: cannot forward <name> to <provider>: <Status>` | row 11 | `realm.cpp:819-820` |
| `init: /boot cannot answer a client: <Status>` | a reply of the root's own file server could not be written | `realm.cpp:723` |

### Two cases worked through

**A route that ends at the wrong protocol.** The change record of the type check broke a route on purpose, with the check off, and recorded both ends of it:

`docs/changes/261029/init.md:126-129`:

```text
init: started pc-ps2
init: pc-ps2's use of "ours.resource.Irq" reaches another protocol: it wants ours.resource.Irq, the route ends at ours.process.Launcher (the built-ins)
pc-ps2: getting the IRQ resource: BadType
init: pc-ps2 exited with 1
```

Read it from the top. The root started `pc-ps2`. When `pc-ps2` connected to `/svc/ours.resource.Irq`, the root routed the use and found an offer, but the offer renamed the Launcher to that name, and the Launcher is another protocol. The second line says which two were mixed up and where the route ended. The third line is the client's own report: it read the epitaph, so it says `BadType`. Before the root wrote that epitaph, the same fault read `getting the IRQ resource: PeerClosed` and the root said nothing (`docs/changes/261029/init.md:20-24`). The fix is in the manifest: offer a capability of the protocol the use means, under that name, or give the use the `type` it means.

The other branch of the line names a provider and not the built-ins:

`docs/changes/261029/init.md:137-138`:

```text
init: routing_checks's use of "ours.test.Echo" reaches another protocol: it wants ours.examples.echo.EchoWrong, the route ends at ours.examples.echo.Echo (provided by echo)
routing_checks: EchoString through the sibling's expose = BadType (expected Ok) FAILED
```

The second line has `FAILED` in it, so the boot check fails the image, which is the point of the check.

**A route to a protocol nobody offers.** This is row 3, and it is the line in `routing_checks`' deliberate case above:

```text
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
```

Read it as: the use is `routing_checks`'s, the walk stopped at the root (`.`), and what was missing was an offer of that name from the root to `routing_checks`. The fix is an `offer` in the parent's manifest, to this child, under that name. If the line is not there at all while the client got `NotFound`, the cause is one of rows 2, 4 and 5, and the way to tell them apart is the questions above.

## Debugging further

- **A boot that hangs or goes silent.** `boot_check.py` sends QEMU an `nmi` after 15 seconds without a line, and the kernel prints a report of what every CPU was doing and panics. The report is kept in the log and printed under the verdict. `.agents/skills/ours-verify/SKILL.md:122-180` explains how to read it.
- **A debugger.** `docs/development/debugging.md` describes starting the machine under the debugger from the editor with F5. It is written in Chinese, and this guide does not copy it. `.agents/skills/ours-verify/references/debugging.md` describes, in English, attaching `lldb` to a machine halted before its first instruction, setting breakpoints (it uses the kernel's panic function, `vpanic`), and running, interrupting and inspecting the machine. The one-click session and the manual one both start the machine through `scripts/qemu_debug.py` (`scripts/qemu_debug.py:1-9`).
- **Your own lines.** The most direct debugging of a server is a line at each place it gives up, in the style above, and a boot of the `rel` image (`--only rel`).

## Where to look next

- [06-checklist.md](06-checklist.md): the checks to run before a server is called done, grouped by the phase you are in, with the test and boot items among them.
- [03-protocols.md](03-protocols.md#closed-protocols): why an unknown method closes the connection with no epitaph, and what that costs the client.
- [04-tree-servers.md](04-tree-servers.md): the Open checks a tree server runs, in order, which decide the statuses of rows 2 and 6.
- `.agents/skills/ours-verify/SKILL.md`: everything the boot check does, its flags and its table of failures.
- `docs/development/debugging.md`: the debugger, in Chinese.

## Sources

- `scripts/host_tests.py`, `scripts/kshell_run.py`, `scripts/qemu_boot.py`, `scripts/qemu_debug.py`, `build/config/BUILDCONFIG.gn`, `BUILD.gn` (the root)
- `.agents/skills/ours-verify/SKILL.md`, `.agents/skills/ours-verify/scripts/boot_check.py`, `.agents/skills/ours-verify/references/debugging.md`
- `system/lib/ioservice/` (`README.md`, `BUILD.gn`, `src/server.cpp`, `src/tree.cpp`, `src/loop_server.cpp`, `src/outgoing.cpp`, `test/BUILD.gn`, `test/fake_kernel.hpp`, `test/fake_kernel.cpp`, `test/server_test.cpp`, `test/loop_server_test.cpp`, `test/outgoing_test.cpp`)
- `system/lib/loop/` (`README.md`, `BUILD.gn`, `include/loop/loop.hpp`, `test/BUILD.gn`, `test/fake_kernel.hpp`, `test/loop_test.cpp`)
- `system/service/init/` (`realm.cpp`, `init.cpp`, `pools.hpp`, `launcher_server.cpp`, `resource_server.cpp`, `BUILD.gn`, `test/routing_checks/BUILD.gn`, `test/routing_checks/routing_checks.cpp`)
- `kernel/products/image/dev/BUILD.gn`, `examples/echo/` (`echo_server.cpp`, `BUILD.gn`, `protocol/`)
- `system/lib/component/include/component/route.hpp`, `system/lib/fsio/include/fsio/` (`fsio.hpp`, `namespace.hpp`), `system/lib/ours.resource/src/client.cpp`, `system/lib/wire/include/wire/wire.hpp`, `system/lib/ort/include/ort/debug.hpp`
- `system/public/ours/vdso.hpp`, `system/public/ours/syscalls.gni`, `kernel/object/include/ours/object/channel_dispatcher.hpp`, `kernel/tests/fake-kernel/README.md`
- `docs/changes/261029/init.md`, `docs/changes/261004/init.md`, `docs/architecture/component-framework/glossary.md`, `docs/repairs/261004/init.md`, `docs/development/debugging.md`, `vendor/googletest/docs/advanced.md`

Checked against commit `693260d0` on 2026-10-29.
