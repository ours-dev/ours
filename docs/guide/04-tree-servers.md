# Tree servers: serving files, directories and services from a tree

Most servers answer a protocol of their own on each client's channel, and [03-protocols.md](03-protocols.md) is about those. This chapter is about the other kind: a server whose answer is a directory tree. A client lists the names in it, opens a path, reads a file, or asks for the memory behind a file. The root's `/boot`, the files of the boot image, is one. You will not write the file protocol: the library `ioservice` answers it for a tree held in memory. You write what is different from one server to the next: the tree, the storage it lives in, and what an Open of a service node comes to.

## What you will be able to do

- Decide whether the server you are about to write is a tree server, a protocol server, or both.
- Build a tree of directories, files and service nodes in storage your program provides.
- Say what the server answers to any Open, in the order it checks.
- Explain what a connection id is, and why it carries a generation.
- Explain how rights narrow, and where.
- Hand an Open of a service node to your own code, and say what your code then owns.
- Lend the storage a tree server needs, and say why it is `constinit`.
- Put a tree on a Loop with `LoopServer`, or provide protocols through `Outgoing`.
- Drive a `Server` from a host test, with no kernel.

Citations such as `server.cpp:231` name a file and a line of the commit this chapter was checked against, which is named at the end. A bare file name is a file of `system/lib/ioservice` (headers in `include/ioservice/`, sources in `src/`, tests in `test/`), except that `ours_io.hpp` is `system/lib/ours.io/include/ours_io/ours_io.hpp` and, in the worked example, `boot_server.*`, `pools.hpp`, `realm.*` and `init.cpp` are files of `system/service/init`. Every other file is named in full.

## Choosing: a tree server or a protocol server

A *protocol server* provides a protocol of its own. A client reaches it by name through its `/svc` and gets a channel of its own to the server, on which it calls the protocol's methods. `examples/echo` is one. A *tree server* serves a directory tree over `ours.io`, the message format of files and directories (`system/lib/ours.io/README.md:3`). A client reaches it through a directory channel, usually bound into its namespace, and uses `fsio` to open, list and read (`system/lib/fsio/README.md:7`). The terms *namespace*, *outgoing directory* and *epitaph* are defined in [01-concepts.md](01-concepts.md#svc-and-the-outgoing-directory) and [01-concepts.md](01-concepts.md#epitaphs).

| | Protocol server | Tree server |
|---|---|---|
| What a client does | calls the methods of your protocol | lists names, opens a path, reads bytes, maps a file's memory |
| What you write | a codec library for the protocol, and the answers | the tree's contents; `ioservice` already answers `ours.io` |
| What it can hold | whatever the protocol says | directories, files that are read-only VMOs, and service nodes |
| Example | `examples/echo` | the root's `/boot` (`system/service/init/boot_server.hpp`) |

Choose a tree server when what you offer is a set of named things the client wants to browse and open, when it is read-only, and when your program alone decides what is in it. Choose a protocol server when what you offer is an operation, when replies are computed per request, or when anything writes. A file's contents here are bytes that already sit in a VMO, mapped into your process, so a tree server fits data that exists before the first client arrives. A *VMO* is a range of memory, held through a handle, that exists apart from any address space: a program maps it to read it, or hands the handle to another program (`kernel/object/include/ours/object/vmo_dispatcher.hpp:28`).

The two are not exclusive. A component that provides a protocol serves it from its outgoing directory, and that directory is a tree: a `svc/` directory and a service node per protocol. `ioservice::Outgoing` builds and serves it for you (`system/lib/ioservice/include/ioservice/outgoing.hpp:18`). And a tree can hold service nodes of its own, whose Open hands the client's channel to your code to speak a protocol on. The root's `/svc` directories are exactly that.

One limit to know before you start. A manifest cannot route a directory that a component provides: the manifest compiler refuses a component that declares or exposes one, and says the only directory is the root's built-in `boot` (`system/component_manifest.py:466`, `system/component_manifest.py:649`). So today a tree server of your own has no manifest route by which another component finds it. The tree servers in the repository are the root's `/boot` and `/svc` and the outgoing directories. The library does not depend on the manifest: `Server::bind`, described below, serves a directory on any channel you give it, and a client can bind a directory channel it holds into its namespace with `fsio::installed_namespace().bind(path, directory)` (`system/lib/fsio/include/fsio/fsio.hpp:33`, `system/lib/fsio/include/fsio/namespace.hpp:95`). How the client comes to hold that channel, for instance as a handle in a reply, is yours to design, and no program in the repository does it yet. This chapter shows the library on the trees the repository does serve.

## The pieces

Objects first; what they do comes in the sections after. Each is one line.

- A *node* is one entry of a tree: a directory, a file, or a service (`system/lib/ioservice/include/ioservice/tree.hpp:35`).
- A `Tree` is a set of node trees, held in an array your program provides (`tree.hpp:86`).
- A *connection* is one client's open handle on one node. It has its own channel, its own rights and its own place (the offset it has read to, or how far down a listing it is).
- A `Server` (`system/lib/ioservice/include/ioservice/server.hpp`) is a table of connections and the rules that answer `ours.io` for a tree. It reads and writes no channel. You give it one message and it gives you an `Outcome`.
- An `Outcome` is what to do next. It holds one of six actions (`server.hpp:77`).
- A `LoopServer` (`system/lib/ioservice/include/ioservice/loop_server.hpp`) is the glue that waits on each connection's channel on a Loop, reads a message, calls the `Server`, and carries out the `Outcome`.
- A `Delegate` is an abstract class your program derives from. The `LoopServer` calls it when an Open reaches a service node.
- `Outgoing` wraps a `LoopServer` over a fixed two-level tree, for a component's outgoing directory. A `Handler` is the abstract class that serves one protocol in it.

