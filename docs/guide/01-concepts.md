# Concepts every server rests on

Six ideas carry everything else in this guide: handles and channels, ports and signals, the Loop and its slots, components and manifests, `/svc` and the outgoing directory, and epitaphs. They come in that order because each one uses only what the ones before it defined. Read the chapter through once; later chapters link back to a single section when they need it.

A *server* is a component with a program that answers requests on channels (the fourth section defines a component); a *client* is a program that sends it requests. The examples are from `examples/echo`, the small server the guide follows, and from `routing_checks`, the dev image's test component that acts as its client.

## What you will be able to do

After this chapter you can:

- say what a handle, a channel, a port and a signal are, and how a program waits for several of them at once;
- say what the Loop does for a server and what a slot is;
- read a manifest and say who provides a protocol, who uses it, and which offer connects them;
- follow one client's request for `/svc/<name>` from the client to the server that answers it;
- tell an epitaph from a channel that simply closed.

## Handles and channels

A *process* is a running program, with a handle table of its own. A *kernel object* is a thing the kernel keeps on a process's behalf: a channel, a port, a process, a block of memory. A process never holds an object itself. It holds a *handle*: a number that picks one entry in its own handle table, and that means nothing in any other process.

In C++ a handle is an `ours::Uandle`, a scoped enum over a 32-bit integer, so that it cannot be confused with a count or a size. `Uandle::Invalid`, which is 0, is a value no handle ever has. Closing it succeeds and does nothing, so a cleanup path can close a variable without asking whether it was ever filled (`system/public/ours/types.hpp:68`).

Each handle carries *rights*: what its holder may do through it. Rights belong to the handle, not to the object, so two handles to one channel can allow different things, and every system call checks the handle it was given (`system/public/ours/rights.hpp:12`). A *system call* is a call into the kernel, such as `ours::handle_close`, which gives a handle up. It answers with a *status*, a code of the type `ours::Status`: `Ok`, or what went wrong, such as `BadHandle` or `NoResource`.

A *channel* is what servers and clients talk over: a two-ended pipe for messages. `channel_create` makes one and gives you a handle to each end. Each end has a queue of the messages the other end wrote.

```text
client process                              server process
  handle A  ---  [ end 0 | end 1 ]  ---  handle B
     write on A  ->  queue of end 1  ->  read on B
     read on A   <-  queue of end 0  <-  write on B
```

A *message* is up to 4096 bytes and up to 64 handles (`system/public/ours/syscall.hpp:43`). The handles are the important part. A process cannot reach another process on its own, but it can send, over a channel it already holds, one end of a new channel. `channel_write` takes each handle out of the sender's table: it travels in the message, or, if the write then fails, it is closed. The reader's `channel_read` puts the handles into the reader's table. This is how a client hands a server the end it will be answered on.

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
- **A channel tells you when the other end is gone.** When the last handle to an end is closed, the other end raises the signal `PeerClosed` (signals are the next section). A process that exits has all its handles closed, so its clients see it go. Messages the closed end wrote before it went are still delivered first.
- **Queues and handles are limited.** Each end holds at most 64 unread messages, and a write to a full end answers `NoResource`; the message is gone (`kernel/object/include/ours/object/channel_dispatcher.hpp:28`). A process may have created at most 1024 live handles, wherever they are now (`system/public/ours/syscall.hpp:59`).

## Ports and signals

A server has one channel for each client, and it must answer whichever client writes first. It cannot sit in a call that waits on one channel. It needs a way to say: wake me when any of these has something.

A *signal* is a flag that a kernel object raises to describe its state. A channel end raises `Readable` while its queue is not empty, `Writable` while its peer is alive, and `PeerClosed` once its peer is gone. A process raises `Terminated` once it is dead. The type is `ours::Signals`, a set of bits (`system/public/ours/signals.hpp:26`).

The simplest wait is `object_wait_one(handle, signals, deadline, &observed)`, which blocks until the object raises one of the signals. A client that talks to one server can use it. This is `routing_checks` waiting for a reply or for the server to vanish (`system/service/init/test/routing_checks/routing_checks.cpp:344`):

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

One port can hold waits on any number of handles, so one `port_wait` serves them all.

```text
channel end --(raises Readable)--> kernel --(packet with my key)--> port <--(port_wait)-- the server
```

Two properties of an async wait decide how a server must be written:

- **It is one-shot.** One packet per call; to wait again, call again.
- **It is level-triggered.** If the signal is already up when you ask, the packet is queued at once; and while the cause is still there, every new wait fires at once. A server that waits again without reading the message that raised `Readable` is woken for the same message for ever.

