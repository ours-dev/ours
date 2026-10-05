# vfs

An ours.io server for read-only trees (component-framework design, "vfs"): the root serves /boot and each component's /svc with it. It is the counterpart of Fuchsia's Rust vfs (`src/storage/lib/vfs/rust`), cut to what ours uses.

- `vfs::Tree` (`<vfs/tree.hpp>`) holds the three node kinds: pseudo-directories, read-only VMO files and services. The program adds the nodes; a client never does. Nodes live in storage the program provides, since user programs have no heap.
- `vfs::Server` (`<vfs/server.hpp>`) holds the connection table and handles ours.io (`//system/lib/ours.io`): Open with the Rust vfs's checks in its order, ReadDirents, Read, GetBackingMemory, Clone and Close.

The server touches no channel. `Server::dispatch()` takes one message and returns an `Outcome`: what to write and where, which new channel to wait on, which channel a service was asked for, or that the connection is to be dropped. The program does the writing and the waiting. Its one system call, the duplicate GetBackingMemory hands out, goes through a function the program gives it, so the whole server runs on the host, where its tests are.

## Layout

```text
include/vfs/   public headers, reached as <vfs/...>
src/           sources
test/          host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/vfs`; its headers alone are `//system/lib/vfs:headers`. The gtest executable is `//system/lib/vfs/test:vfs`, built on the host by the default build.
