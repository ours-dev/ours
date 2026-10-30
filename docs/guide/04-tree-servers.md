# Tree servers: serving files, directories and services from a tree

Most servers answer a protocol of their own on each client's channel, and [03-protocols.md](03-protocols.md) is about those. This chapter is about the other kind: a server whose answer is a directory tree. A client lists the names in it, opens a path, reads a file, or asks for the memory behind a file. The root's `/boot`, the files of the boot image, is one. You do not write the file protocol: the library `ioservice` answers it for a tree held in memory. What differs from one tree server to the next is the tree, the storage it lives in, and what an Open of a service node comes to.

A tree server of your own cannot be reached by another component. A manifest cannot route a directory that a component provides, and no program in the repository hands a directory channel to another by any other route. So this chapter is for understanding and for host tests, not for building a service: it explains how the root's `/boot` and `/svc` and every component's outgoing directory work, and how to drive the library in a test that needs no kernel. The worked example near the end is for reading, not for copying.

## What you will be able to do

- Decide whether the server you are about to write is a tree server or a protocol server.
- Read how the root's `/boot` and a component's outgoing directory are built, stored and put on a Loop.
- Say what a tree server answers to any Open, in the order it checks.
- Explain what a connection id is, and why it carries a generation.
- Explain how the rights of a connection narrow, and where.
- Say what an Open of a service node hands your code, and what your code then owns.
- Build a tree and drive a `Server` from a host test, with no kernel.

A citation such as `system/lib/ioservice/src/server.cpp:231` is a file of the repository and a line of the commit named at the end. Every code block is copied from the file it names; the text diagrams are drawn for this chapter.

## Choosing: a tree server or a protocol server

A *protocol server* provides a protocol of its own. A client reaches it by name through its `/svc` and gets a channel of its own to the server, on which it calls the protocol's methods. `examples/echo` is one. A *tree server* serves a directory tree over `ours.io`, the message format of files and directories. A client reaches it through a directory channel, usually bound into its namespace, and uses `fsio` to open, list and read. The terms *namespace*, *outgoing directory* and *epitaph* are defined in [01-concepts.md](01-concepts.md#svc-and-the-outgoing-directory) and [01-concepts.md](01-concepts.md#epitaphs).

| | Protocol server | Tree server |
|---|---|---|
| What a client does | calls the methods of your protocol | lists names, opens a path, reads bytes, maps a file's memory |
| What you write | a codec library for the protocol, and the answers | the tree's contents; `ioservice` already answers `ours.io` |
| What it can hold | whatever the protocol says | directories, read-only files, and service nodes |
| Example | `examples/echo` | the root's `/boot` (`system/service/init/boot_server.hpp`) |

Choose a tree server when what you offer is a set of named things the client wants to browse and open, when it is read-only, and when your program alone decides what is in it. Choose a protocol server when what you offer is an operation, when replies are computed per request, or when anything writes. A file's contents here are bytes that already sit in a *VMO* (a range of memory held through a handle, defined in [Words used elsewhere](01-concepts.md#words-used-elsewhere)) and are mapped into your process, so a tree server fits data that exists before the first client arrives.

The two kinds share a library, not a role. A component that provides a protocol serves it from its outgoing directory, and that directory is a tree that `ioservice::Outgoing` builds and serves for you. The guide still calls such a component a protocol server: the tree is only how clients reach its protocol, while a tree server is one whose tree is what its clients came for. And a tree can hold service nodes of its own, whose Open hands the client's channel to your code to speak a protocol on; the root's `/svc` directories are exactly that.

Why a tree server of your own cannot be reached: the manifest compiler refuses a component that declares or exposes a directory, and says the only directory is the root's built-in `boot` (`system/component_manifest.py:466`, `system/component_manifest.py:649`). The library does not depend on the manifest. `Server::bind`, described under "Connections and their ids", serves a directory on any channel you give it, and a client can bind a directory channel it holds into its namespace with `fsio::installed_namespace().bind(path, directory)` (`system/lib/fsio/include/fsio/fsio.hpp:33`, `system/lib/fsio/include/fsio/namespace.hpp:95`). How a client comes to hold that channel, for instance as a handle in a reply, would be yours to design, and no program in the repository does it.

Where this is stated: `ours.io` is described in `system/lib/ours.io/README.md:3` and the calls of `fsio` in `system/lib/fsio/README.md:7`.

## The pieces

What the chapter is built from, one line each. The sections after say what each does.

- A *node* is one entry of a tree: a directory, a file, or a service.
- A `Tree` holds nodes in an array your program provides. A directory with no parent is a *top directory*, and one `Tree` can hold several.
- A *connection* is one client's open handle on one node. It has its own channel, its own rights (what it may do; see "Answering an Open") and its own place (the offset it has read to, or how far down a listing it is). In this chapter the word means only that: a client's channel to a protocol server is a channel, not a connection.
- A `Server` is a table of connections and the rules that answer `ours.io` for a tree. It reads and writes no channel. You give it one message and it gives you an `Outcome`.
- An `Outcome` is what to do next: one of six actions, `Reply`, `ReplyAndClose`, `Drop`, `Bind`, `Connect` and `Refuse`. The table under "Putting a tree on a Loop" says what each comes to.
- A `LoopServer` is the glue that waits on each connection's channel on a Loop, reads a message, calls the `Server`, and carries out the `Outcome`.
- A `Delegate` is an abstract class your program derives from. The `LoopServer` calls it when an Open reaches a service node.
- `Outgoing` wraps a `LoopServer` over a fixed two-level tree, for a component's outgoing directory. A `Handler` is the abstract class that serves one protocol in it.

```text
client --ours.io--> channel --> Loop slot --> LoopServer --> Server --> Tree
                                                 |   ^          |
                                                 |   +-Outcome--+
                                                 +--> Delegate::connect(service, channel)    your code
```

You can use the `Server` without a `LoopServer` if your program has its own way to wait on channels. This chapter assumes the `LoopServer`, which is what the root and every outgoing directory use. The build targets are `//system/lib/ioservice` for the `Tree` and `Server`, `//system/lib/ioservice:loop_server` and `//system/lib/ioservice:outgoing`. The core is a target of its own so that it makes no system call and its tests run on the host.