A deadline is 0, to look without waiting, or `ours::INSTANT_INFINITE`, to wait as long as it takes; the kernel refuses any other (`system/public/ours/syscalls.gni:520`). A port holds at most 1024 waits at once, counting the ones with a packet still queued; a wait past that answers `NoResource` (`system/public/ours/port.hpp:167`).

You will not call these two yourself; the Loop of the next section does. This is how it arms one wait (`system/lib/loop/src/loop.cpp:85`), and its `run` calls `port_wait` once for every packet it handles:

```cpp
auto Loop::arm(u32 const index) -> ours::Status {
    auto const &slot = m_slots[index];
    return ours::object_wait_async(slot.m_handle, m_port, Key{index, slot.m_generation}.raw(),
                                   slot.m_signals, ours::WaitAsyncOptions::None);
}
```

## The Loop and its slots

Waiting on a port is the same few lines in every server: arm a wait for each handle, take a packet, find which handle it belongs to, handle it, arm the wait again. The *Loop* (`loop::Loop`, in `system/lib/loop`) is that code, written once.

A program here has no heap, so the Loop does not allocate. You give it a port and a fixed table of `loop::Slot`, sized when you build. A *slot* holds one wait: the handle it waits on, the signals it waits for, what to call when the wait fires, and a generation count. A slot is free while its handle is `Invalid`. `Loop::add` puts a handle in the lowest free slot and arms its wait, or answers `NoResource` if every slot is taken. `Loop::run` is the loop itself.

*What to call* is a *callback*: the Loop calls it with the signals the wait observed, once per packet. It handles one unit of work, usually reading one message, and answers `Rearm`, to wait again for the same signals, or `Close`, which closes the handle and frees the slot. The exact form of a callback is in [loop.hpp](../../system/lib/loop/include/loop/loop.hpp), which is also the specification of the Loop.

For each packet `run` does, in order:

1. Finds the slot the packet's key names. A key carries the slot's index and its generation, which is bumped each time the slot is freed, so a packet left over from before the slot was reused names nothing and is dropped.
2. Calls the slot's callback.
3. If the answer is `Rearm`, arms the wait again; if it is `Close`, or arming again fails, closes the handle and frees the slot.

`run` returns only when waiting on the port itself fails, so a server calls it last.

Four consequences, each of which a later chapter turns into a rule:

- **One thread.** A callback runs to completion before the next starts, so a server's state needs no locks (`system/lib/ioservice/include/ioservice/outgoing.hpp:42`). The price: a callback that takes long delays every other client.
- **One message per callback.** A client with more messages queued is not called again at once; its re-armed wait queues a new packet behind everyone else's. A busy client is served in turns.
- **Read before `Rearm`.** The wait is level-triggered, so a callback that answers `Rearm` with its message still unread is called again at once, on every turn.
- **Who closes the handle.** Once `add` succeeds, the handle is the Loop's. A callback never closes its own handle; it answers `Close` and the Loop does. If `add` fails, nothing changed and the handle is still yours, so you can still write a last message on it before closing it. The echo server does this when it cannot add a client: it writes an epitaph on the channel, then closes it (`examples/echo/echo_server.cpp:132`).

`examples/echo/echo_server.cpp:52` sizes the table. A slot is needed for each client and for each connection to the server's outgoing directory, which a later section explains:

```cpp
/// How many clients may be connected at once. One is all dev's routing
/// checks use; a client past them is refused with NoResource.
constexpr auto MAX_CLIENTS = usize{4};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// client.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);
```

`examples/echo/echo_server.cpp:258` makes the port and the Loop at the start of `main`:

```cpp
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
```

And `examples/echo/echo_server.cpp:279` ends `main` by running it:

```cpp
    // Stop ends the program from inside a callback; run() comes back only
    // when the port itself fails.
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
```

## Components and manifests

A program that has just started knows nobody. It holds only the handles its starter gave it, and it cannot go looking for another program's channels. So who may reach whom has to be decided in advance, in writing, by someone who can hand out the first handles. That is what components, manifests and the root are for.

