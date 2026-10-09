# ioservice outstanding repairs

## 1. Finding a child walks the directory's whole list of children

**Severity**
Low: a performance ceiling, not measured; correct at every size.

**Symptom**
Latent, not visible at today's sizes. Every Open costs, for each segment of its path, a walk of that directory's children with a string compare per child, and so does every `Tree::add_*()`. Building /boot from a BootFS of N files under one directory costs O(N²) compares, and opening `/boot/bin/x` costs O(N) once `bin/` holds N programs. The root serves every Open on its one thread (design D16), so a slow lookup delays every other client too (design worst interleaving 1).

**Cause**
`ioservice::Tree` keeps a directory's children as one singly linked list threaded through the nodes, in name order (`system/lib/ioservice/src/tree.cpp`, `Tree::find_child()` and `Tree::add_child()`). The list needs no storage beyond the nodes, which matters with no heap, and gives ReadDirents its name order and an O(1) cursor for free. Finding a name in it is linear. The Rust vfs keeps a `BTreeMap<Name, Arc<dyn DirectoryEntry>>` per directory (`src/storage/lib/vfs/rust/src/directory/simple.rs:44-57`), O(log N) per lookup and insertion.

**Change**
Measure first, as CLAUDE.md asks of performance work: Opens per second of `bin/<last>` against the number of entries in `bin/`, and the time to build the tree from the dev BootFS, on the host (the core runs there) and, once step 13 lands, on the target. If it matters, the candidates are:
- a sorted array of child ids per directory, filled once at ingest, searched by binary search; needs per-directory storage, sized at ingest, and keeps ReadDirents' cursor an index;
- a hash of the name in each node, compared before the string, which keeps the list and cuts the cost of a miss to one integer compare per child;
- an intrusive balanced tree (ustl has intrusive containers), O(log N) for both, at two or three links per node.

The choice caps lookup cost for every Open, so it goes to the user with the numbers.

**Reference**
Fuchsia `vfs::directory::simple::Simple` (`src/storage/lib/vfs/rust/src/directory/simple.rs:44-57`), a BTreeMap; the C++ vfs's `PseudoDir` keeps two intrusive WAVL trees, by id and by name (`src/storage/lib/vfs/cpp/pseudo_dir.h:94-97`, `:125`).

**Acceptance**
A benchmark in `benches/` gives Opens per second against directory size, before and after, and the user has chosen from those numbers.

## 2. Opening a connection scans the table for a free slot

**Severity**
Low: a performance ceiling, not measured; correct at every size.

**Symptom**
Latent, not visible at today's sizes. Every Open and Clone that succeeds, and every `Server::bind()`, scans the connection table from slot 0 for a free slot: O(table size) per connection, 128 slots under design D27. When the table is nearly full, the scan runs to its end on every Open.

**Cause**
`Server::allocate()` (`system/lib/ioservice/src/server.cpp`) marks a free slot by an invalid channel and looks for one linearly. A free list threaded through the free slots would make allocation and release O(1), at one index per slot.

**Change**
Measure first: Opens per second against the number of live connections, on the host. If it matters, thread a free list through the slots (the Loop's slot table needs the same structure, so the two may share one).

**Reference**
Zircon's `HandleTableArena::Alloc()` takes a handle's storage from an arena's free list (`zircon/kernel/object/handle.cc:103`); FS §4.2 sizes the root's tables but does not say how a slot is found.

**Acceptance**
A benchmark in `benches/` gives Opens per second against live connections, before and after, and the user has chosen from those numbers.