```text
client --ours.io--> channel --> Loop slot --> LoopServer --> Server --> Tree
                                                 |   ^          |
                                                 |   +-Outcome--+
                                                 +--> Delegate::connect(service, channel)    your code
```

You can use the `Server` without a `LoopServer` if your program has its own way to wait on channels. This chapter assumes the `LoopServer`, which is what the root and every outgoing directory use. The build targets are `//system/lib/ioservice` for the `Tree` and `Server`, `//system/lib/ioservice:loop_server` and `//system/lib/ioservice:outgoing` (`system/lib/ioservice/BUILD.gn:9`, `system/lib/ioservice/BUILD.gn:49`, `system/lib/ioservice/BUILD.gn:73`). The core is a target of its own so that it makes no system call and its tests run on the host.

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

A *pseudo-directory* is a directory that exists only in memory, with no disk behind it. Your program builds the tree. A client never changes the tree, so there is no removal: a node, once added, stays where it is for the life of the tree (`tree.hpp:18`). The tree may still grow while it is served (`server.hpp:160`).

Building goes through four calls, each of which returns the new `NodeId` or a status (`tree.hpp:105`):

- `add_root()` makes a directory with no parent. A tree can hold several roots, and the `Server` can serve any directory of it on a channel. The root's tree holds `/boot` as one root and a `/svc` directory for each component as the others (`system/service/init/boot_server.hpp:33`).
- `add_directory(parent, name)` adds an empty directory.
- `add_file(parent, name, file)` adds a file, described by a `VmoFile`: its `vmo`, its `bytes`, and whether it is `executable`.
- `add_service(parent, name, service)` adds a service node and a `u64` that your program chooses, to tell its services apart.

Here is a small tree, the one the host tests of the `Server` serve (`server_test.cpp:87`):

```text
/// The tree every test serves:
///
///   root/                 a directory
///     bin/
///       x                 an executable file
///     config/
///       readme            a plain file, "hello, world"
///   svc/                  a second root, as one component's /svc
///     echo                a service
```

and the calls that build its first three nodes (`server_test.cpp:100`):

```cpp
        m_root = m_tree.add_root().unwrap();
        m_bin = m_tree.add_directory(m_root, "bin").unwrap();
        m_x = m_tree
                  .add_file(m_bin, "x",
                            {.vmo = VMO_X, .bytes = bytes_of(X_CONTENTS), .executable = true})
                  .unwrap();
```

`unwrap()` traps if the call failed, which is right for a test and for a call that cannot fail (`system/lib/ioservice/src/outgoing.cpp:26`); a program whose storage could be too small checks the returned status instead. A `NodeId` is an index into your storage and means something only to the tree that returned it (`tree.hpp:24`). Adding is refused with `InvArgs` for an invalid name or a parent that is not in this tree, `NotDir` for a parent that is not a directory, `AlreadyExists` for a name already in the directory, and `NoResource` when the storage is full (`tree.hpp:116`).

What to know before you write the first `add_`:

- **The storage is yours and fixed.** A user program has no heap (`system/lib/ort/src/cpp_dependencies.cpp:16`), so you declare an array of `Node` and give it to the `Tree`, which allocates nothing. A node past the array answers `NoResource` (`tree.hpp:88`).
- **A name is kept as a view.** `add_directory`, `add_file` and `add_service` do not copy the name, so the bytes must outlive the tree (`tree.hpp:115`). The root's `/boot` passes views into the read-only mapping of the boot image. A name from a temporary string leaves the tree pointing at freed memory.
- **A file lends two things.** Its `bytes` are what `Read` copies from; its `vmo` is what `GetBackingMemory` duplicates. Nothing here is owned by the tree: you keep both alive for as long as the tree is served, and nobody may be able to write the VMO (`tree.hpp:47`). The VMO must hold the rights the library duplicates: Basic, Map, GetProperty and Read, plus Execute for an executable file, and Duplicate (`tree.hpp:49`).
- **Names and paths are strict.** A name is 1 to 255 bytes, not `.` or `..`, with no `/` and no NUL byte. A path is at most 1024 bytes and is either exactly `.` or valid names joined by `/`: no leading or trailing `/`, no `//`, no `.` or `..` segment (`tree.hpp:200`, `tree.cpp:48`, `ours_io.hpp:54`, `ours_io.hpp:59`). A path therefore has one spelling, and a walk down a path can never leave the directory it started in (`tree.cpp:191`).
- **Children are kept in name order, and found by walking the list.** A listing comes out sorted. Finding a name costs a walk over the directory's children, which is fine at the sizes in use and is recorded as a known limit in `docs/repairs/261004/ioservice.md`.

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

## Answering an Open

Open is the method where a tree server decides who may reach what, so it has the most rules. Objects first.