Where this is stated: the three node kinds are `system/lib/ioservice/include/ioservice/tree.hpp:35` and the `Tree` is `system/lib/ioservice/include/ioservice/tree.hpp:96`; the `Server` is `system/lib/ioservice/include/ioservice/server.hpp:157` and the six actions are `system/lib/ioservice/include/ioservice/server.hpp:77`; the `LoopServer` is `system/lib/ioservice/include/ioservice/loop_server.hpp:42`; `Handler` and `Outgoing` are `system/lib/ioservice/include/ioservice/outgoing.hpp:50` and `system/lib/ioservice/include/ioservice/outgoing.hpp:66`; the targets are `system/lib/ioservice/BUILD.gn:9`, `system/lib/ioservice/BUILD.gn:49` and `system/lib/ioservice/BUILD.gn:73`.

## Building a tree

A node has one of three kinds (`system/lib/ioservice/include/ioservice/tree.hpp:37`):

```cpp
enum class NodeKind : u8 {
    /// A pseudo-directory: named children, in name order.
    Directory,
    /// A read-only file whose contents are a VMO.
    File,
    /// A protocol endpoint. Opening it hands the request's channel back to
    /// the program serving the tree, with the value given to add_service().
    Service,
};
```

A *pseudo-directory* is a directory that exists only in memory, with no disk behind it. Your program builds the tree. A client never changes it, so there is no removal: a node, once added, stays where it is for the life of the tree. The tree may still grow while it is served.

Building goes through four calls, each of which returns the new `NodeId` or a status:

- `add_root()` makes a *top directory*: a directory with no parent. A tree can hold several, and the `Server` can serve any directory of it on a channel. The root's tree holds `/boot` as one top directory and a `/svc` directory for each component as the others.
- `add_directory(parent, name)` adds an empty directory.
- `add_file(parent, name, file)` adds a file, described by a `VmoFile`: its `vmo`, its `bytes`, and whether it is `executable`.
- `add_service(parent, name, service)` adds a service node and a `u64` that your program chooses, to tell its services apart.

Here is a small tree, the one the host tests of the `Server` serve. Its top directories are held in the fixture's `m_root` and `m_svc`:

```text
top directory 1 (m_root)
  bin/
    x                 an executable file
  config/
    readme            a plain file, "hello, world"
top directory 2 (m_svc), as one component's /svc
  echo                a service
```

and the calls that build its first three nodes (`system/lib/ioservice/test/server_test.cpp:100`):

```cpp
        m_root = m_tree.add_root().unwrap();
        m_bin = m_tree.add_directory(m_root, "bin").unwrap();
        m_x = m_tree
                  .add_file(m_bin, "x",
                            {.vmo = VMO_X, .bytes = bytes_of(X_CONTENTS), .executable = true})
                  .unwrap();
```

`unwrap()` traps if the call failed, which is right for a test and for a call that cannot fail, as the two that start an `Outgoing`; a program whose storage could be too small checks the returned status instead. A `NodeId` is an index into your storage and means something only to the tree that returned it. Adding is refused with `InvArgs` for an invalid name or a parent that is not in this tree, `NotDir` for a parent that is not a directory, `AlreadyExists` for a name already in the directory, and `NoResource` when the storage is full.

What to know before you write the first `add_`:

