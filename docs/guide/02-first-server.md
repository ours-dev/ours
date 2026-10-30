# Your first server

This chapter is a tutorial. You copy `examples/echo`, the small server that ships in the tree, call the copy `hello`, cut it down to almost nothing, and grow it back in five levels (level 2 in two halves), until the root starts it, a client reaches it, and every boot checks that it still answers. Each level builds, boots and says how to tell it worked, and adds one or two ideas. Words that have a meaning of their own here are linked to [01-concepts.md](01-concepts.md) at first use.

## What you will be able to do

- Start from `examples/echo` and end with a server of your own: a protocol, a program, a manifest, and the edits that make the root start it.
- Write the client side of the exchange, as a check in the dev image's test component.
- Say, for each level, what to run and what a pass looks like in the log.
- Make a boot fail when your server stops working.

## Before you start

You need a build that boots the untouched tree. [00-setup.md](00-setup.md) says what the build needs in `prebuilt/`, and [the README](README.md#build-and-boot-the-dev-image) says what the verify script does. From the root of your checkout:

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

The last line names the directory that holds `rel.log`, the serial log of the boot: `<dir>` below is that directory (here `.../ours-verify-hello`). This chapter reads the log with `grep -a '<pattern>' <dir>/rel.log`. Give `grep` the `-a`: the log holds a few NUL bytes, so `grep` takes it for a binary file and, without the flag, prints `Binary file ... matches` and no lines. If the script prints `FAIL`, the table "Reading a failure" in the verify script's documentation (named in [the README](README.md#build-and-boot-the-dev-image)) says what each message means.

Chapter 1 is read in pieces. Each level that needs one says "Read now" and links it, with its size: before level 0 the three sections [Handles and channels](01-concepts.md#handles-and-channels), [Ports and signals](01-concepts.md#ports-and-signals) and [The Loop and its slots](01-concepts.md#the-loop-and-its-slots) (about 2,100 words in all); at level 1 [Components and manifests](01-concepts.md#components-and-manifests) (about 1,200); at level 2 [`/svc` and the outgoing directory](01-concepts.md#svc-and-the-outgoing-directory) (about 900), [Where a server's memory comes from](01-concepts.md#where-a-servers-memory-comes-from) (about 450) and [Epitaphs](01-concepts.md#epitaphs) (about 450). Read the first three now.

The finished result of this chapter is in the tree, as `examples/hello`. The default build compiles it so that it stays true, and no image starts it. It is what to compare with when a build fails: after you set it aside, `git diff -- examples/hello` shows every line where your copy differs from it, and `git status examples/hello` prints nothing when the two are the same. To do the tutorial yourself, set the finished copy aside first, because the tutorial creates the same directory (with the copy still there, `cp -R` would nest yours inside it, and two protocols would have the same method numbers). Run `rm -r examples/hello`. In the root `BUILD.gn`, delete the block that starts with the comment `# The finished result of chapter 2 of the developer's guide` and ends with the line `"//examples/hello/protocol($host_toolchain)",`. `examples/BUILD.gn` names `hello` too and needs no edit: nothing reads that file once the root stops naming `//examples:hello`. If your checkout has no `examples/hello`, there is nothing to set aside, and nothing to compare with.

The line numbers in this chapter were checked against commit `3cb0d8a5`, the last commit that changed the files it quotes. `git log -1 --format=%h -- examples/echo system kernel/products .agents/skills/ours-verify/scripts` prints that commit for your checkout. If it prints another, a number may have moved: every edit below names the text it is about before it gives a number, so look for the text.

## The places you will touch

A server is spread over seven places. The table lists them in the order you meet them.

| # | What | Where | Copy or write | Level |
|---|---|---|---|---|
| 1 | The protocol library: messages and their codec | `examples/hello/protocol/` | copy `examples/echo/protocol/`, rename, then fix two numbers | 0 |
| 2 | The program, and its build target `user_executable("hello_server")` | `examples/hello/hello_server.cpp`, `examples/hello/BUILD.gn` | write a short `main`, then grow it by copying parts of `echo_server.cpp` | 1 to 4 |
| 3 | The manifest `component_manifest("hello.cm")` | `examples/hello/BUILD.gn` | copy echo's, rename | 1 and 2b |
| 4 | The root's manifest: a child entry, then an offer | `kernel/products/image/dev/BUILD.gn` | add two entries modelled on echo's | 1 and 2b |
| 5 | The client's manifest: a `use`, and a dependency on your library | `system/service/init/test/routing_checks/BUILD.gn` | add | 2b |
| 6 | The client's check | `system/service/init/test/routing_checks/routing_checks.cpp` | write, by copying echo's | 2b to 4 |
| 7 | The lines a boot must print | `REQUIRED` in `.agents/skills/ours-verify/scripts/boot_check.py` | add two lines | 5 |

The client is `routing_checks`, the dev image's test component, which already acts as echo's client. Every `file:line` in this chapter is a file of the tree as it stands, so you can open it, and code under a `file:line` is copied exactly from there. The code you write yourself is not in the tree. It is shown in blocks that carry no `file:line`, and it is what was built and booted for this chapter: the `main` of level 1, the stub class of level 2a, and the client's check in levels 2b, 3 and 4 (a few dozen lines in all). Everything else you paste is echo's, renamed.

These edits change the dev image and its test component for good, so make them on a branch you can throw away; `python3 scripts/new_worktree.py <branch>` makes a copy of the checkout under `.worktrees/`, ready to build. The finished copy is named by a group in `examples/BUILD.gn` and by the root `BUILD.gn`, so that the default build compiles it. Yours needs neither: the root's manifest is what pulls it into the image.

## Level 0: choose, and define the protocol

**Read now:** [Handles and channels](01-concepts.md#handles-and-channels), [Ports and signals](01-concepts.md#ports-and-signals) and [The Loop and its slots](01-concepts.md#the-loop-and-its-slots), about 2,100 words, if you have not.

This level decides what you are building and creates its protocol library. Nothing runs yet.

A server is a *protocol server* if it answers a protocol of its own on each client's channel, as `echo` does, and a *tree server* if it serves files and directories that a client opens by path. A first server is a protocol server; [04-tree-servers.md](04-tree-servers.md#choosing-a-tree-server-or-a-protocol-server) is for the other kind.

Yours is `hello`. Its protocol is named `ours.examples.hello.Hello` and its library `ours_examples_hello` ([03-protocols.md](03-protocols.md#names-and-the-protocol-type) explains the three names a protocol has). It has one method of its own, `Greet`, which answers with the string it was sent. The library keeps echo's second method, `Stop`, which asks the server to exit; level 4 uses it. [Chapter 3](03-protocols.md#how-to-add-a-new-protocol-library) has a longer recipe for a protocol library that is not a copy of another, made with a copier template. `hello` is a copy, so skip that recipe here and come back to it for a second method or a protocol of a different shape.

**Copy.** Make the directory and copy the library: `mkdir examples/hello`, then `cp -R examples/echo/protocol examples/hello/protocol`.

**Trim.** In the copy delete `test/` and `README.md`, and the `tests` group at the end of `BUILD.gn` with the blank line above it (`examples/echo/protocol/BUILD.gn:37-42`). A host test of the codec is steps 6 and 7 of [the recipe in chapter 3](03-protocols.md#6-write-the-test); the tests in this chapter are boots.

**Rename.** Rename the directory `include/ours_examples_echo` and the header inside it, and `src/ours_examples_echo.cpp`, with `hello` for `echo`. Then apply this table to every file of the copy, `.clang-format` included. Apply every row, in order. It is also the table for everything else you copy from echo in this chapter, the program and the client's check, which is why it has rows that the library does not need.

| Echo's | Yours |
|---|---|
| `ours_examples_echo`, `OURS_EXAMPLES_ECHO` | `ours_examples_hello`, `OURS_EXAMPLES_HELLO` |
| `ours.examples.echo`, `//examples/echo` | `ours.examples.hello`, `//examples/hello` |
| `Echo.EchoString`, `EchoString`, `ECHO_STRING`, `echo_string` | `Hello.Greet`, `Greet`, `GREET`, `greet` |
| `ECHO_NAME`, `ECHO_STORAGE`, `ECHO_TEXT` | `HELLO_NAME`, `HELLO_STORAGE`, `HELLO_TEXT` |
| `Echo` | `Hello` |
| `echo`, in lower case, whatever is left | `hello` |

`grep -ri echo examples/hello/protocol` prints nothing when the renames are done. The comments at the top of the header and of `BUILD.gn` still cite design notes that are echo's. Leave them while you follow this chapter, so that your files equal the finished copy; rewrite them to say what your protocol is when the server is yours.

**Fix the method numbers.** A method's number (the code calls it the `ordinal`) is computed from the selector string beside it, and the copy still carries echo's two numbers ([03-protocols.md](03-protocols.md#methods-and-their-numbers) says why). This is how echo declares a method:

`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:56-60`:

```cpp
inline constexpr auto ECHO_STRING = wire::Method{
    .selector = "ours.examples.echo/Echo.EchoString",
    .ordinal = 0x6e80'6bd7'7638'6789,
    .kind = wire::MethodKind::TwoWay,
};
```

Run `python3 system/lib/wire/test/ordinals_test.py`. It fails and names the right number for each selector, in a line like `...: ours.examples.hello/Hello.Greet is 0x0b34f151d5b5de80, not 0x6e806bd776386789`. Two of its failures say a number is not unique: they clear when both numbers are replaced. Paste each number into the `.ordinal` of its method, with the digits grouped in fours by `'` as the file has them (`0x0b34'f151'd5b5'de80` for `Greet`, `0x3b42'cec6'd122'813b` for `Stop`), and run the test again until it ends with `OK`.

Verification: the test prints `OK`. It is all this level can show, because nothing builds the library until level 2. Neither the default build nor a boot runs this test (`system/lib/wire/BUILD.gn:26-27`), so a wrong number would still boot.

## Level 1: the root starts it

**Read now:** [Components and manifests](01-concepts.md#components-and-manifests), about 1,200 words.

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

A `BUILD.gn` file declares build targets, which other files name by label: `//examples/hello:hello.cm` is the target `hello.cm` in `examples/hello/BUILD.gn`, and `//` is the root of the repository. Copy `examples/echo/BUILD.gn` to `examples/hello/BUILD.gn` and apply the table of level 0 to it (it turns `echo_server` into `hello_server`, `echo.cm` into `hello.cm`, and the name in the `assert`). For this level also delete the program's `deps` and the whole `manifest = { ... }` of the manifest (lines 24-28 and 36-48 of echo's file; they are the next section's pieces). Last, delete the comments, which are echo's, and the blank line they leave at the top of the file. Keep the `import` lines and the `assert` (lines 5-7 and 13-15), which stops the build reading the file for anything but programs that run in user mode.

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
- `deps` are the libraries it links. `main` needs none yet; level 2a puts them back.

The manifest is a `component_manifest`, and the target's name must end in `.cm`, the extension of a compiled component manifest:

`examples/echo/BUILD.gn:33-35`:

```gn
component_manifest("echo.cm") {
  visibility = [ "//kernel/products/image/*" ]
  program = ":echo_server"
```

- The name becomes `meta/hello.cm` in the image.
- `visibility` lets only the image's own targets depend on it, here the root's manifest. Yours also lists `//examples/*`, one entry to a line with a trailing comma, because the repository builds its own finished copy of this chapter from a group under `//examples`; a server of your own needs only the first entry.
- `program` is the label of the program, with a bare `:` for "in this file".

Level 2b adds the `manifest = { ... }` scope. Next, the root has to start the component. The root's manifest lists its children, and echo's entry is:

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

**Read now:** [`/svc` and the outgoing directory](01-concepts.md#svc-and-the-outgoing-directory) (about 900 words), [Where a server's memory comes from](01-concepts.md#where-a-servers-memory-comes-from) (about 450) and [Epitaphs](01-concepts.md#epitaphs) (about 450).

This level makes `hello` a server that a client can reach, in two halves. Level 2a is the program: it serves its outgoing directory and registers its protocol, and turns every client away with an epitaph. Level 2b is the route: the manifests that say who may reach it, and a check in the client that expects the refusal.

### Level 2a: the program serves its outgoing directory

This half makes `hello_server.cpp` a server. Two ideas are new: serving a protocol, and where a program's memory comes from. Nothing can reach the server yet, so the proof is that it stays up.

The root gives every component that has a program the server end of its *outgoing directory*, as a startup handle. Echo's `main` takes it first (`examples/echo/echo_server.cpp:269`). It then makes a port and a Loop to wait on, and an `ioservice::Outgoing`, which serves the outgoing directory on that Loop:

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

You derive from it. `connect` receives the channel of each client and owns it: it serves the channel or closes it. The destructor is protected and not virtual because nothing is ever deleted here. For now `connect` refuses every client with an epitaph, the last message on a channel, which carries a status. This stub is yours; it goes between `close_with_epitaph` and the storage:

```cpp
class HelloServer final: public ioservice::Handler {
  private:
    auto connect(Uandle const channel) -> void override {
        close_with_epitaph(channel, Status::NotSupported);
    }
};
```

Build `hello_server.cpp` from echo's, with the rename table of level 0 applied to everything you paste. Each row says where a piece starts and ends in echo's file (the numbers are those of the checked commit) and what else changes:

| From `echo_server.cpp` | What | Change |
|---|---|---|
| the first `#include` to `using ours::Uandle;` (27-50) | includes, `namespace {` and the `using`s | none |
| `constexpr auto SLOTS` with the comment above it, and the `static_assert` (56-61) | the Loop's slot count | `SLOTS` is `ioservice::Outgoing::MAX_CONNECTIONS` for now: a slot for each connection to the outgoing directory |
| from the comment `/// Says |what| failed` to the end of `close_with_epitaph` (63-84) | `report`, `write`, `close_with_epitaph` | none |
| from the comment `// What the server keeps` to `} // namespace` (255-264) | the storage | leave out the `ECHO_STORAGE` line for now |
| from `auto main(int, char **) -> int {` to its closing brace (266-300) | `main` | `HelloServer server;` replaces the `EchoServer server(...)` line |

The table of level 0 changes exactly these names in echo's program: `ours_examples_echo` (the header and the namespace); `ECHO_NAME`, `ECHO_STRING` and `ECHO_STORAGE`; `EchoServer` and `EchoStringRequest`; `decode_echo_string`, `encode_echo_string_reply` and the method `echo_string`; and the three strings that start `echo:`, which the log shows as `hello:`. `STOP`, `StopRequest`, `decode_stop` and `encode_stop_reply` stay as they are.

A program here has no heap, and nothing runs the constructors of its global objects. So its tables are globals that are built with the program, marked `constinit`, and a type with a destructor sits in a `ustl::LazyInit`, raw storage that `main` builds the object in, once, with `init()`. These are echo's three:

`examples/echo/echo_server.cpp:261-263`:

```cpp
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<EchoServer::Storage> ECHO_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
```

[04-tree-servers.md](04-tree-servers.md#storage-you-lend-and-why-it-is-constinit) says how to size a larger storage.

In `examples/hello/BUILD.gn` put the program's `deps` back (`protocol` is your library, found by its directory; the list is lines 24-28 of echo's file). If the build stops with `'ioservice/outgoing.hpp' file not found`, they are not back.

Verification: boot, then `grep -a 'init: \|hello: ' <dir>/rel.log`. `init: started hello` is there, and `init: hello exited` is not: the program no longer returns from `main`, it waits in the Loop. The boot still reports `6/6 required`.

### Level 2b: the manifests and the route

This half says who may reach `hello`, and checks it from the client. It edits four files. Three ideas are new: `capabilities` and `expose` in the component's own manifest, the `offer` of its parent, and the client's `use`.

In `examples/hello/BUILD.gn` add the manifest's scope, which says what the component provides and hands to its parent:

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

In `routing_checks.cpp` add the path the client opens, `constexpr auto HELLO = std::string_view{"/svc/ours.examples.hello.Hello"};`, beside echo's (line 97; a component's `/svc` holds one entry for each protocol it uses). Add this function after `check_echo` (which ends at line 826), in the same anonymous namespace. `connect_status` is `routing_checks.cpp:770-788`; it connects and returns the status of the epitaph the server answers with, or `PeerClosed` if the channel closes with none:

```cpp
auto check_hello() -> void {
    expect("a connection to hello", connect_status(HELLO), Status::NotSupported);
}
```

Call `check_hello();` from `main` after `check_echo();` (line 904).

Verification: boot, then `grep -a 'init: \|hello: \|routing_checks: ' <dir>/rel.log`. You should find `init: started hello`, no `init: hello exited`, no `routes nowhere` line that names hello, and `routing_checks: passed`. To see that the check has teeth, expect `Status::NoResource` for one boot: the script prints `FAIL` and the log holds `routing_checks: a connection to hello = NotSupported (expected NoResource) FAILED`.

If it fails:

| You see | Cause |
|---|---|
| the build: ``exposes 'ours.examples.hello.Helo', which `capabilities` does not declare`` | `expose` and `capabilities` spell the name differently |
| the build: `'#helo' names no child of this component` | `from = "#..."` must be the child's `name` |
| the root logs `init: routing_checks's use of "ours.examples.hello.Hello" routes nowhere: NotFound at . (offer of "ours.examples.hello.Hello")` | the root has no offer of that name to `routing_checks` |
| the client reads `NotFound` and the root logs nothing | `HELLO_NAME` is not the name the manifests use, so the provider's directory has no such entry |

The protocol's name is spelled in six places: `capabilities`, `expose`, `HELLO_NAME`, the offer, the use and the client's path. The build catches some mismatches and a boot the rest; [the table in chapter 5](05-testing-and-debugging.md#the-table) lists what a client sees for each.

## Level 3: it answers a method

This level makes the server answer `Greet`. Each client gets a slot in the Loop, and the server reads one message at a time and writes the reply. The new idea is the *waiter*: the Loop calls an object of yours, not a function.

### The server

A slot waits on a `loop::Waiter`, and `on_signals` is what the Loop calls when the wait fires, with the signals it saw. Its whole declaration is in [chapter 1](01-concepts.md#the-loop-and-its-slots): one virtual function, `on_signals(ours::Signals observed) -> loop::Action`, and a protected constructor and destructor so that nothing deletes a waiter through it.

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

Replace the stub with echo's class, and its limits, renamed with the table of level 0:

- **The class.** From the comment `/// The Echo server: the connections` above `class EchoServer` to the end of `echo_string()` (its last statement is `return loop::Action::Rearm;`), then the two members and the closing brace (`Storage *m_storage;`, `loop::Loop *m_loop;`, `};`). At the checked commit that is lines 86-229 and 251-253. It holds a `Client` (a `Waiter` whose `on_signals` calls `serve`), a `Storage` struct, `connect`, `release` (frees the entry and answers `Close`), `serve` and `echo_string`, which the table renames `greet`.
- **What to leave out.** In `serve`, the `else if` that handles `Stop`: the four lines from `} else if (header.is_ok() && header->ordinal == ours_examples_echo::STOP.ordinal) {` (lines 200-203). The `}` after them closes the `if` and stays. The branch comes back in level 4.
- **The limits.** Replace your `SLOTS` lines by echo's, from the comment `/// How many clients may be connected at once` to the `static_assert` (lines 52-61). Make `MAX_CLIENTS` `usize{1}`: one client is all the check needs, and level 4 relies on it.
- **The storage.** Put the `ECHO_STORAGE` line back (it is `HELLO_STORAGE` now), and in `main` replace `HelloServer server;` by echo's `EchoServer server(*ECHO_STORAGE.init(), loop);` (line 283), renamed.

(`serve` is echo's name for the function that handles one message; it has nothing to do with serving the outgoing directory.) It reads the one message, decodes its header, and picks the method by its number. This is the branch for `Greet`; the `if` it opens is closed after the `else if` you leave out, and a method it does not know falls out of the bottom, which level 4 shows:

`examples/echo/echo_server.cpp:194-199`:

```cpp
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        if (header.is_ok() && header->ordinal == ours_examples_echo::ECHO_STRING.ordinal) {
            if (auto const request = ours_examples_echo::decode_echo_string(message);
                request.is_ok()) {
                return echo_string(client, *request);
            }
```

`greet` (echo's `echo_string`, lines 211-229) builds the reply with the request's `txid`, the *transaction id* that a client uses to match a reply to its request, writes it, and answers `Rearm`, only after the message was read.

### The client

The client is a check in `routing_checks`. First the include: add `#include <ours_examples_hello/ours_examples_hello.hpp>` after the line `#include <ours_examples_echo/ours_examples_echo.hpp>` (line 68). It resolves because `routing_checks` has depended on your library since level 2b. Then add `constexpr auto HELLO_TEXT = std::string_view{"routing_checks"};` after `ECHO_TEXT` (line 109); any string of at most 64 bytes will do.

Copy `check_echo_string` (lines 723-742) next to `check_echo`, apply the table of level 0, and name the copy `check_hello_greet` (the table makes it `check_greet`). It encodes the request and calls `call` (`routing_checks.cpp:424-438`), which writes it, waits for the channel to be readable or closed, reads, and returns the status of an epitaph if the message is one:

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

Replace level 2b's `check_hello` by this one, after `check_hello_greet`. It connects, greets, prints `routing_checks: hello checked` if the count of failures, `FAILURES`, did not grow, and closes the channel:

```cpp
auto check_hello() -> void {
    auto const failures_before = FAILURES;
    auto const connection = connect_or_fail(HELLO);
    if (connection == Uandle::Invalid) {
        return;
    }
    check_hello_greet(connection);
    if (FAILURES == failures_before) {
        auto _ = ort::debug_line("routing_checks: hello checked");
    }
    auto _ = ours::handle_close(connection);
}
```

Verification: boot. Among the lines `grep -a 'init: \|hello: \|routing_checks: ' <dir>/rel.log` prints you should find:

```text
routing_checks: hello checked
routing_checks: passed
```

If it fails:

| You see | Cause |
|---|---|
| the build: ``error: use of undeclared identifier 'ours_examples_hello'; did you mean 'ours_examples_echo'?`` in `routing_checks.cpp` | the `#include` of your header is missing |
| the build: `'ours_examples_hello/ours_examples_hello.hpp' file not found` | the include is there and `routing_checks` does not depend on your library: level 2b's `deps` line |
| the build: ``error: use of undeclared identifier 'ECHO_STORAGE'; did you mean 'HELLO_STORAGE'?`` in `hello_server.cpp` | a name that the table of level 0 renames was left as echo has it; the error names the line |
| the log has `routing_checks: hello answered "<what it said>"` and `routing_checks: Greet: the answer is not what was sent FAILED` | the server answered another string than it was sent |

If the table does not name your error, `git diff -- examples/hello` against the finished copy shows where your `hello_server.cpp` differs.

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

- **A method it does not know**, or a message its decoder refuses. `serve` falls out of its `if` and `else if` to these lines: close any handles the message carried, free the entry, answer `Close`. Nothing is written, so the client reads `PeerClosed` and no epitaph. This is what makes `Hello` a closed protocol ([03-protocols.md](03-protocols.md#closed-protocols)):

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

Put back what you left out: the `else if` that handles `Stop` in `serve` (lines 200-203), and the function `stop`, from its comment `/// Stop: answers Ok, then exits with 0` to its closing brace (lines 231-249), after `greet` and before the two members. Your class is now echo's again, from `class EchoServer` to its closing `};`, renamed. So is the whole file: `hello_server.cpp` is echo's file from its first `#include` to the end of `main`, renamed with the table of level 0, with `MAX_CLIENTS` `usize{1}`, and nothing else differs.

Extend the client, in `routing_checks.cpp` after `check_hello_greet`. Each function must come before the one that calls it, so write them in this order. `check_hello_unknown` sends `hello` a request it has no method for, echo's own `EchoString` request; `hello` closes the channel without a word, so `call` returns `PeerClosed`:

```cpp
auto check_hello_unknown(Uandle const connection) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const request =
        ours_examples_echo::encode_echo_string(bytes, {.txid = next_txid(), .value = HELLO_TEXT});
    if (!expect("a request of another protocol", status_of(request), Status::Ok)) {
        return;
    }
    Incoming reply;
    expect("hello's answer to a method it does not know", call(connection, request.unwrap(), reply),
           Status::PeerClosed);
}
```

`stop_hello` is `stop_echo` (lines 746-767) copied with the table of level 0: it sends `Stop`, expects Ok, then `PeerClosed`. It returns a `bool`, as `stop_echo` does, which the next function ignores. The first client greets, then checks the limit while it is still connected, and only then sends the unknown method, which ends it:

```cpp
auto check_hello_first_client() -> void {
    auto const connection = connect_or_fail(HELLO);
    if (connection == Uandle::Invalid) {
        return;
    }
    check_hello_greet(connection);
    expect("a second client of hello", connect_status(HELLO), Status::NoResource);
    check_hello_unknown(connection);
    auto _ = ours::handle_close(connection);
}
```

The first client's channel is gone, so a second client connects and stops `hello`:

```cpp
auto check_hello_second_client() -> void {
    auto const connection = connect_or_fail(HELLO);
    if (connection == Uandle::Invalid) {
        return;
    }
    stop_hello(connection);
    auto _ = ours::handle_close(connection);
}
```

`check_hello` replaces level 3's, and prints its line after both clients, if nothing failed:

```cpp
auto check_hello() -> void {
    auto const failures_before = FAILURES;
    check_hello_first_client();
    check_hello_second_client();
    if (FAILURES == failures_before) {
        auto _ = ort::debug_line("routing_checks: hello checked");
    }
}
```

The order of the steps matters. The limit check must run while the first client is still connected, and the unknown method must come after it, because it makes `hello` close that channel. Swap them, or leave `MAX_CLIENTS` above 1, and `hello` has a free entry for the second client: it accepts and writes nothing, `connect_status` waits for ever, and the boot fails after 60 s with `no "routing_checks: passed"` among the lines it names and no `FAILED` line.

Verification: boot. You should find these three lines, in an order that can differ from run to run:

```text
hello: stopping, as a client asked
init: hello exited with 0
routing_checks: hello checked
```

What a client sees is in [the table in chapter 5](05-testing-and-debugging.md#the-table): a full table is its cause 10, an unknown method its cause 13.

If it fails:

| You see | Cause |
|---|---|
| the build: `error: use of undeclared identifier 'stop'` in `hello_server.cpp` | the `else if` for `Stop` is back and the function `stop` is not |
| the boot fails after 60 s with no `FAILED` line | the order of the client's steps, or `MAX_CLIENTS` above 1: see the paragraph above |

For the files under `examples/hello`, `git diff -- examples/hello` against the finished copy shows what differs, whatever the error.

## Level 5: gate it

This level makes a regression fail the boot. The checks of levels 2b to 4 fail loudly when `hello` answers wrongly, but they say nothing if they never run: a boot that does not call `check_hello()` passes, and so does one in which `hello` never exits. The cure is to require the lines that prove it ran.

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

To see that they bite, take the call to `check_hello()` out of `main`. Replace it by `static_cast<void>(&check_hello);` rather than deleting it: a function that nothing calls is a build error (``unused function 'check_hello'``). Without the new lines the boot still passes, with `6/6 required`. With them the script prints:

```text
rel        FAIL  no "init: hello exited with 0", "routing_checks: hello checked" within 60s · 41 passed, 8 skipped, 0 failed · 6/8 required · 0 error line(s) · reached: end
```

Put the call back. Run the command without `--only rel`: it boots the debug image too, so the output has a `dbg` line as well (that image starts no components), and the `rel` line now ends in `8/8 required`. A check you have never seen fail proves nothing.

Last, format what you wrote and compare it with the finished copy. Renaming `echo_string` to `greet` shortens lines, and the repository's pre-commit hook refuses C++ and GN files that are not formatted. Format the C++ files under `examples/hello` and the lines you changed in `routing_checks.cpp` with `python3 scripts/format_cpp.py <file>`, and the GN files with `python3 scripts/format_gn.py <file>`. Then `git status examples/hello` prints nothing, and `git diff -- examples/hello` prints nothing: your server is the finished copy.

Then read your server once against [the checklist](06-checklist.md#what-a-first-version-needs), which picks the items a first version cannot do without, and the walks of [its review section](06-checklist.md#review). Two to start with: every `return` in `serve` and `greet` is `Rearm` or `Close`, and `Rearm` only after the message was read; and every handle has one owner at each moment: the channel is the Loop's once `add` succeeds, the handles of a message you reject are closed, the port is the Loop's.

## What to skip on a first pass

- A host test of your codec: [chapter 3](03-protocols.md#6-write-the-test), steps 6 and 7.
- Tree servers, [chapter 4](04-tree-servers.md), except the sizing of storage.
- The tables of failures in [chapter 5](05-testing-and-debugging.md) until a boot fails.
- The items of [chapter 6](06-checklist.md) beyond the ten rules and the first-version list, and handles in messages ([chapter 3](03-protocols.md#handles-in-messages)): `hello` carries none.

## Where to look next

- **A client of your own.** The client of `hello` in this chapter is a check inside `routing_checks`, because the tree has no small standalone client to copy. A client of your own is the same exchange in a component of your own: a program that connects and calls as level 3's check does, a manifest with a `use` of `ours.examples.hello.Hello`, and a child entry and an offer in the root's manifest as in levels 1 and 2b. The helpers that check uses (`Incoming`, `call`, `connect_or_fail`) sit in the anonymous namespace of `routing_checks.cpp`, so you copy them. None of this has been tried here.
- **The finished copy.** `examples/hello` is the answer to this chapter, built by the default build and started by no image. Read it when a level does not work, and diff your files against it as "Before you start" says.
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
- `scripts/new_worktree.py`: the copy of the checkout to work in. `scripts/format_cpp.py`, `scripts/format_gn.py`: the formatters.
- `examples/hello`, `BUILD.gn`, `examples/BUILD.gn`: the finished result, and how the default build names it.

Checked against commit `3cb0d8a5` on 2026-10-29.