- The *request* has three parts: the `path` to open, relative to the directory the Open is written to; the `flags`, which say which rights the caller wants and which kind of node it expects; and `object`, the server end of a new channel the caller made. Open is one-way, so there is no reply to carry a failure, and a failed Open is an epitaph on `object` (`ours_io.hpp:26`, `ours_io.hpp:42`).
- A *right* is one thing a connection may do. There are nine, `Operations` in `ours_io.hpp:127`: Connect, ReadBytes, WriteBytes, Execute, GetAttributes, UpdateAttributes, Enumerate, Traverse and ModifyDirectory. The `PERM_` bits of the request's flags have the same values, so a request's rights are read straight out of its flags (`system/lib/ioservice/src/server.cpp:47`). Five matter in what follows: Traverse lets a client open a name below a directory, Enumerate lets it list the directory, Connect lets it open a service node, ReadBytes lets it read a file, and Execute lets it ask for a file's memory with Execute. WriteBytes and ModifyDirectory give a client nothing in a read-only tree: an Open of a file that asks for WriteBytes is refused, and an Open that asks to create is refused even on a connection that holds ModifyDirectory. GetAttributes and UpdateAttributes can be held, and no method served here uses them.
- The connection the request arrives on holds some rights, and it is a connection to one node, from which the walk starts.

Events: the `Server` receives the request and checks it. The refusals are `ours::Status` values, and [05-testing-and-debugging.md](05-testing-and-debugging.md) says what a client sees for each. **The order of the checks is part of the protocol**: it decides which error a request that is wrong in several ways gets, and so what a client can learn. A connection without Traverse answers AccessDenied to every name, whether or not it exists, before the tree is looked at (`server.cpp:231`).

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
| 9 | the node's own rules, below | see below |

The checks are `server.cpp:237` to `server.cpp:277`. Take the test tree, and a connection to `root/` that holds `rx*`. An Open of `config/readme` with `PERM_READABLE` passes steps 1 to 4 (the path is valid, the connection holds Traverse and every right asked for, and no flag names a service), has no creation flag, finds the file, and the file takes the flags. The connection that results holds ReadBytes and GetAttributes: the other rights of `PERM_READABLE` mean nothing for a file and are dropped (`server_test.cpp:357`). Step 3 is why a connection can never grow its rights (`server.cpp:245`):

```cpp
    // 3. A connection never grows: what it opens holds a subset of its
    //    rights. This is what makes a namespace entry's rights a ceiling
    //    for everything opened through it.
    if ((requested & ~connection.m_rights) != Operations::None) {
        return ustl::err(Status::AccessDenied);
    }
```

Step 9 depends on the kind of node found (`server.cpp:279`):

- **A directory** accepts the request if its flags name a directory or no kind at all. A request that names only another kind is refused: `NotFile` if the flags would accept a file, `BadType` otherwise. Append or truncate flags are `InvArgs`. The new connection keeps every right the request asked for, because a directory's rights are the ceiling for what is opened below it.
- **A file** accepts the request if its flags name a file or no kind at all. A directory-only request is `NotDir`, and any other mismatch `BadType`. Truncate without WriteBytes is `InvArgs`. The new connection keeps only the rights that mean something for a file, and the rest are dropped without an error: opening a file with the flags of a readable directory (`PERM_READABLE`) gives a connection with ReadBytes and GetAttributes only (`server_test.cpp:357`). A file is never writable, so asking for WriteBytes is `AccessDenied`. Asking for Execute on a file that was added without `executable` is `AccessDenied` too.
- **A service node** takes no rights and no other flag. Flags that name another kind are `NotDir`, `NotFile` or `BadType`; any other flag is `InvArgs`. The connection must hold Connect, however the request is spelled, or the answer is `AccessDenied`. The next section is about what happens when all this passes.

If the checks pass for a directory or a file, the `Server` makes a connection for the request's channel and answers `Bind`. If they fail, it answers `Refuse`: an epitaph, written on the request's `object` channel, which is then closed. The connection the request came on stays. The caller learns of a failure only from the epitaph. Success is silent: the caller goes on using the channel. The tests in `system/lib/ioservice/test/server_test.cpp` pin each line of the table; for example (`server_test.cpp:282`):

```cpp
TEST_F(IoserviceServer, OpenChecksRightsBeforeTheTree) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "nope", Flags::PermWriteBytes), Status::AccessDenied);
    EXPECT_EQ(refused(root, "bin", Flags::PermModifyDirectory), Status::AccessDenied);
    EXPECT_EQ(refused(root, "nope", ours_io::PERM_READABLE), Status::NotFound);
}
```

### The other methods

A directory connection serves Open, ReadDirents, Clone and Close. A file connection serves Read, GetBackingMemory, Clone and Close (`server.hpp:182`).

- **ReadDirents** needs Enumerate (`AccessDenied`). A listing starts with `.` and then the children in name order, as many whole entries as fit in the caller's `max_bytes`: `.`, `bin`, `config` for the test tree's root (`server_test.cpp:394`). If the first entry of a reply does not fit, the answer is `BufferTooSmall`. A `max_bytes` above the largest reply is `OutOfRange`. The end of the listing is an Ok reply with no entries. The cursor lives in the connection (`server.cpp:352`).
- **Read** needs ReadBytes (`BadHandle` without it). A `count` above the largest transfer, 4072 bytes, is `OutOfRange`. It copies from the file's `bytes` at the connection's offset and moves the offset. At the end it answers Ok with no bytes (`server.cpp:400`; `ours_io.hpp:64`).
- **GetBackingMemory** hands out a duplicate of the file's VMO. Asking for Read, Write or Execute needs the same right on the connection (`AccessDenied`). Asking for a private clone and a shared buffer together is `InvArgs`. The duplicate has Basic, Map and GetProperty, plus Read and Execute as asked. A private clone gets the same duplicate, which is a private copy only because nobody can write the VMO (`server.cpp:421`).
- **Clone** makes another connection to the same node with the same rights, starting from the beginning again. If the table is full the answer is a `NoResource` epitaph on the new channel (`server.cpp:510`).
- **Close** is answered Ok, then the connection ends and its channel is closed (`server.cpp:497`).