- **The storage is yours and fixed.** A user program has no heap, so you declare an array of `Node` and give it to the `Tree`, which allocates nothing. A node past the array answers `NoResource`. The next section is about sizing it.
- **A name is kept as a view.** `add_directory`, `add_file` and `add_service` do not copy the name, so the bytes must outlive the tree. The root's `/boot` passes views into the read-only mapping of the boot image. A name from a temporary string leaves the tree pointing at freed memory.
- **A file lends two things.** Its `bytes` are what `Read` copies from; its `vmo` is what `GetBackingMemory` duplicates. Nothing here is owned by the tree: you keep both alive for as long as the tree is served, and nobody may be able to write the VMO. The VMO must hold the handle rights ([chapter 1](01-concepts.md#handles-and-channels)) that the library duplicates: Basic, Map, GetProperty and Read, plus Execute for an executable file, and Duplicate.
- **Names and paths are strict.** A name is 1 to 255 bytes, not `.` or `..`, with no `/` and no NUL byte. A path is at most 1024 bytes and is either exactly `.` or valid names joined by `/`: no leading or trailing `/`, no `//`, no `.` or `..` segment. A path therefore has one spelling, and a walk down a path can never leave the directory it started in.
- **Children are kept in name order, and found by walking the list.** A listing comes out sorted. Finding a name costs a walk over the directory's children, which is fine at the sizes in use and is a known limit.

And this is how the root adds one file of the boot image to `/boot` (`system/service/init/boot_server.cpp:125`):

```cpp
        auto const added = m_tree.add_file(
            parent, rest,
            ioservice::VmoFile{
                .vmo = vmo,
                .bytes = {view->file_data(entry).data(), view->file_data(entry).size()},
                .executable = executable,
            });
```

Where this is stated:

- Nothing is removed and a client never changes the tree: `system/lib/ioservice/include/ioservice/tree.hpp:18`. The tree may grow while served: `system/lib/ioservice/include/ioservice/server.hpp:160`.
- The four calls and their refusals: `system/lib/ioservice/include/ioservice/tree.hpp:105`, `system/lib/ioservice/include/ioservice/tree.hpp:111`. A `NodeId` belongs to its tree: `system/lib/ioservice/include/ioservice/tree.hpp:24`.
- No heap and `NoResource`: `system/lib/ort/src/cpp_dependencies.cpp:16`, `system/lib/ioservice/include/ioservice/tree.hpp:88`.
- Names are views: `system/lib/ioservice/include/ioservice/tree.hpp:115`. What a file lends and the rights of its VMO: `system/lib/ioservice/include/ioservice/tree.hpp:47`.
- The root's tree of top directories: `system/service/init/boot_server.hpp:33`. Where `unwrap()` is used for calls that cannot fail: `system/lib/ioservice/src/outgoing.cpp:28`.
- Name and path rules: `system/lib/ioservice/include/ioservice/tree.hpp:200`, `system/lib/ioservice/src/tree.cpp:48`, `system/lib/ours.io/include/ours_io/ours_io.hpp:54`, `system/lib/ours.io/include/ours_io/ours_io.hpp:59`. A walk only goes down: `system/lib/ioservice/src/tree.cpp:191`.

## Storage you lend, and why it is `constinit`

A tree server holds a lot of fixed-size state: the nodes, the connection table, one `Binding` for each entry of that table (the object the Loop calls when a connection's channel has something), and the buffers a message is read into and an answer is built in. A user program has no heap, so you provide all of it, sized by how many connections and nodes you allow. `Outgoing` and `BootServer` each group theirs in a `Storage` struct. This is `Outgoing`'s (`system/lib/ioservice/include/ioservice/outgoing.hpp:96`):

```cpp
    struct Storage {
        /// The outgoing directory, svc/, and a service node per protocol.
        std::array<ioservice::Node, 2 + MAX_PROTOCOLS> nodes;
        std::array<ioservice::Connection, MAX_CONNECTIONS> connections;
        std::array<ioservice::LoopServer::Binding, MAX_CONNECTIONS> bindings;
        /// Indexed by the value each service node was added with.
        std::array<Handler *, MAX_PROTOCOLS> handlers;
        /// Where a connection's callback reads the one message it handles,
        /// and where the server builds its answer. One of each serves every
        /// callback, since one callback runs at a time.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        ReplyBuffer reply;
    };
```

Three rules decide the sizes.

- **One node for every node of the tree.** The array's size is the tree's capacity. `Outgoing` has room for its directory, `svc/`, and one service node per protocol: `2 + MAX_PROTOCOLS`.
- **Every connection is a Loop slot.** One slot waits on each connection's channel, so the connection table, the `Binding`s (one per entry; with fewer, a connection past them cannot be waited on and is refused) and the Loop's slots grow together. Size the Loop's table for the connections plus everything else your program waits on, and check it at build time against the port's limit; [The Loop and its slots](01-concepts.md#the-loop-and-its-slots) shows how `echo` does it. The root does the same sum for 128 connections among others: three slots for each component it can run (its outgoing directory, its process and its `/svc`), and one for each connection of each server it keeps (`system/service/init/pools.hpp:74`).
- **The buffers hold the largest message.** The request buffers must hold 4096 bytes and 64 handles (`wire::MAX_MESSAGE_BYTES` and `wire::MAX_MESSAGE_HANDLES`), or a read of a large message fails and its connection ends. The reply buffer is as large as a message, with room for the one handle a reply carries.

An `Outgoing` costs about 9 KiB and the root's `BootServer` about 42 KiB, both quoted from their headers. That is too much for the stack, so the storage is a global of the program's. A global in a user program is `constinit`, and one whose type has a destructor is built in a `ustl::LazyInit`; [Where a server's memory comes from](01-concepts.md#where-a-servers-memory-comes-from) explains why, with `echo`'s declarations. The storage here has a destructor because `Connection` holds a `ustl::Option`, so it goes in a `ustl::LazyInit` too, which `main()` builds once with `init()`. `echo` does that for `Outgoing::Storage`, and the root does it for `/boot`.

Where this is stated: the sizes are `system/lib/ioservice/include/ioservice/outgoing.hpp:92` and `system/service/init/boot_server.hpp:54`; `Option`'s destructor is `system/lib/ustl/include/ustl/option.hpp:66`; the message limits are `system/public/ours/syscall.hpp:43` (bytes) and `system/public/ours/syscall.hpp:46` (handles); the declarations are `examples/echo/echo_server.cpp:282`, `system/service/init/init.cpp:107` and `system/service/init/init.cpp:272`.

## Putting a tree on a Loop

### LoopServer

A `LoopServer` takes the tree, the Loop, a `Syscalls`, the storage as a `Memory`, and a delegate. This is how `Outgoing` builds one over its `Storage` (`system/lib/ioservice/src/outgoing.cpp:17`):

```cpp
    , m_server(m_tree, loop, Syscalls{.handle_duplicate = ours::handle_duplicate},
               LoopServer::Memory{
                   .connections = storage.connections,
                   .bindings = storage.bindings,
                   .request_bytes = storage.request_bytes,
                   .request_handles = storage.request_handles,
                   .reply = &storage.reply,
               },
               this)
```

`Syscalls` holds the one system call the core makes: the handle duplicate that GetBackingMemory hands out. Your program passes `ours::handle_duplicate`, and a host test passes a function of its own. The fields of `Memory` are the ones of the `Storage` above. The last argument is the delegate, the object that an Open of a service node reaches (see "Service nodes"); it may be null while the object that will serve as delegate has not been built, and `set_delegate()` sets it later. All of it must outlive the `LoopServer`.

Then `serve(channel, directory, rights)` binds a directory to a channel and waits on it; `rights` are the ours.io rights of that first connection (see "Answering an Open"), and every connection opened through it holds a subset. On success the channel is the Loop's. On failure nothing changed and the channel is still yours. The refusals are `InvArgs`, `NotDir`, and `NoResource` when every connection is used, a connection has no `Binding`, or the Loop is full.

Serving a channel, and every Open that makes a new connection, end in `watch()`, which takes the `Binding` at the connection's index, fills it in and registers it with the Loop (`system/lib/ioservice/src/loop_server.cpp:61`):

```cpp
    auto &binding = m_memory.bindings[connection.index];
    binding.m_server = this;
    binding.m_connection = connection;
    auto const added = m_loop->add(channel, CONNECTION_SIGNALS, binding);
    if (added.is_err()) {
        // The Loop did not take the channel, so the connection must not
        // outlive this answer either.
        m_server.release(connection);
        return added.unwrap_err();
    }
    return Status::Ok;
```

A `Binding` is a `loop::Waiter`, the object that a slot waits on ([The Loop and its slots](01-concepts.md#the-loop-and-its-slots) explains it). The Loop keeps its address, which stays valid because the array of `Binding`s is the program's storage. The slot waits for `Readable` or `PeerClosed`, and when it fires the Loop calls the `Binding`'s `on_signals(observed)`, which is the one line `return m_server->handle(m_connection, observed);`. The answer it gives the Loop is `Rearm` (wait again) or `Close` (close the handle and free the slot). Writing a `Waiter` of your own for a protocol server's clients is the subject of the first-server chapter, [02-first-server.md](02-first-server.md).

What `handle()` does:

1. It finds the connection from its id. A connection that has gone answers `Close`.
2. If the client closed and nothing is left to read, it releases the connection and answers `Close`. The Loop then closes the channel.
3. Otherwise it reads one message, hands it to `Server::dispatch()`, and acts on the `Outcome`. Unless the table below says otherwise it then answers `Rearm`, so that a client with more messages waits behind everyone else.

| `Outcome` | What `LoopServer` does | Answer to the Loop |
|---|---|---|
| `Reply` | writes the message on the connection's channel; if the write fails, tells the delegate with `write_failed()` and releases the connection | `Rearm`; `Close` on a failed write |
| `ReplyAndClose` | a Close was answered: writes the message, ignoring failure | `Close` |
| `Drop` | the message was malformed or not served: closes every handle the message carried | `Close` |
| `Bind` | an Open or Clone made a connection: waits on the request's channel as a new connection; if it cannot, writes an epitaph with the reason on it and closes it | `Rearm` |
| `Connect` | an Open reached a service node: calls `Delegate::connect()`, or closes the channel if there is no delegate | `Rearm` |
| `Refuse` | an Open or Clone failed: writes the epitaph on the request's channel, then closes it | `Rearm` |

A reply that cannot be written ends its connection, because the client would otherwise wait for good.

Where this is stated: `Syscalls` is `system/lib/ioservice/include/ioservice/server.hpp:52`; the signals a connection waits for are `system/lib/ioservice/src/loop_server.cpp:15` and the `Binding`'s callback is `system/lib/ioservice/src/loop_server.cpp:75`; the steps above are `system/lib/ioservice/src/loop_server.cpp:78-112` and the table is `system/lib/ioservice/src/loop_server.cpp:113-170`.

### Outgoing

If what you serve is protocols, `Outgoing` is the whole of this for you. It builds the tree (the outgoing directory, its `svc/` directory and a service node per protocol), serves it, and calls your `Handler` when a client's Open of `svc/<name>` arrives. A `Handler` is the abstract class that your protocol's server derives from, with one function that receives the client's channel (`system/lib/ioservice/include/ioservice/outgoing.hpp:50`):

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

You own the channel from the call: serve it, usually by giving it a Loop slot, or close it, and do not wait. Nothing is deleted through a `Handler`, so its destructor is protected and not virtual: a virtual one would add a deleting destructor, which a program with no heap has no use for. `EchoServer` derives from it and its `connect` gives each client's channel a slot. How a program wires `add_protocol(name, handler)` and `serve(directory_request)` into its `main` is shown in [`/svc` and the outgoing directory](01-concepts.md#svc-and-the-outgoing-directory).

`add_protocol(name, handler)` is refused with `InvArgs` for an invalid name, `AlreadyExists` for a name already added, and `NoResource` once 8 protocols are served. At most 4 connections to the outgoing directory may be open at once. The name and the handler are kept by address, so they must outlive the directory. `serve()` takes the `DirectoryRequest` handle that you took at the start of `main`.

Where this is stated: `EchoServer` is `examples/echo/echo_server.cpp:88`; the limits and refusals are in `system/lib/ioservice/include/ioservice/outgoing.hpp:69`, `system/lib/ioservice/include/ioservice/outgoing.hpp:76`, `system/lib/ioservice/include/ioservice/outgoing.hpp:120` and `system/lib/ioservice/include/ioservice/outgoing.hpp:132`.

## Answering an Open

Open is the method where a tree server decides who may reach what, so it has the most rules. The *request* has three parts: the `path` to open, relative to the directory the Open is written to; the `flags`, which say which rights the caller wants and which kind of node it expects; and `object`, the server end of a new channel the caller made. Open is one-way, so there is no reply to carry a failure, and a failed Open is an epitaph on `object`. The request arrives on a connection, which is a connection to one node, from which the walk starts.

A *right* is one thing a connection may do. These are ours.io rights, a different set from the handle rights of [chapter 1](01-concepts.md#handles-and-channels), which say what a holder may do through a handle to a kernel object. There are nine ours.io rights, and five matter in what follows:

| Right | What it lets a connection do |
|---|---|
| Traverse | open a name below a directory |
| Enumerate | list the directory |
| Connect | open a service node |
| ReadBytes | read a file |
| Execute | ask for a file's memory with Execute |

The other four give a client nothing in a read-only tree. WriteBytes and ModifyDirectory: an Open of a file that asks for WriteBytes is refused, and an Open that asks to create is refused even on a connection that holds ModifyDirectory. GetAttributes and UpdateAttributes can be held, and no method served here uses them. The `PERM_` bits of the request's flags have the same values as the rights, so a request's rights are read straight out of its flags.

A manifest writes the rights of a directory as a short string: `r*` is read only (Connect, Enumerate, Traverse, ReadBytes and GetAttributes), and `rx*` adds Execute; `rw*`, `w*` and `x*` are the others. This is the `use` of the root's `/boot` with `rx*` in the manifest of `routing_checks`, the dev image's test component (`system/service/init/test/routing_checks/BUILD.gn:54`):

```gn
      {
        directory = "boot"
        path = "/boot"
        rights = [ "rx*" ]
      },
```

The `Server` receives the request and checks it. The refusals are `ours::Status` values, and [05-testing-and-debugging.md](05-testing-and-debugging.md) says what a client sees for each. **The order of the checks is part of the protocol**: it decides which error a request that is wrong in several ways gets, and so what a client can learn. A connection without Traverse answers AccessDenied to every name, whether or not it exists, before the tree is looked at (`system/lib/ioservice/src/server.cpp:231`).

| Step | Check | Refused with |
|---|---|---|
| 1 | the path is a valid path | `BadPath` |
| 2 | a path other than `.` needs the connection to hold Traverse | `AccessDenied` |
| 3 | every right requested is held by the connection | `AccessDenied` |
| 4 | flags that name a service need the connection to hold Connect | `AccessDenied` |
| 5 | attributes: nothing to check, since Open's options table is not served | |
| 6 | flags that ask to create something | `AccessDenied` without ModifyDirectory, `NotSupported` with it |
| 7 | flags for a node-only connection or an OnRepresentation event | `NotSupported` |
| 8 | the walk from the connection's node along the path | `NotFound`, or `NotDir` if a segment on the way is not a directory |
| 9 | the node's own rules, in the table below | see below |

Take the test tree, and a connection to its first top directory that holds `rx*`. An Open of `config/readme` with `PERM_READABLE` passes steps 1 to 4 (the path is valid, the connection holds Traverse and every right asked for, and no flag names a service), has no creation flag, finds the file, and the file takes the flags. The connection that results holds ReadBytes and GetAttributes: the other rights of `PERM_READABLE` mean nothing for a file and are dropped. Step 3 is why a connection can never grow its rights (`system/lib/ioservice/src/server.cpp:245`):

```cpp
    // 3. A connection never grows: what it opens holds a subset of its
    //    rights. This is what makes a namespace entry's rights a ceiling
    //    for everything opened through it.
    if ((requested & ~connection.m_rights) != Operations::None) {
        return ustl::err(Status::AccessDenied);
    }
```

Step 9 depends on the kind of node found:

| Node found | The request is refused with | A request that passes gets |
|---|---|---|
| Directory | `NotFile` if the flags name only another kind and would accept a file, `BadType` for any other mismatch; `InvArgs` for append or truncate flags | a new connection that keeps every right the request asked for, because a directory's rights are the ceiling for what is opened below it |
| File | `NotDir` if the flags name a directory and neither a file nor a symlink, `BadType` for any other mismatch; `InvArgs` for truncate without WriteBytes; `AccessDenied` for WriteBytes, since a file is never writable, and for Execute on a file added without `executable` | a new connection that keeps only the rights that mean something for a file; the rest are dropped without an error |
| Service | `NotDir`, `NotFile` or `BadType` if the flags name another kind; `InvArgs` for any other flag; `AccessDenied` without Connect, however the request is spelled | no connection: the `Connect` outcome, described under "Service nodes" |

If the checks pass for a directory or a file, the `Server` makes a connection for the request's channel and answers `Bind`. If they fail, it answers `Refuse`: an epitaph, written on the request's `object` channel, which is then closed. The connection the request came on stays. The caller learns of a failure only from the epitaph. Success is silent: the caller goes on using the channel. The tests pin each line of the tables; for example (`system/lib/ioservice/test/server_test.cpp:282`, where `root` is a connection to the first top directory):

```cpp
TEST_F(IoserviceServer, OpenChecksRightsBeforeTheTree) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "nope", Flags::PermWriteBytes), Status::AccessDenied);
    EXPECT_EQ(refused(root, "bin", Flags::PermModifyDirectory), Status::AccessDenied);
    EXPECT_EQ(refused(root, "nope", ours_io::PERM_READABLE), Status::NotFound);
}
```

### The other methods

A directory connection serves Open, ReadDirents, Clone and Close. A file connection serves Read, GetBackingMemory, Clone and Close.

- **ReadDirents** needs Enumerate (`AccessDenied`). A listing starts with `.` and then the children in name order, as many whole entries as fit in the caller's `max_bytes`: `.`, `bin`, `config` for the test tree's first top directory. If the first entry of a reply does not fit, the answer is `BufferTooSmall`. A `max_bytes` above the largest reply is `OutOfRange`. The end of the listing is an Ok reply with no entries. The cursor lives in the connection.
- **Read** needs ReadBytes (`BadHandle` without it). A `count` above the largest transfer, 4072 bytes, is `OutOfRange`. It copies from the file's `bytes` at the connection's offset and moves the offset. At the end it answers Ok with no bytes.
- **GetBackingMemory** hands out a duplicate of the file's VMO. Asking for Read, Write or Execute needs the same right on the connection (`AccessDenied`). Asking for a private clone and a shared buffer together is `InvArgs`. The duplicate has Basic, Map and GetProperty, plus Read and Execute as asked. A private clone gets the same duplicate, which is a private copy only because nobody can write the VMO.
- **Clone** makes another connection to the same node with the same rights, starting from the beginning again. If the table is full the answer is a `NoResource` epitaph on the new channel.
- **Close** is answered Ok, then the connection ends and its channel is closed.

Anything else is not served on that connection: a File method on a directory, a method nobody defines, a message that does not decode, a reply sent as a request. The `Server` answers `Drop`: the connection ends, no epitaph is written, and the handles the message carried are closed. `ours.io` is a closed protocol, in the sense of [03-protocols.md](03-protocols.md#closed-protocols), and the header has no bit that would let an old server say "unknown method".

Where this is stated:

- The nine steps are `system/lib/ioservice/src/server.cpp:237-277` and step 9 is `system/lib/ioservice/src/server.cpp:279-349`. The request's parts and the failed Open's epitaph: `system/lib/ours.io/include/ours_io/ours_io.hpp:26`, `system/lib/ours.io/include/ours_io/ours_io.hpp:42`. The nine rights: `system/lib/ours.io/include/ours_io/ours_io.hpp:127`; the rights of a request come from its flags at `system/lib/ioservice/src/server.cpp:47`; the manifest strings are `system/lib/ours.io/include/ours_io/ours_io.hpp:141`.
- The rights a file keeps are `system/lib/ioservice/src/server.cpp:30`; a file connection after `PERM_READABLE` is pinned by `system/lib/ioservice/test/server_test.cpp:357-370`.
- Which methods each connection serves: `system/lib/ioservice/include/ioservice/server.hpp:182`. ReadDirents: `system/lib/ioservice/src/server.cpp:352` and `system/lib/ioservice/test/server_test.cpp:394`. Read and its limit: `system/lib/ioservice/src/server.cpp:400`, `system/lib/ours.io/include/ours_io/ours_io.hpp:64`. GetBackingMemory: `system/lib/ioservice/src/server.cpp:421`. Clone: `system/lib/ioservice/src/server.cpp:510`. Close: `system/lib/ioservice/src/server.cpp:497`. Drop and the closed protocol: `system/lib/ioservice/src/server.cpp:561`.

## Service nodes: when an Open reaches your code

A service node is where the file protocol ends. Three things are involved: the service node, added with a name and a `u64` value of your choosing; the request's channel, the server end of a channel that the client made and kept the other end of; and the delegate, the object you gave the `LoopServer`.

When an Open of a service node passes steps 1 to 4 and the service rules above, the `Server` answers `Connect` instead of `Bind`. The `LoopServer` calls `Delegate::connect(service, channel)` with the node's value and the request's channel (`system/lib/ioservice/include/ioservice/loop_server.hpp:54`):

```cpp
        /// Where an Open of a service node goes: |service| is the value the
        /// node was added with and |channel| the request's, which the
        /// function owns from the call on. It serves the channel, forwards
        /// it, or closes it.
        virtual auto connect(u64 service, ours::Uandle channel) -> void = 0;
```

A `Delegate` has one more function, `write_failed(status)`, called just before a connection ends because its reply could not be written; it does nothing unless overridden. Like `Handler`, it is an abstract class with a protected, non-virtual destructor.

From the call to `connect` on, the channel is yours, and it no longer speaks `ours.io`: it carries the protocol's own messages. What you may do with it:

- *Serve it*, by adding it to your Loop, as `echo` does for each client (`examples/echo/echo_server.cpp:139`).
- *Forward it*, by writing it into another channel as the server end of a new Open. The root does this for a protocol a sibling provides (`system/service/init/realm.cpp:789`).
- *Close it*, with an epitaph first if you want the client to learn why (`examples/echo/echo_server.cpp:77`).

Never keep it and do nothing: the client would wait for an answer that never comes. You give the delegate to the `LoopServer`'s constructor, or later with `set_delegate()` when the delegate can only exist after the server does. The function runs inside a Loop callback, so it may add slots to the Loop but must not wait. If no delegate has been set, the `LoopServer` closes the channel itself.

The value is how the delegate tells which service was opened. `Outgoing` adds each protocol with the index of its handler and looks the handler up by that index (`system/lib/ioservice/src/outgoing.cpp:38`). The root's `Realm` is its delegate: the object that keeps the root's table of components (each entry an *instance*, the root's record of one component), starts them, and routes their `/svc`. It packs two numbers into the value, which instance and which of that instance's uses (`system/service/init/realm.cpp:97`):

```cpp
    constexpr auto service_key(usize const instance, u32 const use) -> u64 {
        return (static_cast<u64>(instance) << 32) | use;
    }
```

A client's Open of `/svc/ours.test.Echo` passes through two tree servers on its way to `echo`. The first is the root's, whose delegate is `Realm`; the second is `echo`'s outgoing directory, whose delegate is `Outgoing`. `ProtocolService` is the flag that asks for the protocol a node stands for, not the node itself:

```text
client:  Open("ours.test.Echo", ProtocolService, S)         on its /svc connection
root:    the /svc tree finds the service node, answers Connect
         Realm::connect(key, S) routes the use, then writes
         Open("svc/ours.examples.echo.Echo", ProtocolService, S) on echo's outgoing directory
echo:    Outgoing's tree walks svc -> the node, answers Connect
         Outgoing::connect(index, S) -> Handler::connect(S): echo adds S to its Loop
```

Each hop writes and does not wait. From then on the client and `echo` speak directly on `S`.

Where this is stated: the request's channel is described in `system/lib/fsio/include/fsio/fsio.hpp:52` and `ProtocolService` in `system/lib/fsio/include/fsio/fsio.hpp:82`; the hops do not wait at `system/service/init/realm.cpp:820`.

## Connections and their ids

The `Server` keeps its connections in a table you provide, and it never hands out a pointer into it. It hands out a `ConnectionId` (`system/lib/ioservice/include/ioservice/server.hpp:42`):

```cpp
struct ConnectionId {
    u32 index;
    u32 generation;

    friend constexpr auto operator==(ConnectionId, ConnectionId) -> bool = default;
};
```

The `index` says which entry of the table, and the `generation` says which life of that entry. Three facts:

- An entry of the connection table is free until a connection is made in it; it is reused after the connection ends.
- A `ConnectionId` is the name of one connection, kept by whoever waits on its channel. In the `LoopServer` the `Binding` its Loop slot waits on keeps it.
- The generation of an entry goes up by one each time a connection in it ends.

What happens, with entry 3:

1. A client opens a directory. The `Server` makes a connection in entry 3, which has generation 1, and the program keeps `{3, 1}`.
2. The client closes. The `Server` releases entry 3, which moves to generation 2.
3. Another client opens a directory. The `Server` makes its connection in entry 3, now `{3, 2}`.
4. A late message arrives for `{3, 1}`: a copy of the id that the program kept, or an event queued before the release. `find()` sees that entry 3 is at generation 2, finds no connection, and `dispatch()` answers `Drop`.

Without the generation, step 4 would be served as the second client's connection: its rights, its offset, its files. The Loop guards its own slots the same way, with a key made of a generation and an index.

The `Server` never writes or closes a channel. It only records the channel of each connection so that you can find it from the id. Whoever drives it must close the channel when a connection ends. With a `LoopServer` the Loop does: the connection's `Binding` answers `Close`.

The first connection to a tree has no Open to make it. `Server::bind(channel, directory, rights)` serves a directory on a channel "as if a client had opened it": no message is sent. The root uses it to give a component a namespace entry, and `Outgoing::serve()` uses it for the outgoing directory.

Side note, generation 0 and the wrap-around. Generation 0 never names a live connection: every entry starts at generation 1, and a release that would wrap to 0 skips it. So `ConnectionId{}`, or a table of ids zeroed at start, is stale by construction and cannot name whichever connection sits in entry 0. The release looks like this (`system/lib/ioservice/src/server.cpp:159`):

```cpp
    // A new generation, so that |id| and every copy of it go stale. It wraps
    // after 2^32 uses of one slot, which is far past any connection's life,
    // and skips 0 when it does, so that no live connection is ever {*, 0}
    // (see ConnectionId).
    auto next = id.generation + 1;
    if (next == 0) {
        next = FIRST_GENERATION;
    }
    *slot = Connection{};
    slot->m_generation = next;
```

Where this is stated: a release bumps the generation at `system/lib/ioservice/src/server.cpp:154`; generation 0 is explained at `system/lib/ioservice/include/ioservice/server.hpp:37`; the Loop's key is `system/lib/loop/include/loop/loop.hpp:85`; the `Server` records the channel but never closes it at `system/lib/ioservice/include/ioservice/server.hpp:136`; `bind` is `system/lib/ioservice/include/ioservice/server.hpp:166`.

## Rights only shrink

Four places decide what a connection may do, and each can only keep or shrink what the one before it gave.

**The manifest.** A use of the root's `boot` states rights, and so does each offer on the way. Walking from the use toward the source, a hop that states rights must state at least the rights of the hop before it, or the route is `AccessDenied`. The dev image uses this on purpose, in three routes of `routing_checks`:

| Entry in `routing_checks` | The use states | The root's offer states | The route |
|---|---|---|---|
| `/boot` | `rx*` | `rx*` | succeeds |
| `/boot-r` | `r*` | `r*`, through an offer that renames `boot` with `as` | succeeds |
| `/denied` | `rx*` | `r*` | `AccessDenied`: the root closes that entry with an `AccessDenied` epitaph, and every open under it answers `AccessDenied` |

**The binding.** When the root binds a directory to a component, it passes the route's rights to the tree server. This is the ceiling for everything opened through that connection (`system/service/init/realm.cpp:524`):

```cpp
            served = m_boot->serve(server, routed->subdir.view(), to_operations(*routed->rights));
```

**Open.** Step 3 above: what a connection opens holds a subset of its rights. A file shrinks further to the rights that mean something for a file, and is never writable.

**GetBackingMemory.** The VMO it hands out can do no more than the connection can: its handle rights (chapter 1's sense) follow the connection's ours.io rights, so a VMO with Read needs ReadBytes on the connection and one with Execute needs Execute. A file's Execute is double-gated: the file must have been added `executable` for a connection to hold Execute, and the connection must hold Execute for the VMO to carry it.

Clone keeps the same rights, so cloning is not a way around any of this. A component's `/svc` is served with `r*`, which holds Connect, the right needed to open a service node, and Traverse.

Where this is stated:

- The rule for hops is `system/lib/component/src/route.cpp:95`. The root serves a component's `/svc` with `r*` at `system/service/init/realm.cpp:93`. The dev image's offers are `kernel/products/image/dev/BUILD.gn:114`, `kernel/products/image/dev/BUILD.gn:125` and `kernel/products/image/dev/BUILD.gn:134`; the uses are `system/service/init/test/routing_checks/BUILD.gn:54`, `system/service/init/test/routing_checks/BUILD.gn:59` and `system/service/init/test/routing_checks/BUILD.gn:70`.
- The denied entry is closed with the epitaph at `system/service/init/realm.cpp:535` and answers `AccessDenied` at `system/service/init/test/routing_checks/routing_checks.cpp:317`.
- GetBackingMemory follows the connection at `system/lib/ioservice/src/server.cpp:429`; the Execute gate is `system/lib/ioservice/src/server.cpp:309`; Clone keeps the rights at `system/lib/ioservice/src/server.cpp:516`.

## Testing without a kernel

The `Server` touches no channel and makes one system call through a function you give it, so a host test builds a tree and drives it one encoded message at a time. The fixture of `server_test.cpp` is five members (`system/lib/ioservice/test/server_test.cpp:239`):

```cpp
    std::array<ioservice::Node, 16> m_nodes{};
    ioservice::Tree m_tree{m_nodes};
    std::array<ioservice::Connection, 4> m_connections{};
    ioservice::Server m_server{m_tree, m_connections, {.handle_duplicate = fake_duplicate}};
    ioservice::ReplyBuffer m_out{};
```

A test encodes a request with the `ours.io` codec, calls `dispatch()` and reads the `Outcome` as a program would act on it (`system/lib/ioservice/test/server_test.cpp:135`):

```cpp
    auto open(ConnectionId const connection, std::string_view const path, Flags const flags,
              Uandle const object = OBJECT) -> Outcome {
        return dispatch(connection,
                        ours_io::encode_open(m_bytes, m_handles,
                                             {.flags = flags, .path = path, .object = object}));
    }
```

Handles are plain numbers there, and `fake_duplicate` records what it was asked. The `LoopServer` and `Outgoing` need a Loop, so their tests run on a model of ports and channels, `fake::Kernel`. It defines the vDSO's channel and port functions on the host under their real names (the vDSO is the library of system-call entry points that the kernel maps into each process; [Words used elsewhere](01-concepts.md#words-used-elsewhere)), so the library's source is compiled unchanged. The three test programs are `//system/lib/ioservice/test:ioservice`, `:loop_server` and `:outgoing`. How to run them, and how to boot the image, is in [05-testing-and-debugging.md](05-testing-and-debugging.md).

Where this is stated: the fake kernel is described in `system/lib/ioservice/test/fake_kernel.hpp:15` and the test targets are in `system/lib/ioservice/test/BUILD.gn:1`.

## Worked example: the root's `/boot`

This is for reading, not for copying: no other component can reach a tree server of its own, so the root's `/boot` is the one place where all the pieces above work together. `BootServer` (`system/service/init/boot_server.hpp`) is made of:

- The **tree**: one top directory for `/boot`, with a directory per directory of the boot image and a file per file, and further top directories, one `/svc` directory for each component the root starts.
- The **`LoopServer`** that serves all of them from one connection table, 128 connections.
- The **`Storage`**: 256 nodes for `/boot` and 128 for `/svc` in one array, and the buffers.
- The **delegate**: the root's `Realm`, which derives from `LoopServer::Delegate`.

What happens:

1. `main` has the storage in a `constinit ustl::LazyInit` and builds the `BootServer` over it.
2. `ingest()` reads the boot image, mapped read-only, and makes one node for each directory and file in it, with `add_file` as shown above. A file under `bin/` gets an executable VMO, so that a client can ask for its memory with Execute. A name the tree refuses is logged and left out; running out of nodes fails the whole ingest with `NoResource`.
3. The root starts a component. For each directory the component uses, it routes the use, makes a channel pair, and calls `BootServer::serve(server_end, subdir, rights)`, which finds the subdirectory and calls `LoopServer::serve`. No message is sent. The client end goes to the component as a startup handle that `fsio` binds into the namespace.
4. If the component uses protocols, the root calls `add_svc_directory()`, which is `Tree::add_root()`, adds one service node per protocol use with `service_key(...)` as its value, and serves that directory with `r*`.
5. A client opens a file under `/boot`. The checks of the table run on the client's connection, which holds the rights the route gave it. Read copies from the mapping into the reply; GetBackingMemory duplicates the file's VMO.
6. A client opens a service in its `/svc`. The `Connect` outcome reaches `Realm::connect()`, which routes it, as the trace under "Service nodes" showed.

What to take from it for your own tree server:

- **Decide the sizes first, as constants.** `pools.hpp` states every limit and `static_assert`s the sums, so a limit that cannot hold is a build error, not a failure at run time.
- **Share one array fairly.** `/boot`'s nodes and `/svc`'s nodes are counted separately inside one array, so `/boot` cannot take `/svc`'s share.
- **Read your own tree by direct call.** The root finds a file with `BootServer::file(path)`, which is `Tree::lookup` and `Tree::file`, never by opening it over a channel. The root has one thread, and a message it sent itself would wait for a reply that only it can write.
- **Set the delegate when it exists.** `BootServer` is built before `Realm`, and `Realm` needs the `BootServer`, so the delegate is set after, by `Realm`'s constructor. Until then an Open of a service node closes its channel.
- **Names are views.** The tree's names point into the mapping of the boot image, which stays mapped for good.

Where this is stated:

- The pools: `system/service/init/pools.hpp:27` (connections), `system/service/init/pools.hpp:51` and `system/service/init/pools.hpp:62` (nodes); the `Storage` is `system/service/init/boot_server.hpp:58`; the delegate is `system/service/init/realm.hpp:215`.
- The steps: `system/service/init/init.cpp:272` (`main`), `system/service/init/boot_server.cpp:45` (`ingest()`), `system/service/init/realm.cpp:524` and `system/service/init/realm.cpp:538` (a namespace entry), `system/service/init/realm.cpp:703`, `system/service/init/realm.cpp:724` and `system/service/init/realm.cpp:730` (`/svc`).
- The advice: shares are `system/service/init/boot_server.hpp:143`; direct calls are `system/service/init/boot_server.hpp:49` and `system/service/init/boot_server.cpp:149`; the delegate is set at `system/service/init/realm.cpp:317`; the mapping stays mapped, `system/service/init/boot_server.hpp:78`.

## What a tree server does not do

- **It does not write.** There is no create, remove or rename: a creating Open is refused and a file is never writable. A disk filesystem is a separate server and need not use this library.
- **It has no attributes, no symlinks and no node-only connections.** An Open that asks for a node-only connection or the OnRepresentation event is `NotSupported`, since a client served without it would wait for an event that never comes.
- **It is single-threaded.** One thread calls the `Server`, one message at a time.
- **Lookups and new connections scan.** Finding a child walks its directory's list, and finding a free connection walks the table. Both are known limits.
- **No manifest routes a directory a component serves**, as the start of the chapter said.

Where this is stated: `system/lib/ioservice/README.md:5`, `system/lib/ioservice/src/server.cpp:40` and `system/lib/ioservice/include/ioservice/server.hpp:155`.

## Where to look next

- [02-first-server.md](02-first-server.md): the first server, and the `Waiter` a protocol server writes for its clients.
- [05-testing-and-debugging.md](05-testing-and-debugging.md): running the host tests, booting the image, and what a client sees for each failure above.
- [06-checklist.md](06-checklist.md): the rules for the callback, who closes which handle, and the limits, as a checklist.
- [03-protocols.md](03-protocols.md): the protocol a service node's channel speaks.
- [01-concepts.md](01-concepts.md#the-loop-and-its-slots): the Loop and its slots, which the sizing rules above rest on.
- The headers are the reference: `system/lib/ioservice/include/ioservice/tree.hpp`, `system/lib/ioservice/include/ioservice/server.hpp`, `system/lib/ioservice/include/ioservice/loop_server.hpp` and `system/lib/ioservice/include/ioservice/outgoing.hpp`.

## Sources

- `system/lib/ioservice/README.md`, `system/lib/ioservice/BUILD.gn`: what the library is and is not, and its targets.
- `system/lib/ioservice/include/ioservice/tree.hpp`, `system/lib/ioservice/include/ioservice/server.hpp`, `system/lib/ioservice/include/ioservice/loop_server.hpp`, `system/lib/ioservice/include/ioservice/outgoing.hpp`: the tree, the `Server`, the `LoopServer` and `Outgoing`.
- `system/lib/ioservice/src/tree.cpp`, `system/lib/ioservice/src/server.cpp`, `system/lib/ioservice/src/loop_server.cpp`, `system/lib/ioservice/src/outgoing.cpp`: the walk, the Open checks in order, the generation, and what the `LoopServer` does with each `Outcome`.
- `system/lib/ioservice/test/server_test.cpp`, `system/lib/ioservice/test/tree_test.cpp`, `system/lib/ioservice/test/loop_server_test.cpp`, `system/lib/ioservice/test/outgoing_test.cpp`, `system/lib/ioservice/test/fake_kernel.hpp`, `system/lib/ioservice/test/BUILD.gn`: what each answer is pinned to, and how to drive a server on the host.
- `system/service/init/boot_server.hpp`, `system/service/init/boot_server.cpp`, `system/service/init/pools.hpp`, `system/service/init/init.cpp`, `system/service/init/realm.hpp`, `system/service/init/realm.cpp`: the root's `/boot` and `/svc`.
- `system/service/init/test/routing_checks/BUILD.gn`, `system/service/init/test/routing_checks/routing_checks.cpp`, `kernel/products/image/dev/BUILD.gn`: the narrowed `/boot` entries of the dev image and the checks that run against them.
- `examples/echo/echo_server.cpp`: an outgoing directory, its storage and a `Handler`.
- `system/lib/ours.io/README.md`, `system/lib/ours.io/include/ours_io/ours_io.hpp`: the file protocol, its flags, rights and limits.
- `system/lib/loop/include/loop/loop.hpp`, `system/lib/fsio/include/fsio/fsio.hpp`, `system/lib/fsio/include/fsio/namespace.hpp`, `system/lib/fsio/README.md`, `system/lib/wire/include/wire/wire.hpp`: the Loop, the client calls, the message limits.
- `system/lib/ort/src/cpp_dependencies.cpp`, `system/lib/ustl/include/ustl/option.hpp`: no heap, and the type that has a destructor.
- `system/lib/component/src/route.cpp`, `system/component_manifest.py`: the rights a route may state, and what a manifest may declare.
- `docs/architecture/component-framework/glossary.md`: the words *component*, *manifest*, *offer*, *use* and *route*.

Checked against commit `3cb0d8a5` on 2026-10-29.
