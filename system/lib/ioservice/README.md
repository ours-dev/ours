# ioservice

The server side of ours.io for trees held in memory (component-framework design, "ioservice"): the root serves /boot and each component's /svc with it, and every provider serves its outgoing directory with it. It is the counterpart of Fuchsia's Rust vfs (`src/storage/lib/vfs/rust`), cut to what ours uses.

ours.io (`//system/lib/ours.io`) only says what the messages look like. This library is what answers them: the connection table, the rights and path checks of Open, Clone, Close, Read, ReadDirents and GetBackingMemory, and the walk down a tree. It is not a mounting layer: there is no mount table, and a client puts a server's tree at a path in its own namespace (`//system/lib/fsio`). A disk filesystem is a separate server and may not use this library at all.

- `ioservice::Tree` (`<ioservice/tree.hpp>`) holds the three node kinds: pseudo-directories, read-only VMO files and services. The program adds the nodes; a client never does. Nodes live in storage the program provides, since user programs have no heap.
- `ioservice::Server` (`<ioservice/server.hpp>`) holds the connection table and handles ours.io: Open with the Rust vfs's checks in its order, ReadDirents, Read, GetBackingMemory, Clone and Close.
- `ioservice::LoopServer` (`<ioservice/loop_server.hpp>`, in the target `//system/lib/ioservice:loop_server`) serves a `Server` on a `loop::Loop`: one slot per connection, whose callback reads a message from the channel, hands it to the `Server` and does what the `Outcome` says. The program says what an Open of a service node comes to by implementing `ioservice::LoopServer::Delegate`, an abstract class its own server class derives from. It is the glue the root's /boot and /svc and a provider's outgoing directory all need; it is a target of its own so that the core below makes no system call.
- `ioservice::Outgoing` (`<ioservice/outgoing.hpp>`, in the target `//system/lib/ioservice:outgoing`) is the provider side of a component, the counterpart of Fuchsia's `component::OutgoingDirectory` (`sdk/lib/component/outgoing/cpp`) cut to unmanaged protocols: its outgoing directory, an `ioservice::Tree` served by a `LoopServer`, with one entry under `svc/` per protocol. `add_protocol(name, handler)` adds `svc/<name>`, where `handler` is an `ioservice::Handler`, the abstract class a protocol's server derives from, with one function, `connect(channel)`; `serve(directory_request)` serves the directory on the DirectoryRequest startup handle and waits on it. When the root forwards a client's Open of `svc/<name>`, the handler gets the client's channel, usually to add it to the same Loop. `//examples/echo` is its first user, and dev's routing_checks reaches echo through it and prints `routing_checks: passed`.

The core touches no channel. `Server::dispatch()` takes one message and returns an `Outcome`: what to write and where, which new channel to wait on, which channel a service was asked for, or that the connection is to be dropped. The program does the writing and the waiting. Its one system call, the duplicate GetBackingMemory hands out, goes through a function the program gives it, so the whole server runs on the host, where its tests are.

## Layout

```text
include/ioservice/   public headers, reached as <ioservice/...>
src/                 sources
test/                host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ioservice`; its headers alone are `//system/lib/ioservice:headers`. The gtest executable is `//system/lib/ioservice/test:ioservice`, built on the host by the default build. `LoopServer` is tested by `//system/lib/ioservice/test:loop_server`, on a real Loop against the fake kernel of `//system/lib/ioservice/test:fake_kernel`, which defines the vDSO's channel and port calls on the host; `Outgoing` is tested by `//system/lib/ioservice/test:outgoing`, on the same fake kernel.