Anything else is not served on that connection: a File method on a directory, a method nobody defines, a message that does not decode, a reply sent as a request. The `Server` answers `Drop`: the connection ends, no epitaph is written, and the handles the message carried are closed. `ours.io` is a closed protocol, in the sense of [03-protocols.md](03-protocols.md#closed-protocols), and the header has no bit that would let an old server say "unknown method" (`server.cpp:561`).

## Connections and their ids

The `Server` keeps its connections in a table you provide, and it never hands out a pointer into it. It hands out a `ConnectionId` (`server.hpp:42`):

```cpp
struct ConnectionId {
    u32 index;
    u32 generation;

    friend constexpr auto operator==(ConnectionId, ConnectionId) -> bool = default;
};
```

The `index` says which entry of the table, and the `generation` says which life of that entry. Objects first:

- An entry of the connection table is free until a connection is made in it; it is reused after the connection ends.
- A `ConnectionId` is the name of one connection, kept by whoever waits on its channel. In the `LoopServer` the object its Loop slot waits on keeps it.
- The generation of an entry goes up by one each time a connection in it ends (`system/lib/ioservice/src/server.cpp:154`).

Events, with entry 3:

1. A client opens a directory. The `Server` makes a connection in entry 3, which has generation 1, and the program keeps `{3, 1}`.
2. The client closes. The `Server` releases entry 3, which moves to generation 2.
3. Another client opens a directory. The `Server` makes its connection in entry 3, now `{3, 2}`.
4. A late message arrives for `{3, 1}`: a copy of the id that the program kept, or an event queued before the release. `find()` sees that entry 3 is at generation 2, finds no connection, and `dispatch()` answers `Drop`.

Without the generation, step 4 would be served as the second client's connection: its rights, its offset, its files. The Loop guards its own slots the same way, with a key made of a generation and an index (the comment on `Key` in `system/lib/loop/include/loop/loop.hpp`).

Two details. Generation 0 never names a live connection: every entry starts at generation 1, and a release that would wrap to 0 skips it (`server.hpp:37`). So `ConnectionId{}`, or a table of ids zeroed at start, is stale by construction and cannot name whichever connection sits in entry 0. The release looks like this (`server.cpp:159`):

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

The test shows the three facts at once, a reused entry, a new generation and a stale id (`server_test.cpp:581`):

```cpp
    // Releasing one frees its slot under a new generation: the old id stays
    // stale even though its slot serves again.
    m_server.release(root);
    auto const again = bind(m_root, ours_io::R_STAR_DIR);
    EXPECT_EQ(again.index, root.index);
    EXPECT_NE(again.generation, root.generation);
    EXPECT_TRUE(m_server.channel(root).is_none());
```

The `Server` never writes or closes a channel. It only records the channel of each connection so that you can find it from the id (`server.hpp:136`). Whoever drives it must close the channel when a connection ends. With a `LoopServer` the Loop does: the connection's callback answers `Close`.

The first connection to a tree has no Open to make it. `Server::bind(channel, directory, rights)` serves a directory on a channel "as if a client had opened it": no message is sent (`server.hpp:166`). The root uses it to give a component a namespace entry, and `Outgoing::serve()` uses it for the outgoing directory.

## Rights only shrink

Four places decide what a connection may do, and each can only keep or shrink what the one before it gave.

- **The manifest.** A use of the root's `boot` states rights, and so does each offer on the way. Walking from the use toward the source, a hop that states rights must state at least the rights of the hop before it, or the route is `AccessDenied` (`system/lib/component/src/route.cpp:95`). The dev image uses this on purpose. `routing_checks` uses `boot` as `/boot` with `rx*` (read and execute), as `/boot-r` with `r*` through an offer that renames it, and as `/denied` with `rx*` through an offer that states only `r*` (`system/service/init/test/routing_checks/BUILD.gn:70`, `kernel/products/image/dev/BUILD.gn:123`, `kernel/products/image/dev/BUILD.gn:132`). The route to `/denied` fails, the root closes that entry with an `AccessDenied` epitaph (`system/service/init/realm.cpp:525`), and every open under it answers `AccessDenied` (`system/service/init/test/routing_checks/routing_checks.cpp:317`).
- **The binding.** When the root binds a directory to a component, it passes the route's rights to the tree server. This is the ceiling for everything opened through that connection (`system/service/init/realm.cpp:514`):

```cpp
            served = m_boot->serve(server, routed->subdir.view(), to_operations(*routed->rights));
```

- **Open.** Step 3 above: what a connection opens holds a subset of its rights. A file shrinks further to the rights that mean something for a file, and is never writable.
- **GetBackingMemory.** The VMO it hands out can do no more than the connection can (`server.cpp:429`). A file's Execute is double-gated: the file must have been added `executable` for a connection to hold Execute, and the connection must hold Execute for the VMO to carry it (`server.cpp:309`).

Clone keeps the same rights (`server.cpp:515`), so cloning is not a way around any of this.

The rights words a manifest uses are constants in `ours_io.hpp:141`: `r*`, `rw*`, `rx*`, `w*` and `x*`. The comment there says the built-in `boot` directory is `rx*`, and each component's `/boot` carries the rights its own route ends with. A component's `/svc` is served with `r*`, which holds Connect, the right needed to open a service node, and Traverse (`system/service/init/realm.cpp:90`).

## Service nodes: when an Open reaches your code

A service node is where the file protocol ends. Objects:

- The *service node*, added with a name and a `u64` value of your choosing.
- The *request's channel*: the server end of a channel that the client made and kept the other end of (`system/lib/fsio/include/fsio/fsio.hpp:52`, `system/lib/fsio/include/fsio/fsio.hpp:82`).
- The *delegate*: the object you gave the `LoopServer`.

Events: an Open of a service node that passes steps 1 to 4 and the service rules above answers `Connect` instead of `Bind`. The `LoopServer` calls `Delegate::connect(service, channel)` with the node's value and the request's channel (`system/lib/ioservice/include/ioservice/loop_server.hpp:54`):

```cpp
        /// Where an Open of a service node goes: |service| is the value the
        /// node was added with and |channel| the request's, which the
        /// function owns from the call on. It serves the channel, forwards
        /// it, or closes it.
        virtual auto connect(u64 service, ours::Uandle channel) -> void = 0;
```

From that call on the channel is yours, and it no longer speaks `ours.io`: it carries the protocol's own messages. What you may do with it:

- *Serve it*, by adding it to your Loop, as `echo` does for each client (`examples/echo/echo_server.cpp:126`).
- *Forward it*, by writing it into another channel as the server end of a new Open. The root does this for a protocol a sibling provides (`system/service/init/realm.cpp:786`).
- *Close it*, with an epitaph first if you want the client to learn why (`examples/echo/echo_server.cpp:75`).

Never keep it and do nothing: the client would wait for an answer that never comes. You give the delegate to the `LoopServer`'s constructor, or later with `set_delegate()` when the delegate can only exist after the server does (`loop_server.hpp:134`). The function runs inside a Loop callback, so it may add slots to the Loop but must not wait (`loop_server.hpp:44`). If no delegate has been set, the `LoopServer` closes the channel itself (`system/lib/ioservice/src/loop_server.cpp:157`).

The value is how the delegate tells which service was opened. The two uses in the repository are instructive. `Outgoing` adds each protocol with the index of its handler and looks the handler up by that index (`system/lib/ioservice/src/outgoing.cpp:38`). The root packs two numbers into the value, which instance of the component pool and which of its uses (`system/service/init/realm.cpp:97`):

```cpp
    constexpr auto service_key(usize const instance, u32 const use) -> u64 {
        return (static_cast<u64>(instance) << 32) | use;
    }
```

A client's Open of `/svc/ours.test.Echo` passes through two tree servers on its way to `echo`. The first is the root's, whose delegate is the root's `Realm`; the second is `echo`'s outgoing directory, whose delegate is `Outgoing`:

```text
client:  Open("ours.test.Echo", ProtocolService, S)         on its /svc connection
root:    the /svc tree finds the service node, answers Connect
         Realm::connect(key, S) routes the use, then writes
         Open("svc/ours.examples.echo.Echo", ProtocolService, S) on echo's outgoing directory
echo:    Outgoing's tree walks svc -> the node, answers Connect
         Outgoing::connect(index, S) -> Handler::connect(S): echo adds S to its Loop
```

Each hop writes and does not wait (`system/service/init/realm.cpp:809`). From then on the client and `echo` speak directly on `S`.

## Storage you lend, and why it is `constinit`

A tree server holds a lot of fixed-size state: the nodes, the connection table, one per-connection record for the Loop, and the buffers a message is read into and an answer is built in. A user program has no heap, so you provide all of it, sized by how many connections and nodes you allow. `LoopServer` takes it as a `Memory`, which names each region (`loop_server.hpp:104`). `Outgoing` and `BootServer` each group theirs in a `Storage` struct (`outgoing.hpp:96`, `system/service/init/boot_server.hpp:58`). The exact fields of `Memory`, and of the per-connection record the Loop calls, are in `loop_server.hpp`, which is the reference: they are being reworked as the Loop's callback changes.

Three rules decide the sizes.

- **Every connection is a Loop slot.** One slot waits on each connection's channel. Size the Loop's table for the connections plus everything else your program waits on, and check it at build time against the port's limit. `echo` does so with `static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS)` (`examples/echo/echo_server.cpp:58`). The root's version, for 128 connections among others, is in `system/service/init/pools.hpp:74`:

```cpp
inline constexpr auto LOOP_SLOTS = 3 * MAX_INSTANCES + MAX_CONNECTIONS + MAX_LAUNCHER_CONNECTIONS +
                                   RESOURCE_SERVERS * MAX_RESOURCE_CONNECTIONS;

// Each slot in use holds one live wait on the port, armed or with its packet
// queued, and the port refuses an observer past its limit
// (<loop/loop.hpp>, Loop::Loop()).
static_assert(LOOP_SLOTS <= ours::PORT_MAX_OBSERVERS);
```

- **A record for each connection.** The Loop needs one per-connection record for each entry of the connection table. With fewer records than entries, a connection past them cannot be waited on and is refused (`loop_server.hpp:107`).
- **The buffers hold the largest message.** The request buffers must hold 4096 bytes and 64 handles, or a read of a large message fails and its connection ends (`loop_server.hpp:111`). The reply buffer is as large as a message (`server.hpp:63`).

An `Outgoing` costs about 9 KiB and the root's `BootServer` about 42 KiB, both quoted from their headers (`outgoing.hpp:92`, `system/service/init/boot_server.hpp:54`). That is too much for the stack, so the storage is a global of the program's. `constinit` is the C++ keyword that makes the compiler insist a global is initialised at compile time, and storage here is `constinit` for a reason that also explains why it sits in a `ustl::LazyInit`.

- Nothing runs a user program's dynamic initialisers. There is no walk over `.init_array`, so a global that needs a constructor to run would silently start out as zeros (`system/lib/ort/src/start.cpp:42`). A `constinit` global is built at compile time and is in the program image.
- The storage contains `Connection`, which is not trivially destructible: it holds a `ustl::Option` (`system/lib/ustl/include/ustl/option.hpp:66`). A plain global of such a type would have its destructor registered with `__cxa_atexit`, which the runtime does not have (`examples/echo/echo_server.cpp:238`).
- `ustl::LazyInit<T>` is raw, suitably aligned bytes with a `constexpr` constructor, so it can be `constinit`. Your `main()` builds the real object in it once with `init()` (`system/lib/ustl/include/ustl/lazy_init.hpp:12`).

`echo` declares its storage and uses it like this (`examples/echo/echo_server.cpp:238`):

```cpp
// What the server keeps, outside the stack: constinit, because nothing runs
// a user program's dynamic initialisers. The outgoing directory's storage
// holds types with destructors (ustl::Option, in the ioservice connections), which
// as a plain global would register them with __cxa_atexit, which the runtime
// does not have; ustl::LazyInit is raw storage that main() builds it in
// instead, as init's are (system/service/init/init.cpp).
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<EchoServer::Storage> ECHO_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
```

and `main` builds it (`examples/echo/echo_server.cpp:264`):

```cpp
    loop::Loop loop(port, SLOT_STORAGE);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    EchoServer server(*ECHO_STORAGE.init(), loop);
```

Call `init()` once per `LazyInit`. The root does the same for `/boot` (`system/service/init/init.cpp:107`, `system/service/init/init.cpp:272`). Note that the `Slot` table of the Loop is a plain `constinit` array: a `Slot` is trivially destructible.

## Putting a tree on a Loop

### LoopServer

`LoopServer` takes the tree, the Loop, the storage, the `Syscalls` and a delegate (`loop_server.hpp:127`). The one system call the core makes is the duplicate that GetBackingMemory hands out. Your program passes `ours::handle_duplicate`, as `Outgoing` does (`outgoing.cpp:17`), and a host test passes a function of its own (`server.hpp:52`). Then `serve(channel, directory, rights)` binds a directory to a channel and waits on it (`loop_server.hpp:140`). On success the channel is the Loop's. On failure nothing changed and the channel is still yours. The refusals are `InvArgs`, `NotDir`, and `NoResource` when every connection is used, a connection has no record, or the Loop is full.

Each connection's slot waits for Readable or PeerClosed (`system/lib/ioservice/src/loop_server.cpp:15`). As the Loop's contract says, the Loop calls the slot's callback with the signals the wait observed, and the callback answers `Rearm` (wait again) or `Close` (close the handle and free the slot), as [01-concepts.md](01-concepts.md#the-loop-and-its-slots) explains; the exact form of the callback, and of how a `LoopServer` registers one, is in [loop.hpp](../../system/lib/loop/include/loop/loop.hpp) and `loop_server.hpp`. What the `LoopServer`'s callback does:

1. It finds the connection from its id. A connection that has gone answers `Close`.
2. If the client closed and nothing is left to read, it releases the connection and answers `Close`. The Loop then closes the channel.
3. Otherwise it reads one message, hands it to `Server::dispatch()`, and acts on the `Outcome`. Unless the table below says otherwise it then answers `Rearm`, so that a client with more messages waits behind everyone else (`loop_server.cpp:78`).

| `Outcome` | What `LoopServer` does | Answer to the Loop |
|---|---|---|
| `Reply` | writes the message on the connection's channel; if the write fails, tells the delegate with `write_failed()` and releases the connection | `Rearm`; `Close` on a failed write |
| `ReplyAndClose` | writes the message, ignoring failure | `Close` |
| `Drop` | closes every handle the message carried | `Close` |
| `Bind` | waits on the request's channel as a new connection; if it cannot, writes an epitaph with the reason on it and closes it | `Rearm` |
| `Connect` | calls `Delegate::connect()`, or closes the channel if there is no delegate | `Rearm` |
| `Refuse` | writes the epitaph on the request's channel, then closes it | `Rearm` |

Those are `loop_server.cpp:114` to `loop_server.cpp:168`. A reply that cannot be written ends its connection, because the client would otherwise wait for good (`loop_server.cpp:115`).

### Outgoing

If what you serve is protocols, `Outgoing` is the whole of this for you. It builds the tree (the outgoing directory, its `svc/` directory and a service node per protocol), serves it, and calls your `Handler` when a client's Open of `svc/<name>` arrives. A `Handler` is an abstract class with one function that receives the client's channel. Its exact form is in `outgoing.hpp:46`. Its rules are the delegate's: you own the channel from the call, serve it or close it, and do not wait. The wiring in `echo`'s `main` is (`examples/echo/echo_server.cpp:268`):

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
```

`add_protocol(name, handler)` is refused with `InvArgs` for an invalid name, `AlreadyExists` for a name already added, and `NoResource` once 8 protocols are served (`outgoing.hpp:69`, `outgoing.hpp:120`). At most 4 connections to the outgoing directory may be open at once (`outgoing.hpp:76`). The name and the handler are kept by address, so they must outlive the directory. `serve()` takes the `DirectoryRequest` handle that you took at the start of `main` (`outgoing.hpp:132`).

## Worked example: the root's `/boot`

`BootServer` (`system/service/init/boot_server.hpp`) is the biggest user of everything above. Objects:

- The **tree**: one root for `/boot`, with a directory per directory of the boot image and a file per file, and further roots, one `/svc` directory for each component the root starts.
- The **`LoopServer`** that serves all of them from one connection table, 128 connections (`system/service/init/pools.hpp:27`).
- The **`Storage`**: 256 nodes for `/boot` and 128 for `/svc` in one array, and the buffers (`pools.hpp:51`, `pools.hpp:62`, `system/service/init/boot_server.hpp:58`).
- The **delegate**: the root's `Realm`, which derives from `LoopServer::Delegate` (`system/service/init/realm.hpp:147`).

Events:

1. `main` has the storage in a `constinit ustl::LazyInit` and builds the `BootServer` over it (`system/service/init/init.cpp:272`).
2. `ingest()` reads the boot image, mapped read-only. It splits each flat name such as `bin/shell` on `/`, makes the directories on the way with `add_directory`, makes a read-only slice of the image's VMO for the file, replaces the slice with an executable one under `bin/`, and calls `add_file` as shown above. A name the tree refuses is logged and left out; running out of nodes fails the whole ingest with `NoResource` (`system/service/init/boot_server.cpp:45`).
3. The root starts a component. For each directory the component uses, it routes the use, makes a channel pair, and calls `BootServer::serve(server_end, subdir, rights)`, which finds the subdirectory and calls `LoopServer::serve`. No message is sent. The client end goes to the component as a startup handle that `fsio` binds into the namespace (`system/service/init/realm.cpp:514`, `system/service/init/realm.cpp:527`).
4. If the component uses protocols, the root calls `add_svc_directory()`, which is `Tree::add_root()`, adds one service node per protocol use with `service_key(...)` as its value, and serves that directory with `r*` (`realm.cpp:692`, `realm.cpp:713`, `realm.cpp:719`).
5. A client opens a file under `/boot`. The checks of the table run on the client's connection, which holds the rights the route gave it. Read copies from the mapping into the reply; GetBackingMemory duplicates the file's VMO.
6. A client opens a service in its `/svc`. The `Connect` outcome reaches `Realm::connect()`, which routes it, as the diagram in the previous section showed.

What to take from it for your own tree server:

- **Decide the sizes first, as constants.** `pools.hpp` states every limit and `static_assert`s the sums, so a limit that cannot hold is a build error, not a failure at run time.
- **Share one array fairly.** /boot's nodes and /svc's nodes are counted separately inside one array, so /boot cannot take /svc's share (`boot_server.hpp:143`).
- **Read your own tree by direct call.** The root finds a file with `BootServer::file(path)`, which is `Tree::lookup` and `Tree::file`, never by opening it over a channel (`boot_server.hpp:49`, `boot_server.cpp:149`). The root has one thread, and a message it sent itself would wait for a reply that only it can write: invariant I2 of `docs/design/261004/component-framework.md` (`docs/design/261004/component-framework.md:191`).
- **Set the delegate when it exists.** `BootServer` is built before `Realm`, and `Realm` needs the `BootServer`, so the delegate is set after, by `Realm`'s constructor (`realm.cpp:309`). Until then an Open of a service node closes its channel.
- **Names are views.** The tree's names point into the mapping of the boot image, which stays mapped for good (`boot_server.hpp:78`).

## Testing without a kernel

The `Server` touches no channel and makes one system call through a function you give it, so a host test builds a tree and drives it one encoded message at a time. The fixture of `server_test.cpp` is five members (`system/lib/ioservice/test/server_test.cpp:239`):

```cpp
    std::array<ioservice::Node, 16> m_nodes{};
    ioservice::Tree m_tree{m_nodes};
    std::array<ioservice::Connection, 4> m_connections{};
    ioservice::Server m_server{m_tree, m_connections, {.handle_duplicate = fake_duplicate}};
    ioservice::ReplyBuffer m_out{};
```

A test encodes a request with the `ours.io` codec, calls `dispatch()` and reads the `Outcome` as a program would act on it (`server_test.cpp:135`):

```cpp
    auto open(ConnectionId const connection, std::string_view const path, Flags const flags,
              Uandle const object = OBJECT) -> Outcome {
        return dispatch(connection,
                        ours_io::encode_open(m_bytes, m_handles,
                                             {.flags = flags, .path = path, .object = object}));
    }
```

Handles are plain numbers there, and `fake_duplicate` records what it was asked. The `LoopServer` and `Outgoing` need a Loop, so their tests run on a model of ports and channels, `fake::Kernel`. It defines the vDSO's channel and port functions on the host under their real names, so the library's source is compiled unchanged (`system/lib/ioservice/test/fake_kernel.hpp:15`). The three test programs are `//system/lib/ioservice/test:ioservice`, `:loop_server` and `:outgoing` (`system/lib/ioservice/test/BUILD.gn`). How to run them, and how to boot the image, is in [05-testing-and-debugging.md](05-testing-and-debugging.md).

## What a tree server does not do

- **It does not write.** There is no create, remove or rename: a creating Open is refused and a file is never writable. A disk filesystem is a separate server and need not use this library (`system/lib/ioservice/README.md:5`).
- **It has no attributes, no symlinks and no node-only connections.** An Open that asks for a node-only connection or the OnRepresentation event is `NotSupported`, since a client served without it would wait for an event that never comes (`server.cpp:40`).
- **It is single-threaded.** One thread calls the `Server`, one message at a time (`server.hpp:155`).
- **Lookups and new connections scan.** Finding a child walks its directory's list, and finding a free connection walks the table. Both are recorded as repair items with the measurement to take first (`docs/repairs/261004/ioservice.md`).
- **No manifest routes a directory a component serves**, as the first section said.

## Where to look next

- [05-testing-and-debugging.md](05-testing-and-debugging.md): running the host tests, booting the image, and what a client sees for each failure above.
- [06-checklist.md](06-checklist.md): the rules for the callback, who closes which handle, and the limits, as a checklist.
- [03-protocols.md](03-protocols.md): the protocol a service node's channel speaks.
- [01-concepts.md](01-concepts.md#the-loop-and-its-slots): the Loop and its slots, which the sizing rules above rest on.
- Chapter 2, the first server, is in preparation.
- The headers are the reference: `tree.hpp`, `server.hpp`, `loop_server.hpp`, `outgoing.hpp` under `system/lib/ioservice/include/ioservice/`.

## Sources

- `system/lib/ioservice/README.md`, `system/lib/ioservice/BUILD.gn`: what the library is and is not, and its targets.
- `system/lib/ioservice/include/ioservice/tree.hpp`, `system/lib/ioservice/include/ioservice/server.hpp`, `system/lib/ioservice/include/ioservice/loop_server.hpp`, `system/lib/ioservice/include/ioservice/outgoing.hpp`: the tree, the `Server`, the `LoopServer` and `Outgoing`.
- `system/lib/ioservice/src/tree.cpp`, `system/lib/ioservice/src/server.cpp`, `system/lib/ioservice/src/loop_server.cpp`, `system/lib/ioservice/src/outgoing.cpp`: the walk, the Open checks in order, the generation, and what the `LoopServer` does with each `Outcome`.
- `system/lib/ioservice/test/server_test.cpp`, `system/lib/ioservice/test/tree_test.cpp`, `system/lib/ioservice/test/loop_server_test.cpp`, `system/lib/ioservice/test/outgoing_test.cpp`, `system/lib/ioservice/test/fake_kernel.hpp`, `system/lib/ioservice/test/BUILD.gn`: what each answer is pinned to, and how to drive a server on the host.
- `system/service/init/boot_server.hpp`, `system/service/init/boot_server.cpp`, `system/service/init/pools.hpp`, `system/service/init/init.cpp`, `system/service/init/realm.hpp`, `system/service/init/realm.cpp`: the root's `/boot` and `/svc`.
- `system/service/init/test/routing_checks/BUILD.gn`, `system/service/init/test/routing_checks/routing_checks.cpp`, `kernel/products/image/dev/BUILD.gn`: the narrowed `/boot` entries of the dev image and the checks that run against them.
- `examples/echo/echo_server.cpp`: an outgoing directory and its storage.
- `system/lib/ours.io/README.md`, `system/lib/ours.io/include/ours_io/ours_io.hpp`: the file protocol, its flags, rights and limits.
- `system/lib/loop/include/loop/loop.hpp`, `system/lib/fsio/include/fsio/fsio.hpp`, `system/lib/fsio/include/fsio/namespace.hpp`, `system/lib/wire/include/wire/wire.hpp`, `system/public/ours/syscall.hpp`, `system/public/ours/port.hpp`: the Loop, the client calls, the epitaph, the message and observer limits.
- `system/lib/ort/src/cpp_dependencies.cpp`, `system/lib/ort/src/start.cpp`, `system/lib/ustl/include/ustl/lazy_init.hpp`, `system/lib/ustl/include/ustl/option.hpp`: no heap, no dynamic initialisers, `LazyInit`.
- `system/lib/component/src/route.cpp`, `system/component_manifest.py`: the rights a route may state, and what a manifest may declare.
- `kernel/object/include/ours/object/vmo_dispatcher.hpp`: what a VMO is.
- `docs/design/260924/userspace-filesystem.md` (the Loop's key, section 4.2), `docs/design/261004/component-framework.md` (the root's design, invariant I2, decisions D10, D21 and D27), `docs/changes/261008/ioservice.md`, `docs/changes/261028/ioservice.md`, `docs/repairs/261004/ioservice.md`, `docs/architecture/component-framework/glossary.md`: the reasons behind the code, and the known limits.

Checked against commit `693260d0` on 2026-10-29.
