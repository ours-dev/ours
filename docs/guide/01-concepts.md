# Concepts every server rests on

Six ideas carry everything else in this guide: handles and channels, ports and signals, the Loop and its slots, components and manifests, `/svc` and the outgoing directory, and epitaphs. They come in that order because each one uses only what the ones before it defined. Two more sections sit among them: how a server holds its memory when a program has no heap, and the words other chapters use that this one only names. Read the chapter through once; later chapters link back to a single section when they need it.

A *server* is a component with a program that answers requests on channels (the section on components defines a component); a *client* is a program that sends it requests. The examples are from `examples/echo`, the small server the guide follows, and from `routing_checks`, the test component of the dev image ([the README](README.md#build-and-boot-the-dev-image) defines the dev image) that acts as echo's client.

## What you will be able to do

After this chapter you can:

- say what a handle, a channel, a port and a signal are, and how a program waits for several of them at once;
- say what the Loop does for a server, what a slot and a waiter are, and why a server's memory is declared the way echo's is;
- read a manifest and say who provides a protocol, who uses it, and which offer connects them;
- follow one client's request for `/svc/<name>` from the client to the server that answers it;
- tell an epitaph from a channel that simply closed.

## Handles and channels

A *process* is a running program, with a handle table of its own. A *kernel object* is a thing the kernel keeps on a process's behalf: a channel, a port, a process, a block of memory (a VMO; see [Words used elsewhere](#words-used-elsewhere)). A process never holds an object itself. It holds a *handle*: a number that picks one entry in its own handle table, and that means nothing in any other process.

In C++ a handle is an `ours::Uandle`, a scoped enum over a 32-bit integer, so that it cannot be confused with a count or a size. `Uandle::Invalid`, which is 0, is a value no handle ever has. Closing it succeeds and does nothing, so a cleanup path can close a variable without asking whether it was ever filled.

Each handle carries *rights*: what its holder may do through it. Rights belong to the handle, not to the object, so two handles to one channel can allow different things, and every system call checks the handle it was given. (These are *handle rights*. The guide uses the word for two more things, which belong to directories; [Routes](#routes) tells the three apart.)

A *system call* is a call into the kernel, such as `ours::handle_close`, which gives a handle up. It answers with a *status*, a code of the type `ours::Status`: `Ok`, or what went wrong, such as `BadHandle` or `NoResource`.

A *channel* is what servers and clients talk over: a two-ended pipe for messages. `channel_create` makes one and gives you a handle to each end. Each end has a queue of the messages the other end wrote.

```text
client process                              server process
  handle A  ---  [ end 0 | end 1 ]  ---  handle B
     write on A  ->  queue of end 1  ->  read on B
     read on A   <-  queue of end 0  <-  write on B
```

A *message* is up to 4096 bytes and up to 64 handles. The handles are the important part. A process cannot reach another process on its own, but it can send, over a channel it already holds, one end of a new channel. `channel_write` takes each handle out of the sender's table: it travels in the message, or, if the write then fails, it is closed. The reader's `channel_read` puts the handles into the reader's table. This is how a client hands a server the end it will be answered on.

`examples/echo/echo_server.cpp:69`, the server's one function that writes:

```cpp
auto write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}
```

What the bytes of a message mean is a *protocol*; the next sections and [03-protocols.md](03-protocols.md) come to it. Three facts about channels shape every server:

- **Reading does not wait.** `channel_read` answers `ShouldWait` when no message is queued, and `PeerClosed` when none is and none ever will be. A message too big for the buffer you gave is not lost: the read answers `BufferTooSmall` and leaves it queued.
- **A channel tells you when the other end is gone.** When the last handle to an end is closed, the other end raises the signal `PeerClosed` (signals are the next section; the status and the signal of that name report the same fact). A process that exits has all its handles closed, so its clients see it go. Messages the closed end wrote before it went are still delivered first.
- **Queues and handles are limited.** Each end holds at most 64 unread messages, and a write to a full end answers `NoResource`; the message is gone. A process can have at most 1024 of the handles it created open at once, wherever they now are: one sent to another process still counts against its creator.

Where this is stated:

- A handle, `Uandle::Invalid` and closing it: `system/public/ours/types.hpp:67`.
- Rights on the handle, not the object: `system/public/ours/rights.hpp:12`.
- The limits of a message and of a process's handles: `system/public/ours/syscall.hpp:43`, `:46` and `:59`.
- A channel end's queue of 64 messages: `kernel/object/include/ours/object/channel_dispatcher.hpp:28`.

## Ports and signals

A server has one channel for each client, and it must answer whichever client writes first. It cannot sit in a call that waits on one channel. It needs a way to say: wake me when any of these has something.

A *signal* is a flag that a kernel object raises to describe its state. A channel end raises `Readable` while its queue is not empty, `Writable` while its peer is alive, and `PeerClosed` once its peer is gone. A process raises `Terminated` once it is dead. The type is `ours::Signals`, a set of bits.

The simplest wait is `object_wait_one(handle, signals, deadline, &observed)`, which blocks until the object raises one of the signals. The *deadline* is 0, to look without waiting, or `ours::INSTANT_INFINITE`, to wait as long as it takes; the kernel refuses any other. A client that talks to one server can use it. This is `routing_checks` waiting for a reply or for the server to vanish (`system/service/init/test/routing_checks/routing_checks.cpp:344`):

```cpp
        auto observed = ours::Signals::None;
        if (auto const status =
                ours::object_wait_one(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
            status != Status::Ok) {
            return status;
        }
```

A server with many clients would be stuck on the first one. For it there is the *port*: a kernel object that is a queue of *packets*. Two calls do the work:

- `object_wait_async(handle, port, key, signals, options)` tells the kernel: when this object raises any of these signals, queue a packet carrying my `key` on that port.
- `port_wait(port, deadline, &packet)` takes the next packet off the port, waiting if there is none. The packet's `key` says which wait fired; for a signal wait, the packet also carries the signals the object had raised.

One port can hold waits on any number of handles, so one `port_wait` serves them all. A port holds at most 1024 waits at once, counting the ones with a packet still queued; a wait past that answers `NoResource`.

```text
channel end --(raises Readable)--> kernel --(packet with my key)--> port <--(port_wait)-- the server
```

Two properties of an async wait decide how a server must be written:

- **It is one-shot.** One packet per call; to wait again, call again.
- **It is level-triggered.** If the signal is already up when you ask, the packet is queued at once; and while the cause is still there, every new wait fires at once. A server that waits again without reading the message that raised `Readable` is woken for the same message for ever.

You will not call these two yourself; the Loop of the next section does. This is how it arms one wait (`system/lib/loop/src/loop.cpp:65`):

```cpp
auto Loop::arm(u32 const index) -> ours::Status {
    auto const &slot = m_slots[index];
    return ours::object_wait_async(slot.m_handle, m_port, Key{index, slot.m_generation}.raw(),
                                   slot.m_signals, ours::WaitAsyncOptions::None);
}
```

And this is the top of its `run`, which calls `port_wait` once for every packet it handles (`system/lib/loop/src/loop.cpp:75`):

```cpp
auto Loop::run(ours::Instant const deadline) -> ours::Status {
    while (true) {
        auto packet = ours::PortPacket{};
        if (auto const status = ours::port_wait(m_port, deadline, &packet);
            status != ours::Status::Ok) {
            return status;
        }
```

Where this is stated:

- The signals a channel end and a process raise: `system/public/ours/signals.hpp:26`.
- The deadlines, and the one-shot, level-triggered wait: `system/public/ours/syscalls.gni:295` (`object_wait_one`), `:331` (`object_wait_async`) and `:517` (`port_wait`).
- The port's limit of 1024 waits: `system/public/ours/port.hpp:167`.

## The Loop and its slots

Waiting on a port is the same few lines in every server: arm a wait for each handle, take a packet, find which handle it belongs to, handle it, arm the wait again. The *Loop* (`loop::Loop`, in `system/lib/loop`) is that code, written once.

A program here has no heap, so the Loop does not allocate ([the next section](#where-a-servers-memory-comes-from) says what that costs). You give it a port and a fixed table of `loop::Slot`, sized when you build. A *slot* holds one wait: the handle it waits on, the signals it waits for, the *waiter* to call, and a generation count. A slot is free while its handle is `Invalid`. `Loop::add(handle, signals, waiter)` puts a handle in the lowest free slot and arms its wait, or answers `NoResource` if every slot is taken. `Loop::run` is the loop itself.

A *waiter* is an object of your own that derives from `loop::Waiter` and has one function, `on_signals`. This is the whole class (`system/lib/loop/include/loop/loop.hpp:74`):

```cpp
class Waiter {
  public:
    virtual auto on_signals(ours::Signals observed) -> Action = 0;

  protected:
    constexpr Waiter() = default;
    constexpr Waiter(Waiter const &) = default;
    constexpr auto operator=(Waiter const &) -> Waiter & = default;
    constexpr ~Waiter() = default;
};
```

The Loop calls `on_signals` with the signals the wait observed, once per packet. It handles one unit of work, usually reading one message, and answers a `loop::Action`: `Rearm`, to wait again for the same signals, or `Close`, which closes the handle and frees the slot. (Other chapters call this function the callback.) The destructor is protected and not virtual because nothing is ever deleted through a `Waiter`. The slot keeps only the waiter's address, so a waiter has to stay where it is from `add` until the slot is freed. [loop.hpp](../../system/lib/loop/include/loop/loop.hpp) is the specification of the Loop.

In echo, `EchoServer::Client` is such a waiter, one for each client. `EchoServer::connect` fills in a free one and calls `m_loop->add(channel, Signals::Readable | Signals::PeerClosed, client)` (`examples/echo/echo_server.cpp:149`). [02-first-server.md](02-first-server.md) builds the whole class.

For each packet `run` does, in order:

1. Finds the slot the packet's key names. A key carries the slot's index and its generation, which is bumped each time the slot is freed, so a packet left over from before the slot was reused names nothing and is dropped.
2. Calls the slot's waiter.
3. If the answer is `Rearm`, arms the wait again; if it is `Close`, or arming again fails, closes the handle and frees the slot.

`run` returns only when waiting on the port itself fails, so a server calls it last.

Four consequences, each of which a later chapter turns into a rule:

- **One thread.** `on_signals` runs to completion before the next starts, so a server's state needs no locks. The price: a call that takes long delays every other client.
- **One message per call.** A client with more messages queued is not called again at once; its re-armed wait queues a new packet behind everyone else's. A busy client is served in turns.
- **Read before `Rearm`.** The wait is level-triggered, so a waiter that answers `Rearm` with its message still unread is called again at once, on every turn.
- **Who closes the handle.** Once `add` succeeds, the handle is the Loop's. A waiter never closes its own handle; it answers `Close` and the Loop does. If `add` fails, nothing changed and the handle is still yours, so you can still write a last message on it before closing it. The echo server does this when it cannot add a client: it writes an epitaph on the channel (a last message that says why; [Epitaphs](#epitaphs) defines it), then closes it (`examples/echo/echo_server.cpp:154`).

Echo sizes the table. `MAX_CLIENTS` is how many clients may be connected at once (the dev image's `routing_checks` uses one; a client past them is refused with `NoResource`). A slot is needed for each client and for each *connection* to the server's outgoing directory, which [a later section](#svc-and-the-outgoing-directory) explains (`examples/echo/echo_server.cpp:54`):

```cpp
constexpr auto MAX_CLIENTS = usize{4};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// client.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);
```

`examples/echo/echo_server.cpp:275` makes the port and the Loop at the start of `main`. The table itself, `SLOT_STORAGE`, is declared in [the next section](#where-a-servers-memory-comes-from):

```cpp
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
```

`Stop` is the second of echo's two methods (`EchoString`, the first, answers with the string it was sent): it ends the program from inside `on_signals`. So `examples/echo/echo_server.cpp:296` ends `main` by running the Loop, which comes back only if the port fails:

```cpp
    // Stop ends the program from inside an on_signals(); run() comes back only
    // when the port itself fails.
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
```

Where this is stated:

- The Loop's whole specification, including the order of the steps above: the comment on `Loop::run` in `system/lib/loop/include/loop/loop.hpp:204`.
- The rules for a waiter (one unit of work, never closes its own handle, `Rearm` only after reading): the comment on `Waiter`, `system/lib/loop/include/loop/loop.hpp:43`.
- One thread, nothing locked: `system/lib/ioservice/include/ioservice/outgoing.hpp:42`.

## Where a server's memory comes from

A program here has no heap. The runtime defines no `operator new`, so an accidental `new` fails at link time, and its `operator delete` stops the program on any real pointer. A server therefore cannot use `new`, `malloc` or a type that allocates, such as `std::vector`: everything it holds is memory whose size was fixed when it was built.

Two more things are missing, and they decide how that memory is declared:

- **Nothing runs the constructors of global objects.** The runtime does not walk the list of start-up initialisers, so a global that needs a constructor to run would silently start out as zeros.
- **There is no `__cxa_atexit`**, the function the compiler uses to register the destructor of a global that has one. A global of a type with a destructor would not link.

So a server keeps what it holds in a few globals that the compiler can build at compile time. This is echo's (`examples/echo/echo_server.cpp:261`):

```cpp
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<EchoServer::Storage> ECHO_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
```

- `constinit` is a C++ keyword: the variable must be initialised when the program is built, not when it starts. The compiler refuses a global that would need a constructor to run.
- `ustl::LazyInit<T>` is raw, suitably aligned bytes with a `constexpr` constructor, so it can be `constinit`, and no `T` exists in it yet. The program builds one in place, once, with `init()`, which returns a pointer to it. That is the way for a type that has a destructor: a `LazyInit` is only bytes, so the compiler registers no destructor for it. (`ustl` is the repository's own library of small standard-library-like types; see [Words used elsewhere](#words-used-elsewhere).)
- A table of a type with nothing to destroy, such as `loop::Slot`, is a plain `constinit` array.

`main` builds the two `LazyInit`s when it needs them (`examples/echo/echo_server.cpp:281`):

```cpp
    loop::Loop loop(port, SLOT_STORAGE);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    EchoServer server(*ECHO_STORAGE.init(), loop);
```

Call `init()` once for each `LazyInit`. What goes in the storage is the state the server needs for its whole life: echo's `EchoServer::Storage` holds its table of clients (the waiters of the last section) and its message buffers. A message buffer is 4 KiB, and echo's storage is about 8 KiB in all, which is one reason it is a global and not a local of `main`. Chapter 4 sizes the storage of a tree: [Storage you lend, and why it is `constinit`](04-tree-servers.md#storage-you-lend-and-why-it-is-constinit).

Where this is stated:

- No `operator new`, and an `operator delete` that stops on a real pointer: `system/lib/ort/src/cpp_dependencies.cpp:27`.
- No start-up initialisers are run: `system/lib/ort/src/start.cpp:42`.
- No `__cxa_atexit`, and why echo's storage is a `LazyInit`: the comment at `examples/echo/echo_server.cpp:255`.
- `LazyInit`: `system/lib/ustl/include/ustl/lazy_init.hpp:12`.
- Echo's storage is about 8 KiB, most of it message buffers: `examples/echo/echo_server.cpp:116`.

## Components and manifests

A program that has just started knows nobody. It holds only the handles its starter gave it, its *startup handles*, and it cannot go looking for another program's channels. So who may reach whom has to be decided in advance, in writing, by someone who can hand out the first handles. That is what components, manifests and the root are for.

### Components, manifests and the root

- A *component* is a program together with its *manifest*. A component can have *children*, other components that its manifest lists. It can also have a manifest and no program; it then only holds children.
- A *manifest* is the file that declares what a component provides, what it needs and which children it starts. It is written as a `component_manifest()` target in a `BUILD.gn`. The build checks it and compiles it into the *BootFS*, the files in the boot image that user space starts from, as `meta/<name>.cm`, beside the programs.
- The *root* is the component at the top. Its manifest is `meta/root.cm`, its children are the components of the image, and it has no program of its own. `init` is the program that runs it: `userboot`, the first user process, starts `init`. `init` reads `meta/root.cm`, starts the children in the order the manifest lists them, and decides what each may reach. Routing happens in `init` and nowhere else.

### Who may reach whom

A *capability* is anything that can be routed. A component provides protocols. (Directories are the other kind, but the only one a route can lead to is the root's `boot`, one of its [built-ins](#words-used-elsewhere).)

A *protocol* is a message format spoken over one channel, named by a string such as `ours.examples.echo.Echo`. Its *protocol type* is another string, which says which protocol a capability or a use speaks. When left out it equals the entry's name. It exists because an offer may rename a capability, after which the name no longer tells you what the protocol is.

Three words say how a protocol gets from the component that provides it to the one that needs it:

- A *use* is written in a component's own manifest: "I need this". It is always from the component's parent.
- An *offer* is written in a parent's manifest: "I give this to a child". Its source is the parent's own parent, the parent itself or another of its children, and it may rename the capability with `as`.
- An *expose* is written in a child's manifest: "I hand this up to my parent". Its source is the child itself or one of its own children.

In the dev image three components take part: `echo`, which provides a protocol, `routing_checks`, which needs it, and the root, their parent. The protocol goes up from `echo` by an expose and down to `routing_checks` by an offer, and changes its name on the way:

```text
echo (child)                  root (their parent)                 routing_checks (child)

capabilities  Echo
expose        Echo      -->   offer  Echo                   -->   use  ours.test.Echo
                                from #echo                          (type ours.examples.echo.Echo)
                                to   #routing_checks
                                as   ours.test.Echo
```

Each of the three is a `component_manifest()` target. The first is `echo.cm`, whole but for its two closing braces (`examples/echo/BUILD.gn:33`). `component_manifest("echo.cm")` names the file `meta/echo.cm`; `visibility` says who may depend on the target; `program` names the program's target; `manifest = { ... }` holds the keys. `capabilities` says "I serve this myself", and `expose` hands it up to the parent:

```gn
component_manifest("echo.cm") {
  visibility = [ "//kernel/products/image/*" ]
  program = ":echo_server"
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
```

The root's manifest is `component_manifest("root.cm")` (`kernel/products/image/dev/BUILD.gn:81`), too long to show whole. Its keys sit like this:

```text
component_manifest("root.cm")   kernel/products/image/dev/BUILD.gn:81
 |-- children                   :88    the components it starts
 `-- manifest
      `-- offer                 :111   what it gives them
```

`children` lists the components to start, each with the name the offers call it by (`#echo`), the target of its manifest, and `startup = "eager"`, which every child must state: the root starts it itself, in the order listed (`kernel/products/image/dev/BUILD.gn:88`):

```gn
  children = [
    {
      name = "routing_checks"
      manifest = "//system/service/init/test/routing_checks:routing_checks.cm"
      startup = "eager"
    },
    {
      name = "echo"
      manifest = "//examples/echo:echo.cm"
      startup = "eager"
    },
```

And in `offer` is the entry that connects the two, renaming the protocol (`kernel/products/image/dev/BUILD.gn:183`). `#echo` is the name of a child:

```gn
      # echo's protocol, from the sibling that exposes it, renamed on the
      # way: echo serves it as ours.examples.echo.Echo, and routing_checks
      # finds it at /svc/ours.test.Echo.
      {
        protocol = "ours.examples.echo.Echo"
        from = "#echo"
        to = "#routing_checks"
        as = "ours.test.Echo"
      },
```

The third manifest is `routing_checks.cm`. Its `use` list is long; this is the head of the target (`system/service/init/test/routing_checks/BUILD.gn:49`), with the first of its uses, a directory:

```gn
component_manifest("routing_checks.cm") {
  visibility = [ "//kernel/products/image/*" ]
  program = ":routing_checks"
  manifest = {
    use = [
      {
        directory = "boot"
        path = "/boot"
        rights = [ "rx*" ]
      },
```

The `rights = [ "rx*" ]` line belongs to a directory use. `rx*` is one of the rights words a manifest uses: it means read and execute, and `r*` means read only. [Routes](#routes) says what they stand for.

Further down the same list is the use of echo's protocol (`system/service/init/test/routing_checks/BUILD.gn:85`). Because the name was changed, the use states the type of protocol it means:

```gn
      {
        protocol = "ours.test.Echo"
        type = "ours.examples.echo.Echo"
      },
```

Where this is stated: `system/component_manifest.gni:5` (the manifest becomes `meta/<name>.cm`), `kernel/products/image/dev/BUILD.gn:269` (the image starts `init`), and [the component framework glossary](../architecture/component-framework/glossary.md) for use, offer and expose.

### Routes

A *route* is the walk the root makes from a use: up through the parents' offers, and down through the children's exposes, to the capability's source. It must end at a capability of the type the use states. The build checks every route it can resolve, and the root walks the route again on every request, so a route always reflects the component tree as it is now.

A walk that fails answers with a status:

- `NotFound` when something is missing: nobody offers what the use asks for, or the component that provides it has exited.
- `BadType` when it ends at another protocol than the type the use states.
- `AccessDenied`, for a directory use only, when an offer on the way gives fewer rights than the use asks for. A protocol use states no rights, so a route to a protocol never ends this way.

Those answers reach the client as epitaphs, the last section.

The word *rights* has three meanings in the guide, and the third is the one that matters here:

- *handle rights*: what a holder may do through a handle (the first section);
- *ours.io rights*: what a connection to a file or directory may do (nine of them; chapter 4's [Rights only shrink](04-tree-servers.md#rights-only-shrink));
- *the rights words* of a manifest, such as the `rx*` in the directory use above: `r*` stands for the set of ours.io rights that reads a directory, and `rx*` for those plus Execute.

When a component's process exits, the root logs `init: <moniker> exited with <code>`, where the moniker is the component's path from the root (`echo` for a child of the root). Later routes to it answer `NotFound`: a component is never restarted.

Where this is stated:

- The walk, and the three answers: `system/lib/component/include/component/route.hpp` and `system/service/init/realm.cpp:751`.
- The rights words: `system/lib/ours.io/include/ours_io/ours_io.hpp:141`.
- The exit line, and `NotFound` after it: `system/service/init/realm.cpp:858` and `:793`.

## `/svc` and the outgoing directory

The manifest says that a protocol can be reached. This section is how a client reaches it, and how the server receives the client.

The pieces on the way use the file-and-directory side of the system, so they need three words first:

- *ours.io* is the message format of files and directories: `Open`, `Close`, `Read` and a few more. A *directory channel* is a channel on which a server answers ours.io for one directory; whoever holds the client end holds that directory.
- `Open` asks a directory for the node at a path, and passes the server end of a channel for the answer. It is one-way: it has no reply to carry a failure. A failure shows up on the new channel, as an epitaph.
- A *namespace* is a process's own table from a path prefix, such as `/svc`, to the client end of a directory channel. The library `fsio` fills it before `main` from the startup handles the root sent, and provides `open()` and `connect()`.

`/svc` is the namespace entry the root serves for each component that uses a protocol, holding one entry for each protocol it uses; it is that component's view, not a registry. A client reaches a protocol by connecting to its entry. `routing_checks` does it in a few lines (`system/service/init/test/routing_checks/routing_checks.cpp:407`):

```cpp
auto connect_or_fail(std::string_view const path) -> Uandle {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (!expect("channel_create",
                ours::channel_create(ours::ChannelCreateOptions::None, &client, &server),
                Status::Ok)) {
        return Uandle::Invalid;
    }
    if (!expect(path, fsio::connect(path, server), Status::Ok)) {
        auto _ = ours::handle_close(client);
        return Uandle::Invalid;
    }
    return client;
}
```

It makes a channel, keeps one end, and gives the other, the *server end*, to `fsio::connect`. That writes an `Open` for `/svc/ours.test.Echo` on the component's `/svc` channel, with the server end in the message, and returns without waiting.

The root receives that Open. It routes the use, as the last section described. When the route ends at another component, the root does not carry any messages itself: it forwards the same server end to the component that provides the protocol, as an `Open` of `svc/<name>` on that component's *outgoing directory*. From then on the client and the server talk on the channel directly, and the root is out of the path.

```text
client (routing_checks)        root (init)                      server (echo)
  /svc/ours.test.Echo
     | Open(server end) ----->  routes the use
     |                            | Open("svc/ours.examples.echo.Echo", server end)
     |                            +------------------------->  outgoing directory
     |                                                            | the protocol's handler
     |                                                            | takes the channel
     +<================ messages on the new channel ==========>  a slot in echo's Loop
```

The *outgoing directory* is the directory a component serves for others. The root creates its channel when it resolves the component and keeps the client end; the component receives the server end as its `DirectoryRequest` startup handle, and serves it. A component without a program gets none, since nothing would serve it. Under `svc/` it holds one entry for each protocol the component provides, a *service node*: a node that, when opened, hands the request's channel to the program. `ioservice::Outgoing` (`system/lib/ioservice`) builds that directory and serves it on the Loop. It keeps a *connection* for each handle that is open on the directory or on `svc/`: a record of one open handle on a node, with a Loop slot to wait on. The `DirectoryRequest` channel it serves is the first; an open or clone of a directory through it makes another. That is why echo's table counts them.

The server registers each protocol with `add_protocol(name, handler)`. The *handler* is an object of the program's own that derives from `ioservice::Handler`, with one function, `connect`, which `Outgoing` calls with the channel of each client. This is the whole class (`system/lib/ioservice/include/ioservice/outgoing.hpp:50`):

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

`EchoServer` is echo's handler, a class in `echo_server.cpp` and no part of a library. Its `connect` gives each channel a Loop slot, as the last section showed. `examples/echo/echo_server.cpp:282`, in `main`:

```cpp
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    EchoServer server(*ECHO_STORAGE.init(), loop);

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
```

`directory_request` is taken at the top of `main` with `ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0)` (`examples/echo/echo_server.cpp:269`).

Two things are called svc, and they are opposite ends of this picture. A component's `/svc` is the namespace entry that holds what it *uses*. The `svc/` directory inside its outgoing directory holds what it *provides*. Their names for one protocol can differ: the client above asks for `ours.test.Echo`, and the server serves `ours.examples.echo.Echo`. The root translates one into the other when it routes.

An outgoing directory is a very small node tree: the directory itself, `svc/` inside it, and a service node for each protocol. A server that serves a larger tree, files and directories a client opens by path, is a tree server, and [04-tree-servers.md](04-tree-servers.md) is about it.

Where this is stated:

- `fsio::connect` writes an `Open` and does not wait: `system/lib/fsio/include/fsio/fsio.hpp:82`.
- The root forwards the Open into the provider's outgoing directory: `system/service/init/realm.cpp:789`.
- The outgoing directory, `add_protocol` and `serve`: `system/lib/ioservice/include/ioservice/outgoing.hpp:18` and `:120`.
- A component without a program gets no outgoing directory: `system/service/init/realm.cpp:364`.

## Epitaphs

An `Open` has no reply, so when it fails, the failure has nowhere to go. The answer is the *epitaph*: the last message a server writes on a channel before it closes it, carrying a status that says why. Every message begins with a 16-byte header, and two of its fields matter here: the *method number* says which method of the protocol the message is, and the *transaction id* ties a reply to its request (a reply carries its request's id, and a one-way message carries 0). An epitaph is a fixed 24-byte message, the same in every protocol: a header with transaction id 0 and, as method number, the reserved all-ones value that no method can have, and then the status (`system/lib/wire/include/wire/wire.hpp:35`). [03-protocols.md](03-protocols.md) has the whole layout.

Three kinds of code write one:

- the root, when a route fails: `NotFound` or `AccessDenied` or `BadType`, as the last section described;
- the directory server, `ioservice`, when an `Open` it receives is refused;
- a server, when it cannot take a client it was handed. `echo` closes a client it has no room for with `NoResource`.

`examples/echo/echo_server.cpp:77`:

```cpp
auto close_with_epitaph(Uandle const channel, Status const status) -> void {
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    if (auto const epitaph = wire::encode_epitaph(bytes, status); epitaph.is_ok()) {
        // The client may be gone already; the channel closes either way.
        auto _ = write(channel, epitaph.unwrap());
    }
    auto _ = ours::handle_close(channel);
}
```

A client sees the epitaph as the first message on its end, after which the channel reads `PeerClosed`. Before it decodes a message as the reply it expects, it asks whether the message is an epitaph, because an epitaph's transaction id is 0 and would not match any reply. This is `routing_checks` doing it (`system/service/init/test/routing_checks/routing_checks.cpp:369`):

```cpp
    auto epitaph() const -> ustl::Option<Status> {
        auto const header = wire::decode_header(message().bytes, wire::Direction::ToClient);
        if (header.is_err() || !wire::is_epitaph(header.unwrap())) {
            return ustl::NONE;
        }
        auto const status = wire::decode_epitaph(message());
        if (status.is_err()) {
            return ustl::some(Status::InvArgs);
        }
        return ustl::some(status.unwrap());
    }
```

Two things an epitaph is not:

- **It is not how a method reports failure.** A method that fails answers with a reply, a message that carries a status; the channel stays open. An epitaph ends the channel.
- **It is not always there.** A channel that closes with no epitaph reads `PeerClosed` and nothing more. `Echo` is a *closed protocol*: it closes the channel without an epitaph when it receives a method it does not know (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39`; [03-protocols.md](03-protocols.md#closed-protocols) has the rule). The same happens when a server exits with an `Open` still queued for it. So a client that sees `PeerClosed` alone cannot tell why from the channel. [05-testing-and-debugging.md](05-testing-and-debugging.md) lists what a client sees for each failure, and the code that produces each.

## Words used elsewhere

The other chapters use a few words that this one does not need to define in order to be followed. One entry each.

- *VMO*: a kernel object that holds a range of memory apart from any address space. A program maps it to read or write it, or hands its handle to another program. Chapter 4 puts the contents of a tree's files in VMOs (`kernel/object/include/ours/object/vmo_dispatcher.hpp:28`).
- *built-in*: a capability the root itself provides, where a component would provide the others. They are the directory `boot` (the files of the BootFS), the protocol `ours.process.Launcher` (starts programs), the `ours.resource.*` protocols (hand out kernel resources) and `ours.kernel.RootJob`. In the root's manifest a built-in is offered with `from = "parent"` (the list is `system/service/init/builtins.json`; the offers are in `kernel/products/image/dev/BUILD.gn`).
- *`ort`*: the runtime every user program links. It runs between the program's entry point and `main`, keeps the startup handles (`ort::take_startup_handle`) and provides the print helpers (`ort::debug_line`) (`system/lib/ort/README.md`).
- *`ustl`*: the repository's own library of small standard-library-like types, such as `ustl::Option`, `ustl::Result` and `ustl::LazyInit`, shared by the kernel and the user programs (`system/lib/ustl/include/ustl/`).
- *vDSO*: a small library the kernel maps into every process. A program does not issue the instruction that enters the kernel itself; it calls a function of the vDSO, which does (`system/public/ours/vdso.hpp:6`).
- *obi*: the container format of a boot image, and the extension of its file. An `.obi` file is a set of items: the kernel with the code that loads it, and the BootFS that user space starts from (`kernel/lib/obi/include/obi/package_format.hpp:17`).

## Where to look next

- [02-first-server.md](02-first-server.md): a tutorial that copies `examples/echo` into a server of your own and grows it, level by level, until the root starts it and every boot checks it.
- [03-protocols.md](03-protocols.md): what is in a message, how a method's number comes from its name, and closed protocols (`03-protocols.md#closed-protocols`).
- [04-tree-servers.md](04-tree-servers.md): serving a directory tree, and what the root's `/boot` does with an `Open`.
- [05-testing-and-debugging.md](05-testing-and-debugging.md): what a client sees for each failure, and how to read the logs.
- [06-checklist.md](06-checklist.md): the rules in this chapter, as a checklist.
- For the words in one list, the [component framework glossary](../architecture/component-framework/glossary.md); for the library layers, [libraries](../architecture/component-framework/libraries.md).

## Sources

- `system/public/ours/types.hpp`, `rights.hpp`, `signals.hpp`, `port.hpp`, `syscall.hpp`, `syscalls.gni`, `vdso.hpp`: handles, rights, signals, ports, limits, and what each system call does.
- `kernel/object/include/ours/object/channel_dispatcher.hpp`, `vmo_dispatcher.hpp`: a channel's signals and queue, and a VMO.
- `system/lib/loop/include/loop/loop.hpp`, `system/lib/loop/src/loop.cpp`, `system/lib/loop/README.md`: the Loop and its rules.
- `system/lib/ort/src/cpp_dependencies.cpp`, `system/lib/ort/src/start.cpp`, `system/lib/ort/README.md`, `system/lib/ustl/include/ustl/lazy_init.hpp`: the runtime and why a server's memory is declared as it is.
- `system/lib/wire/include/wire/wire.hpp`: the epitaph and the message header.
- `system/lib/fsio/include/fsio/fsio.hpp`, `system/lib/ours.io/README.md`, `system/lib/ours.io/include/ours_io/ours_io.hpp`, `system/lib/ioservice/README.md`, `system/lib/ioservice/include/ioservice/outgoing.hpp`: the client side, the directory format, the rights words and the outgoing directory.
- `system/lib/component/include/component/route.hpp`, `system/component_manifest.gni`, `system/service/init/builtins.json`, `system/service/init/realm.cpp`: how a route is walked, what a manifest compiles to, the built-ins, and how the root starts a component, routes and forwards an `Open`, and reports an exit.
- `docs/architecture/component-framework/glossary.md`, `docs/architecture/component-framework/libraries.md`: the words and the library layers.
- `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn`, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp`, `kernel/products/image/dev/BUILD.gn`, `kernel/lib/obi/include/obi/package_format.hpp`, `system/service/init/test/routing_checks/BUILD.gn`, `system/service/init/test/routing_checks/routing_checks.cpp`: the running example.

Checked against commit `3cb0d8a5` on 2026-10-29.
