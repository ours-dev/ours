# Your first server

This chapter is a tutorial. You copy `examples/echo`, the small server that ships in the tree, call the copy `hello`, cut it down to almost nothing, and grow it back in five levels, until the root starts it, a client reaches it, and every boot checks that it still answers. Each level builds, boots and says how to tell it worked, and adds one or two ideas. Words that have a meaning of their own here are linked to [01-concepts.md](01-concepts.md) at first use.

## What you will be able to do

- Start from `examples/echo` and end with a server of your own: a protocol, a program, a manifest, and the edits that make the root start it.
- Write the client side of the exchange, as a check in the dev image's test component.
- Say, for each level, what to run and what a pass looks like in the log.
- Make a boot fail when your server stops working.

## Before you start

Read [01-concepts.md](01-concepts.md) first: this chapter links back into it instead of repeating it. You also need a build that boots the untouched tree. [00-setup.md](00-setup.md) says what the build needs in `prebuilt/`, and [the README](README.md#build-and-boot-the-dev-image) says what the verify script does. From the root of your checkout:

```bash
python3 .agents/skills/ours-verify/scripts/boot_check.py --only rel --out out/hello
```

`--only rel` boots the dev image (`kernel.dev.obi`; the script calls it `rel`), the one with a root. `--out out/hello` is a build directory of your own, which the script configures on first use. A pass looks like this, with paths shortened and numbers that will differ on your machine. The first build took 34 s on the machine used for this chapter; with the build done, a rebuild and a boot took about 7 s:

```text
build      ok (.../out/hello, 34.0s)
rel        PASS  shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end
host-tests PASS  148 passed, 0 failed · 10.9s · log .../host-tests.log
logs       .../ours-verify-hello/{rel}.log
```

The last line names the directory that holds `rel.log`, the serial log of the boot. This chapter reads it with `grep -a '<pattern>' <dir>/rel.log`. Give `grep` the `-a`: the log holds a few NUL bytes, so `grep` takes it for a binary file and, without the flag, prints `Binary file ... matches` and no lines. If the script prints `FAIL`, the table "Reading a failure" in the verify script's documentation (named in [the README](README.md#build-and-boot-the-dev-image)) says what each message means.

## The places you will touch

A server is spread over seven places. The table lists them in the order you meet them.

| # | What | Where | Copy or write | Level |
|---|---|---|---|---|
| 1 | The protocol library: messages and their codec | `examples/hello/protocol/` | copy `examples/echo/protocol/`, rename, then fix two numbers | 0 |
| 2 | The program, and its build target `user_executable("hello_server")` | `examples/hello/hello_server.cpp`, `examples/hello/BUILD.gn` | write a short `main`, then grow it by copying parts of `echo_server.cpp` | 1 to 4 |
| 3 | The manifest `component_manifest("hello.cm")` | `examples/hello/BUILD.gn` | copy echo's, rename | 1 and 2 |
| 4 | The root's manifest: a child entry, then an offer | `kernel/products/image/dev/BUILD.gn` | add two entries modelled on echo's | 1 and 2 |
| 5 | The client's manifest: a `use`, and a dependency on your library | `system/service/init/test/routing_checks/BUILD.gn` | add | 2 |
| 6 | The client's check | `system/service/init/test/routing_checks/routing_checks.cpp` | write, by copying echo's | 2 to 4 |
| 7 | The lines a boot must print | `REQUIRED` in `.agents/skills/ours-verify/scripts/boot_check.py` | add two lines | 5 |

The client is `routing_checks`, the dev image's test component, which already acts as echo's client. Every `file:line` in this chapter is a file of the tree as it stands, so you can open it, and code under a `file:line` is copied exactly from there. The one piece of code in this chapter that echo does not already contain is the short `main` of level 1.

## Level 0: choose, and define the protocol

This level decides what you are building and creates its protocol library. Nothing runs yet.

A server is a *protocol server* if it answers a protocol of its own on each client's channel, as `echo` does, and a *tree server* if it serves files and directories that a client opens by path. A first server is a protocol server; [04-tree-servers.md](04-tree-servers.md#choosing-a-tree-server-or-a-protocol-server) is for the other kind.

Yours is `hello`. Its protocol is named `ours.examples.hello.Hello` and its library `ours_examples_hello` ([03-protocols.md](03-protocols.md#names-and-the-protocol-type) explains the three names a protocol has). It has one method of its own, `Greet`, which answers with the string it was sent. The library keeps echo's second method, `Stop`, which asks the server to exit; level 4 uses it.

**Copy.** Make the directory and copy the library: `mkdir examples/hello`, then `cp -R examples/echo/protocol examples/hello/protocol`.

**Trim.** In the copy delete `test/` and `README.md`, and the `tests` group at the end of `BUILD.gn` (`examples/echo/protocol/BUILD.gn:38-42`). A host test of the codec is steps 6 and 7 of [the recipe in chapter 3](03-protocols.md#6-write-the-test); the tests in this chapter are boots.

**Rename.** Rename `include/ours_examples_echo` and the header and source inside it, then replace the names in this table in every file of the copy, `.clang-format` included. Do the rows in order.

| Echo's | Yours |
|---|---|
| `ours_examples_echo`, `OURS_EXAMPLES_ECHO` | `ours_examples_hello`, `OURS_EXAMPLES_HELLO` |
| `ours.examples.echo`, `//examples/echo` | `ours.examples.hello`, `//examples/hello` |
| `Echo.EchoString`, `EchoString`, `ECHO_STRING`, `echo_string` | `Hello.Greet`, `Greet`, `GREET`, `greet` |
| `ECHO_NAME`, `Echo` | `HELLO_NAME`, `Hello` |

`grep -ri echo examples/hello/protocol` prints nothing when the renames are done.

**Fix the method numbers.** A method's number (the code calls it the `ordinal`) is computed from the selector string beside it, and the copy still carries echo's two numbers ([03-protocols.md](03-protocols.md#methods-and-their-numbers) says why). This is how echo declares a method:

`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:56-60`:

```cpp
inline constexpr auto ECHO_STRING = wire::Method{
    .selector = "ours.examples.echo/Echo.EchoString",
    .ordinal = 0x6e80'6bd7'7638'6789,
    .kind = wire::MethodKind::TwoWay,
};
```

Run `python3 system/lib/wire/test/ordinals_test.py`. It fails and names the right number for each selector, in a line like `...: ours.examples.hello/Hello.Greet is 0x0b34f151d5b5de80, not 0x6e806bd776386789`. Two of its failures say a number is not unique: they clear when both numbers are replaced. Paste each number into the `.ordinal` of its method (the file groups the digits with `'`, as in `0x0b34'f151'd5b5'de80`) and run the test again until it ends with `OK`.

Verification: the test prints `OK`. It is all this level can show, because nothing builds the library until level 2. Neither the default build nor a boot runs this test (`system/lib/wire/BUILD.gn:26-27`), so a wrong number would still boot.

## Level 1: the root starts it

This level adds a program and a manifest, and gets the root to start it. The program only says it started and exits.

Write `examples/hello/hello_server.cpp`:

```cpp
#include <ort/debug.hpp>
#include <ort/main.hpp>

auto main(int, char **) -> int {
    auto _ = ort::debug_line("hello: started");
    return 0;
}
```

`ort` is the runtime every program links, and `ort::debug_line` writes one line to the debug console, which is the serial log. `main`'s return value is the process's exit code. Keep `<ort/main.hpp>`: without it the link fails with `ld.lld: error: undefined symbol: main` and the hint `did you mean to declare main(int, char**) as extern "C"?`, because a program here is compiled freestanding and the runtime looks for an unmangled `main`.

A `BUILD.gn` file declares build targets, which other files name by label: `//examples/hello:hello.cm` is the target `hello.cm` in `examples/hello/BUILD.gn`, and `//` is the root of the repository. Copy `examples/echo/BUILD.gn` to `examples/hello/BUILD.gn`, replace `echo_server` by `hello_server` and `echo.cm` by `hello.cm`, delete its comments, and for this level also delete the program's `deps` and the whole `manifest = { ... }` of the manifest (lines 24-28 and 36-48). Keep the `import` lines and the `assert` (lines 5-7 and 13-15), which stops the build reading the file for anything but programs that run in user mode.

The program is a `user_executable`:

`examples/echo/BUILD.gn:21-29`:

```gn
user_executable("echo_server") {
  visibility = [ "//examples/*" ]
  sources = [ "echo_server.cpp" ]
  deps = [
    "protocol",
    "//system/lib/ioservice:outgoing",
    "//system/lib/loop",
  ]
}
```

- `visibility` limits which targets may depend on it: only targets under `//examples`.
- `sources` is its source file. The program is installed as `bin/hello_server`.
- `deps` are the libraries it links. `main` needs none yet; level 2 puts them back.

The manifest is a `component_manifest`, and the target's name must end in `.cm`:

`examples/echo/BUILD.gn:33-35`:

```gn
component_manifest("echo.cm") {
  visibility = [ "//kernel/products/image/*" ]
  program = ":echo_server"
```

- The name becomes `meta/hello.cm` in the image.
- `visibility` lets only the image's own targets depend on it, here the root's manifest.
- `program` is the label of the program, with a bare `:` for "in this file".

Level 2 adds the `manifest = { ... }` scope. Next, the root has to start the component. The root's manifest lists its children, and echo's entry is:

`kernel/products/image/dev/BUILD.gn:94-98`:

```gn
    {
      name = "echo"
      manifest = "//examples/echo:echo.cm"
      startup = "eager"
    },
```

Add an entry after it, copying echo's with `name = "hello"` and `manifest = "//examples/hello:hello.cm"`. `name` is what an offer calls the child (`#hello`) and what the root prints in its log lines. `manifest` is the label of the child's `component_manifest`. `startup = "eager"` makes the root start the child at boot, and the build refuses a child that does not write it out. The root starts its children in the order the list gives.

Verification: run the boot command of "Before you start" and read the log with `grep -a 'init: \|hello: ' <dir>/rel.log`. Among its lines you should find:

```text
init: started hello
hello: started
init: hello exited with 0
```

The root logs the start, and the exit code when `main` returns. The two lines `init: routing_checks's use of ... routes nowhere` that the log also holds were there before you changed anything: the dev image's test component asks for two things that must be refused. The boot still reports `6/6 required`, since it knows nothing of `hello` yet.

## Level 2: a client can reach it

This level makes the program a server: it serves an outgoing directory and registers its protocol on a Loop, and the manifests say who may reach it. For now every client is turned away with an [epitaph](01-concepts.md#epitaphs), so that the answer shows the route works. Two ideas are new: serving a protocol, and where a program's memory comes from.

### The program

The root gives every component that has a program the server end of its *outgoing directory*, as a startup handle ([01-concepts.md](01-concepts.md#svc-and-the-outgoing-directory) says what that directory is). Echo's `main` takes it first (`examples/echo/echo_server.cpp:269`). It then makes a port and a Loop to wait on ([01-concepts.md](01-concepts.md#the-loop-and-its-slots)), and an `ioservice::Outgoing`, which serves the outgoing directory on that Loop:

`examples/echo/echo_server.cpp:275-283`:

```cpp
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    EchoServer server(*ECHO_STORAGE.init(), loop);
```

It registers the protocol under its name, serves the directory, and runs the Loop, which comes back only if the port fails:

`examples/echo/echo_server.cpp:285-299`:

```cpp
    if (auto const status = outgoing.add_protocol(ours_examples_echo::ECHO_NAME, server);
        status != Status::Ok) {
        report("cannot add its protocol", status);
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request); status != Status::Ok) {
        report("cannot serve its outgoing directory", status);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Stop ends the program from inside an on_signals(); run() comes back only
    // when the port itself fails.
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
```

What receives each client is a `Handler`. This is the whole class:

`system/lib/ioservice/include/ioservice/outgoing.hpp:50-62`:

```cpp
class Handler {
  public:
    /// |channel| is the request's, and the function owns it from the call on:
    /// it serves it, typically by adding it to the Loop, or closes it. It
    /// runs inside a Loop callback, so it may add() slots but must not wait.
    virtual auto connect(ours::Uandle channel) -> void = 0;

  protected:
    Handler() = default;
    Handler(Handler const &) = default;
    auto operator=(Handler const &) -> Handler & = default;
    ~Handler() = default;
};
```

You derive from it. `connect` receives the channel of each client and owns it: it serves the channel or closes it. The destructor is protected and not virtual because nothing is ever deleted here. For now `connect` refuses every client with an epitaph, the last message on a channel, which carries a status. Write `class HelloServer final: public ioservice::Handler` with a private `auto connect(Uandle const channel) -> void override` whose body is `close_with_epitaph(channel, Status::NotSupported);`.

Build `hello_server.cpp` from echo's, renamed, with the class between `close_with_epitaph` and the storage:

| `echo_server.cpp` lines | What | Change |
|---|---|---|
| 27-50 | includes, `namespace {` and the `using`s | `ours_examples_echo` becomes `ours_examples_hello` |
| 56-61 | the Loop's slot count | `SLOTS` is `ioservice::Outgoing::MAX_CONNECTIONS` for now: a slot for each connection to the outgoing directory |
| 63-84 | `report`, `write`, `close_with_epitaph` | `"echo: "` becomes `"hello: "` |
| 255-264 | the storage | leave out `ECHO_STORAGE` for now |
| 266-300 | `main` | `ECHO_NAME` becomes `HELLO_NAME`; `HelloServer server;` replaces the `EchoServer` line |

A program here has no heap, and nothing runs the constructors of its global objects. So its tables are globals that are built with the program, marked `constinit`, and a type with a destructor sits in a `ustl::LazyInit`, raw storage that `main` builds the object in, once, with `init()`. These are echo's three:

`examples/echo/echo_server.cpp:261-263`:

```cpp
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<EchoServer::Storage> ECHO_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
```

[01-concepts.md](01-concepts.md#where-a-servers-memory-comes-from) explains each word, and [04-tree-servers.md](04-tree-servers.md#storage-you-lend-and-why-it-is-constinit) how to size a larger storage.

### The route

In `examples/hello/BUILD.gn` put back the program's `deps` (`protocol` is your library, found by its directory) and add the manifest's scope, which says what the component provides and hands to its parent:

`examples/echo/BUILD.gn:36-49`:

```gn
  manifest = {
    capabilities = [
      {
        protocol = "ours.examples.echo.Echo"
      },
    ]
    expose = [
      {
        protocol = "ours.examples.echo.Echo"
        from = "self"
      },
    ]
  }
}
```

`capabilities` declares the protocol. `expose` hands it up to the parent, `from = "self"`. The name is the string of `HELLO_NAME`.

Who may reach it is decided by its parent, the root. The root's manifest has an `offer` list (it opens at `kernel/products/image/dev/BUILD.gn:111`), and echo's offer in it is:

`kernel/products/image/dev/BUILD.gn:186-191`:

```gn
      {
        protocol = "ours.examples.echo.Echo"
        from = "#echo"
        to = "#routing_checks"
        as = "ours.test.Echo"
      },
```

Add yours after it, with `from = "#hello"` and no `as`: echo's renames its protocol on the way, and yours keeps its name. The client is `routing_checks`. In `system/service/init/test/routing_checks/BUILD.gn` add `//examples/hello/protocol` to its program's `deps` (line 21), and to the `use` list of its manifest (line 53) the entry `{ protocol = "ours.examples.hello.Hello" }`, in the shape of echo's (lines 85-88; echo's has a `type` because its offer renames it).

In `routing_checks.cpp` add the path the client opens, `constexpr auto HELLO = std::string_view{"/svc/ours.examples.hello.Hello"};`, beside echo's (line 97; a component's `/svc` holds one entry for each protocol it uses). Add a function `check_hello` whose one line connects and expects the epitaph: `expect("a connection to hello", connect_status(HELLO), Status::NotSupported);`. `connect_status` is `routing_checks.cpp:770-788`. Call `check_hello();` from `main` after `check_echo();` (line 904).

Verification: boot, then `grep -a 'init: \|hello: \|routing_checks: ' <dir>/rel.log`. You should find `init: started hello`, no `init: hello exited`, no `routes nowhere` line that names hello, and `routing_checks: passed`. To see that the check has teeth, expect `Status::NoResource` for one boot: the script prints `FAIL` and the log holds `routing_checks: a connection to hello = NotSupported (expected NoResource) FAILED`.

If it fails:

| You see | Cause |
|---|---|
| the build: ``exposes 'ours.examples.hello.Helo', which `capabilities` does not declare`` | `expose` and `capabilities` spell the name differently |
| the build: `'#helo' names no child of this component` | `from = "#..."` must be the child's `name` |
| `'ours_examples_hello/ours_examples_hello.hpp' file not found` in `routing_checks.cpp` | `routing_checks` does not depend on your library |
| the root logs `init: routing_checks's use of "ours.examples.hello.Hello" routes nowhere: NotFound at . (offer of "ours.examples.hello.Hello")` | the root has no offer of that name to `routing_checks` |
| the client reads `NotFound` and the root logs nothing | `HELLO_NAME` is not the name the manifests use, so the provider's directory has no such entry |

The protocol's name is spelled in six places: `capabilities`, `expose`, `HELLO_NAME`, the offer, the use and the client's path. The build catches some mismatches and a boot the rest; [the table in chapter 5](05-testing-and-debugging.md#the-table) lists what a client sees for each.

## Level 3: it answers a method

This level makes the server answer `Greet`. Each client gets a slot in the Loop, and the server reads one message at a time and writes the reply. The new idea is the *waiter*: the Loop calls an object of yours, not a function.

### The server

A slot waits on a `loop::Waiter`, and `on_signals` is what the Loop calls when the wait fires, with the signals it saw:

Its whole declaration is in [chapter 1](01-concepts.md#the-loop-and-its-slots): one virtual function, `on_signals(ours::Signals observed) -> loop::Action`, and a protected constructor and destructor so that nothing deletes a waiter through it.

It does one unit of work and answers `loop::Action::Rearm`, to wait again for the same signals, or `Close`, and then the Loop closes the handle and frees the slot. The rules it must keep are in the comment above `Waiter` and in [06-checklist.md](06-checklist.md#the-callback-that-w4-to-w9-bind); this chapter does not repeat them.

A client's channel is registered with `Loop::add(handle, signals, waiter)`. Echo's `connect` does it for each new client, asking to be woken by `Readable` (a message is waiting) or `PeerClosed` (the client is gone). `client` is an entry of a table in the server's storage, and it is a `Waiter`:

`examples/echo/echo_server.cpp:147-156`:

```cpp
            client.server = this;
            client.channel = channel;
            if (auto const added =
                    m_loop->add(channel, Signals::Readable | Signals::PeerClosed, client);
                added.is_err()) {
                client.channel = Uandle::Invalid;
                report("cannot wait on a client", added.unwrap_err());
                close_with_epitaph(channel, added.unwrap_err());
            }
            return;
```

Replace your stub class with echo's `EchoServer`, renamed:

- the class: lines 86-229 and 251-253, leaving out lines 200-203 of `serve`, the `else if` that handles `Stop`, which come back in level 4. It holds a `Client` (lines 100-113, a `Waiter` whose `on_signals` calls `serve`), a `Storage` struct, `connect`, `release` (frees the entry and answers `Close`), `serve` and `greet`;
- the limits: lines 52-61, `MAX_CLIENTS` (make it `usize{1}`: one client is all the check needs) and `SLOTS` as echo has it;
- the storage: the `ECHO_STORAGE` line (262), as `HELLO_STORAGE`, and in `main` `HelloServer server(*HELLO_STORAGE.init(), loop);` as line 283 has it.

`serve` reads the one message, decodes its header, and picks the method by its number. This is the branch for `Greet` (the `if` it opens is closed at line 204, after the `else if` for `Stop` that you leave out for now); a method it does not know falls out of the bottom, which level 4 shows:

`examples/echo/echo_server.cpp:194-199`:

```cpp
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        if (header.is_ok() && header->ordinal == ours_examples_echo::ECHO_STRING.ordinal) {
            if (auto const request = ours_examples_echo::decode_echo_string(message);
                request.is_ok()) {
                return echo_string(client, *request);
            }
```

`greet` (lines 211-229) builds the reply with the request's `txid`, the *transaction id* that a client uses to match a reply to its request, writes it, and answers `Rearm`, only after the message was read.

### The client

The client is a check in `routing_checks`. Copy `check_echo_string` as `check_hello_greet`, with `encode_greet`, `decode_greet_reply` and a `HELLO_TEXT` beside `ECHO_TEXT` (line 109). It encodes the request and calls `call` (`routing_checks.cpp:424-438`), which writes it, waits for the channel to be readable or closed, reads, and returns the status of an epitaph if the message is one:

`system/service/init/test/routing_checks/routing_checks.cpp:723-735`:

```cpp
auto check_echo_string(Uandle const connection) -> void {
    auto const txid = next_txid();
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const request =
        ours_examples_echo::encode_echo_string(bytes, {.txid = txid, .value = ECHO_TEXT});
    if (!expect("EchoString", status_of(request), Status::Ok)) {
        return;
    }
    Incoming reply;
    if (!expect("EchoString through the sibling's expose",
                call(connection, request.unwrap(), reply), Status::Ok)) {
        return;
    }
```

It then decodes the reply and compares it with what was sent:

`system/service/init/test/routing_checks/routing_checks.cpp:736-742`:

```cpp
    auto const decoded = ours_examples_echo::decode_echo_string_reply(reply.message(), txid);
    if (expect("EchoString's reply", status_of(decoded), Status::Ok) &&
        expect("EchoString", decoded->status, Status::Ok) && decoded->response != ECHO_TEXT) {
        auto _ = ort::debug_line({"routing_checks: echo answered \"", decoded->response, "\""});
        fail("EchoString: the answer is not what was sent");
    }
}
```

Replace level 2's check with a `check_hello` that opens the client's channel with `connect_or_fail(HELLO)` (line 407), calls `check_hello_greet`, prints `routing_checks: hello checked` if the count of failures, `FAILURES`, did not grow, and closes the channel.

Verification: boot. Among the lines `grep -a 'init: \|hello: \|routing_checks: ' <dir>/rel.log` prints you should find:

```text
routing_checks: hello checked
routing_checks: passed
```

If the server answers another string, the log says `routing_checks: hello answered "<what it said>"` and `routing_checks: Greet: the answer is not what was sent FAILED`, and the script prints `FAIL`.

## Level 4: it keeps its limits

This level makes the server answer what it cannot do. A server has no heap, so each table is full at some point, and a client must never be left waiting for an answer that cannot come. Four cases, one snippet each; the first three are lines of the `connect` and `serve` you already copied.

- **A client past `MAX_CLIENTS`.** A free entry takes the client and returns. When the loop over the entries finds none, `connect` falls out of it to a refusal with an epitaph, and the client reads `NoResource`:

`examples/echo/echo_server.cpp:156-159`:

```cpp
            return;
        }
        close_with_epitaph(channel, Status::NoResource);
    }
```

- **A method it does not know**, or a message its decoder refuses. `serve` falls out of its `if` and `else if` (lines 194-204) to these lines: close any handles the message carried, free the entry, answer `Close`. Nothing is written, so the client reads `PeerClosed` and no epitaph. This is what makes `Hello` a closed protocol ([03-protocols.md](03-protocols.md#closed-protocols)):

`examples/echo/echo_server.cpp:205-208`:

```cpp
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
        return release(client);
```

- **A reply that cannot be written.** The client would wait for ever, so its entry is freed and its channel closed. No check of yours makes a write fail, so this one is read, not run:

`examples/echo/echo_server.cpp:223-227`:

```cpp
        // A reply that cannot be written leaves its client waiting for good,
        // so the connection goes with it.
        if (auto const written = write(client.channel, reply.unwrap()); written != Status::Ok) {
            return release(client);
        }
```

- **Exit.** Nothing starts a component again once it has exited. `Stop` answers Ok and then ends the process, whether or not the answer could be written. The Loop has no quit, so ending the program from inside `on_signals` is `process_exit`:

`examples/echo/echo_server.cpp:240-249`:

```cpp
    [[noreturn]]
    auto stop(Client const &client, ours_examples_echo::StopRequest const &request) -> void {
        if (auto const reply = ours_examples_echo::encode_stop_reply(m_storage->reply_bytes,
                                                                     request.txid, Status::Ok);
            reply.is_ok()) {
            auto _ = write(client.channel, reply.unwrap());
        }
        auto _ = ort::debug_line("echo: stopping, as a client asked");
        ours::process_exit(0);
    }
```

Put back lines 200-203 of `serve` and the function `stop` (lines 231-249); your class is now echo's lines 86-253, renamed. The root logs `init: hello exited with 0`, and, as for `echo`, a later route to `hello` answers `NotFound`.

Extend the client. All three checks use what `routing_checks` already has:

- While the first client is connected, `connect_status(HELLO)` must be `Status::NoResource`. Keep `MAX_CLIENTS` at 1 for this: with a larger number `hello` accepts the second client and writes nothing, the check waits for ever, and the boot fails after 60 s with `no "routing_checks: passed"` among the missing lines and no `FAILED` line.
- Send `hello` a request it has no method for: echo's own `ours_examples_echo::encode_echo_string`, with `call`. The answer is `Status::PeerClosed`, and `hello` has closed the channel.
- The channel of the first client is gone, so connect again and copy `stop_echo` (`routing_checks.cpp:746-767`) as `stop_hello`. It sends `Stop`, expects Ok and then `PeerClosed`.

Print `routing_checks: hello checked` after all three, if none failed.

Verification: boot. You should find these three lines, in an order that can differ from run to run:

```text
hello: stopping, as a client asked
init: hello exited with 0
routing_checks: hello checked
```

What a client sees is in [the table in chapter 5](05-testing-and-debugging.md#the-table): a full table is its row 10, an unknown method its row 13.

## Level 5: gate it

This level makes a regression fail the boot. The checks of levels 2 to 4 fail loudly when `hello` answers wrongly, but they say nothing if they never run: a boot that does not call `check_hello()` passes, and so does one in which `hello` never exits. The cure is to require the lines that prove it ran.

The boot check passes only if every line in `REQUIRED` appeared. This is its list for the dev image:

`.agents/skills/ours-verify/scripts/boot_check.py:90-100`:

```python
REQUIRED = {
    "dbg": [],
    "rel": [
        "routing_checks: passed",
        "init: echo exited with 0",
        "init: holding the IRQ and I/O port resources",
        "pc-ps2: ready",
        "pc-ps2: key a",
        "pc-ps2: key A",
    ],
}
```

Add `"init: hello exited with 0"` and `"routing_checks: hello checked"` after echo's line. Both lines are printed only when `hello` was reached, answered, refused, and stopped, so they are the proof.

To see that they bite, take the call to `check_hello()` out of `main`. Without the new lines the boot still passes, with `6/6 required`. With them the script prints:

```text
rel        FAIL  no "init: hello exited with 0", "routing_checks: hello checked" within 60s · 41 passed, 8 skipped, 0 failed · 6/8 required · 0 error line(s) · reached: end
```

Put the call back. The full command, with the host tests, now ends in `8/8 required`. A check you have never seen fail proves nothing.

Then read your server once against [the checklist](06-checklist.md#what-a-first-version-needs), which picks the items a first version cannot do without, and the walks of [its review section](06-checklist.md#review). Two to start with: every `return` in `serve` and `greet` is `Rearm` or `Close`, and `Rearm` only after the message was read; and every handle has one owner at each moment: the channel is the Loop's once `add` succeeds, the handles of a message you reject are closed, the port is the Loop's.

## What to skip on a first pass

- A host test of your codec: [chapter 3](03-protocols.md#6-write-the-test), steps 6 and 7.
- Tree servers, [chapter 4](04-tree-servers.md), except the sizing of storage.
- The tables of failures in [chapter 5](05-testing-and-debugging.md) until a boot fails.
- The items of [chapter 6](06-checklist.md) beyond the first-version list, and handles in messages ([chapter 3](03-protocols.md#handles-in-messages)): `hello` carries none.

## Where to look next

- [03-protocols.md](03-protocols.md): a second method, a reply that carries a value, the layout of a message, and the host test of your codec.
- [05-testing-and-debugging.md](05-testing-and-debugging.md): reading the root's log when a route fails.
- [06-checklist.md](06-checklist.md): the whole list, for design and review.
- [04-tree-servers.md](04-tree-servers.md): when what you offer is files and directories.

## Sources

- `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn`, `examples/echo/protocol/` (`BUILD.gn`, the header): the program, its targets and its protocol library, which `hello` copies.
- `system/lib/loop/include/loop/loop.hpp`, `system/lib/ioservice/include/ioservice/outgoing.hpp`: `Waiter`, `Loop::add`, `Handler`.
- `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn`, `system/service/init/test/routing_checks/routing_checks.cpp`: the root's manifest, the client's manifest and the client's checks.
- `.agents/skills/ours-verify/scripts/boot_check.py`: the lines a boot requires.
- `system/lib/wire/test/ordinals_test.py`, `system/lib/wire/BUILD.gn`: the method numbers.
- `system/component_manifest.gni`, `system/component_manifest.py`, `system/user_executable.gni`: the manifest and program templates, the `.cm` name, `startup`, and where a program is installed.
- `system/lib/ort/include/ort/main.hpp`, `system/lib/ort/include/ort/debug.hpp`, `system/lib/ort/src/start.cpp`, `system/lib/ustl/include/ustl/lazy_init.hpp`: the runtime's `main`, its debug line, and why storage is `constinit`.

Checked against commit `3cb0d8a5` on 2026-10-29.