- A *component* is a program together with its *manifest*. A component can have *children*, other components that its manifest lists. It can also have a manifest and no program, as the root does; it then only holds children.
- A *manifest* is the file that declares what a component provides, what it needs and which children it starts. It is written as a `component_manifest()` target in a `BUILD.gn`. The build checks it and compiles it into the boot image, the set of files the system boots from, as `meta/<name>.cm`, beside the programs (`system/component_manifest.gni:5`).
- The *root* is the program that starts the components and decides what each may reach. On the dev image it is `init`, which `userboot`, the first user process, starts (`kernel/products/image/dev/BUILD.gn:269`). It reads `meta/root.cm`, whose children are the components of the image, and starts them in the order the manifest lists them. Routing happens in the root and nowhere else.
- A *capability* is anything that can be routed. A component provides protocols. (Directories are the other kind, but today the only one is the root's built-in `boot`.)
- A *protocol* is a message format spoken over one channel, named by a string such as `ours.examples.echo.Echo`. Its *protocol type* is another string, which says which protocol a capability or a use speaks. When left out it equals the entry's name. It exists because an offer may rename a capability, after which the name no longer tells you what the protocol is.

Three words say how a protocol gets from the component that provides it to the one that needs it:

- A *use* is written in a component's own manifest: "I need this". It is always from the component's parent.
- An *offer* is written in a parent's manifest: "I give this to a child". Its source is the parent's own parent, the parent itself or another of its children, and it may rename the capability with `as`.
- An *expose* is written in a child's manifest: "I hand this up to my parent". Its source is the child itself or one of its own children.

In the dev image three components matter here: the root, `echo`, which provides a protocol, and `routing_checks`, which needs it. The example is split across three manifests. The first declares the protocol and exposes it (`examples/echo/BUILD.gn:35`):

```gn
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
  }
```

The second is the root's, which offers echo's protocol to `routing_checks`, its sibling (another child of the root), under another name (`kernel/products/image/dev/BUILD.gn:186`). `#echo` is the name of a child:

```gn
      {
        protocol = "ours.examples.echo.Echo"
        from = "#echo"
        to = "#routing_checks"
        as = "ours.test.Echo"
      },
```

The third is `routing_checks`' use of it (`system/service/init/test/routing_checks/BUILD.gn:85`). Because the name was changed, the use states the type of protocol it means:

```gn
      {
        protocol = "ours.test.Echo"
        type = "ours.examples.echo.Echo"
      },
```

```text
root (init)           root.cm:  offer  ours.examples.echo.Echo  from #echo  to #routing_checks  as ours.test.Echo
 |-- echo             echo.cm:  capabilities + expose  ours.examples.echo.Echo  (from self)
 `-- routing_checks   routing_checks.cm:  use  ours.test.Echo  (type ours.examples.echo.Echo)
```

A *route* is the walk the root makes from a use: up through the parents' offers, and down through the children's exposes, to the capability's source. It must end at a capability of the type the use states. The build checks every route it can resolve, and the root walks the route again on every request, so a route always reflects the tree as it is now. A walk that fails answers `NotFound` when something is missing, `AccessDenied` when rights are narrowed, and `BadType` when it ends at another protocol. Those answers reach the client as epitaphs, the last section.

Starting a component, the root hands it its first handles in one message, the *processargs*: the process's arguments, a table of names, and handles each tagged with what it is for, its *startup handles*. A component with a program gets the server end of its outgoing directory and, for what it uses, a namespace. The next section is about both. When the process exits, the root logs `init: <name> exited with <code>`, and later routes to it answer `NotFound`: a component is never restarted (`system/service/init/realm.cpp:782`).

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

It makes a channel, keeps one end, and gives the other, the *server end*, to `fsio::connect`. That writes an `Open` for `/svc/ours.test.Echo` on the component's `/svc` channel, with the server end in the message, and returns without waiting (`system/lib/fsio/include/fsio/fsio.hpp:82`).

The root receives that Open. It routes the use, as the last section described. When the route ends at another component, the root does not carry any messages itself: it forwards the same server end to the component that provides the protocol, as an `Open` of `svc/<name>` on that component's *outgoing directory* (`system/service/init/realm.cpp:786`). From then on the client and the server talk on the channel directly, and the root is out of the path.

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

The *outgoing directory* is the directory a component serves for others. The root creates its channel when it resolves the component and keeps the client end; the component receives the server end as its `DirectoryRequest` startup handle, and serves it. Under `svc/` it holds one entry for each protocol the component provides, a *service node*: a node that, when opened, hands the request's channel to the program. `ioservice::Outgoing` (`system/lib/ioservice`) builds that directory and serves it on the Loop. The server registers each protocol with `add_protocol(name, handler)`, where the handler is an object with one function that receives the channel of each client. `echo`'s handler gives each channel a Loop slot.

`examples/echo/echo_server.cpp:265`, in `main`:

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

`directory_request` is taken at the top of `main` with `ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0)` (`examples/echo/echo_server.cpp:252`). A component without a program gets no outgoing directory, since nothing would serve it (`system/service/init/realm.cpp:356`).

Two things are called svc, and they are opposite ends of this picture. A component's `/svc` is the namespace entry that holds what it *uses*. The `svc/` directory inside its outgoing directory holds what it *provides*. Their names for one protocol can differ: the client above asks for `ours.test.Echo`, and the server serves `ours.examples.echo.Echo`. The root translates one into the other when it routes.

An outgoing directory is a very small tree of nodes: the directory itself, `svc/` inside it, and a service node for each protocol. A server that serves a larger tree, files and directories a client opens by path, is a tree server, and [04-tree-servers.md](04-tree-servers.md) is about it.

## Epitaphs

An `Open` has no reply, so when it fails, the failure has nowhere to go. The answer is the *epitaph*: the last message a server writes on a channel before it closes it, carrying a status that says why. Every message begins with a 16-byte header, and two of its fields matter here: the *method number* says which method of the protocol the message is, and the *transaction id* ties a reply to its request (a reply carries its request's id, and a one-way message carries 0). An epitaph is a fixed 24-byte message, the same in every protocol: a header with transaction id 0 and, as method number, the reserved all-ones value that no method can have, and then the status (`system/lib/wire/include/wire/wire.hpp:35`). [03-protocols.md](03-protocols.md) has the whole layout.

Three kinds of code write one:

- the root, when a route fails: `NotFound` or `AccessDenied` or `BadType`, as the last section described (`system/service/init/realm.cpp:279`);
- the directory server, `ioservice`, when an `Open` it receives is refused;
- a server, when it cannot take a connection it was handed. `echo` closes a client it has no room for with `NoResource`.

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

- **It is not how a method reports failure.** A method that fails answers with a reply, a message that carries a status; the connection stays open. An epitaph ends the connection.
- **It is not always there.** A connection that closes with no epitaph reads `PeerClosed` and nothing more. `Echo` is a *closed protocol*: it closes the connection without an epitaph when it receives a method it does not know (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39`; [03-protocols.md](03-protocols.md#closed-protocols) has the rule). The same happens when a server exits with an `Open` still queued for it. So a client that sees `PeerClosed` alone cannot tell why from the channel. [05-testing-and-debugging.md](05-testing-and-debugging.md) lists what a client sees for each failure, and the code that produces each.

## Where to look next

- [03-protocols.md](03-protocols.md): what is in a message, how a method's number comes from its name, and closed protocols (`03-protocols.md#closed-protocols`).
- [04-tree-servers.md](04-tree-servers.md): serving a directory tree, and what the root's `/boot` does with an `Open`.
- [05-testing-and-debugging.md](05-testing-and-debugging.md): what a client sees for each failure, and how to read the logs.
- [06-checklist.md](06-checklist.md): the rules in this chapter, as a checklist.
- Chapter 2, the first server, is in preparation.
- For the words in one list, the [component framework glossary](../architecture/component-framework/glossary.md); for the library layers, [libraries](../architecture/component-framework/libraries.md).

## Sources

- `system/public/ours/types.hpp`, `rights.hpp`, `signals.hpp`, `port.hpp`, `syscall.hpp`, `syscalls.gni`, `process_args.hpp`: handles, rights, signals, ports, limits, and what each system call does.
- `kernel/object/include/ours/object/channel_dispatcher.hpp`: a channel's signals and queue.
- `system/lib/loop/include/loop/loop.hpp`, `system/lib/loop/src/loop.cpp`, `system/lib/loop/README.md`: the Loop and its rules.
- `system/lib/wire/include/wire/wire.hpp`: the epitaph and the message header.
- `system/lib/fsio/include/fsio/fsio.hpp`, `system/lib/fsio/src/fsio.cpp`, `system/lib/ours.io/README.md`, `system/lib/ioservice/README.md`, `system/lib/ioservice/include/ioservice/outgoing.hpp`, `system/lib/ioservice/include/ioservice/server.hpp`, `system/lib/ioservice/src/server.cpp`: the client side, the directory format, the outgoing directory and the epitaph on a refused `Open`.
- `system/lib/ort/include/ort/startup.hpp`: startup handles.
- `system/component_manifest.gni`, `docs/architecture/component-framework/glossary.md`, `docs/architecture/component-framework/libraries.md`, `docs/design/261004/component-framework.md`, `docs/design/261029/protocol-identity.md`: manifests, the words, the root's design.
- `system/service/init/realm.cpp`: how the root starts a component, routes an `Open`, forwards it, and reports an exit.
- `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn`, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp`, `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn`, `system/service/init/test/routing_checks/routing_checks.cpp`: the running example.

Checked against commit `94a62227` on 2026-10-29.
