# directory-capability: a component provides a directory that other components use in their namespaces, so that a filesystem server can be introduced

**Date**: 2026-10-30
**Status**: Proposed

## Problem
A component can provide protocols and nothing else. The only directory in the system is the root's built-in `boot`, which the root serves itself. The maintainer wants the Server system complete before a filesystem is introduced, and a filesystem is a component that provides a directory. Today there is no way to say that in a manifest, and no way for the root to connect a client to it.

Where the limit is written down:

- Design D3 (`docs/design/261004/component-framework.md:372`) fixes "protocol capabilities only, the only directory being the built-in `boot`". The non-goals repeat it, with "no forwarding of a namespace entry into a provider, and no remote node" (`component-framework.md:52`), and "Under D3 (a), rights and subdirs appear only on the way up to the built-in `boot`" (`:301`).
- The manifest compiler refuses a declared directory (`system/component_manifest.py:466`), an offer of a directory from anything but the parent (`:576-581`) and an exposed directory (`:649`).
- The root answers `NotSupported` when a directory use routes to a provider (`system/service/init/realm.cpp:518-523`).
- `ioservice::Outgoing` builds `svc/` and nothing else (`system/lib/ioservice/include/ioservice/outgoing.hpp:98`).
- The developer's guide tells readers that a tree server of their own cannot be reached (`docs/guide/04-tree-servers.md:5`, `:34`, `docs/guide/README.md:60`).

The limit is shallower than it looks. Most of the machinery already treats the two kinds alike, so the work is mostly taking refusals out and adding two hops, the declaration and the expose, with the fields they need:

| Piece | Directory support today | What is missing for a provider to end a directory route |
|---|---|---|
| `route()` (`system/lib/component/include/component/route.hpp`) | Walks a directory use through offers and exposes, matching the kind at every lookup, with the rights and subdir hop rules (`:396`, `detail::Hops`). A test already ends one at a provider, with the bytes patched by hand (`system/lib/component/test/route_test.cpp:1032-1060`). | The declaration at the end states no rights, so the walk applies no hop at the provider (`route.hpp:457-470`), and an expose states none either and is no hop (`:259-260`): neither a declaration that allows less than an offer asks nor an expose that narrows is seen. The answer does not carry the declared path. |
| Compiled format (`format.hpp:169`, `:205`) | `Capability` has a kind, a name and a type; `Expose` a source, a name and a target. | No path or rights on a declaration (24 bytes, `:227`); no rights or subdir on an expose (40 bytes, `:236`); `VERSION` is 2. |
| Compiler | Parses directory uses, and directory offers from the parent, with rights and subdir (`component_manifest.py:495-550`, `:553-636`). | The three refusals above. |
| Root, `Realm::run` (`realm.cpp:497-544`) | Routes each directory use at start, binds the entry to `/boot`'s tree for the built-in. | Writing the Open into a provider's outgoing directory, as `Realm::forward` does for a protocol (`:789-833`). |
| `ioservice::Server` | Serves directory connections with rights, and an Open that reaches a directory makes a connection (`server.cpp:226-349`). | A directory at its declared path in `Outgoing`, and a way for the program to fill it. |
| `fsio` | Binds any NsDir handle at a path (`system/lib/fsio/include/fsio/namespace.hpp`). | Nothing. |

## Goals and non-goals
Goals:

- A component declares a directory in `capabilities`, exposes it, offers it from `self`, from a child or from its parent, and uses it, with the words protocols already have plus a path, rights and subdir. Rights are held against the hop before at every hop, the declaration and the expose included.
- A consumer sees the directory as an ordinary namespace entry. `fsio` and the programs that use it do not change.
- A route that ends at a provider is connected by the root at the consumer's start, with one forwarded Open. Afterwards the root is out of the path of every read, as it is for a protocol.
- A provider can serve a tree at it: a static in-memory tree first, and the seam a filesystem plugs into (decision 7): a Subtree node that hands the Open over, then a handler node whose content may wait, with the Loop leaving a waiting connection unread.
- Every step boots. The negative cases (rights widened by an offer or an expose, a capability that nobody offers, a declared path the program does not serve, a provider that exits) run on the target; those the build refuses, such as overlapping declared paths, run in the compiler's tests.

Non-goals:

- A filesystem, a block device, disks, and the consistency of a writable tree. The subset of ours.io has no writes.
- Remote nodes in Fuchsia's sense: a node in a library's tree that owns a client end and forwards to it, with a policy for a remote that died (`src/storage/lib/vfs/rust/src/remote.rs:60-123`). The Subtree node of decision 7 hands the Open to the program and keeps no policy.
- Restart of a provider, lazy start (D4), revocation. A provider that exits is gone for good (D23).
- More than one request outstanding on a connection (pipelining), and a timeout on a request that waits. See the limits of the handler node.
- Executing a program from a provider's directory. Only a VMEX holder can make a VMO executable (`docs/design/261004/component-framework.md:349`), and no built-in hands VMEX out (`system/service/init/builtins.json`).
- A file's VMO from a provider that serves bytes from a disk. That needs a pager-backed VMO, and `.agents/tasks/page-source/DESIGN.md` stops at the kernel side.
- A global mount table or a VFS process (S1). `docs/changes/261008/ioservice.md` records why the library is not named for one.
- Performance work. The shape keeps the root off the data path; any measurement goes to the maintainer item by item.

## Design
### Objects
- **Directory capability.** A capability of kind directory, named by its provider, with the rights it allows at most. Provided by a component with a program, from its outgoing directory at the path its declaration names (decision 2).
- **Declaration.** The entry in the provider's `capabilities` that states the name, the path and the rights. It is the last hop of a route.
- **Hop.** One declaration of the walk that may state rights and a subdir: the use, an offer, an expose, the provider's declaration, or a built-in (`route.hpp:267-290`; the expose and the declaration are new hops, decisions 3 and 4).
- **Route.** `component::route()`'s answer: the provider, the capability's name there, the subdir joined from every hop, and exactly the use's rights (`route.hpp:113`). For a directory it also carries the declared path.
- **Outgoing directory.** The directory a provider serves on its `DirectoryRequest` handle, whose client end only the root holds. `svc/<name>` holds protocols today (`outgoing.hpp:80`); a directory sits at its declared path, such as `export/data`, and never under `svc`.
- **O.** The root's connection to the outgoing directory: the first connection of the provider's `Outgoing`, served with every right (`outgoing.hpp:85-92`). The root writes the forwarded Opens on it.
- **Entry.** The consumer's namespace entry for the use: the client end N of a channel pair the root made, whose server end S the root sends to the provider.
- **C1.** The connection the provider's server makes for S when it opens the declared path joined with the subdir, with the use's rights.
- **Subtree node.** A node that the program's code answers for: an Open that reaches it, with the rest of its path, goes to the program (decision 7, step 6).

### The language
A provider, the parent that connects it, and a consumer. The names are the document's own; the dev image's first provider is a new example, `files` (decision 10). The `path` of a declaration is a place in the provider's outgoing directory; the `path` of a use is a place in the consumer's namespace.

```gn
# the provider
capabilities = [
  {
    directory = "data"
    path = "/export/data"
    rights = [ "rw*" ]
  },
]
expose = [
  {
    directory = "data"
    from = "self"
    rights = [ "r*" ]
    subdir = "shared"
  },
]

# the parent, root.cm
offer = [
  {
    directory = "data"
    from = "#store"
    to = "#client"
    as = "notes"
    rights = [ "r*" ]
    subdir = "public"
  },
]

# the consumer
use = [
  {
    directory = "notes"
    path = "/notes"
    rights = [ "r*" ]
    subdir = "docs"
  },
]
```

What each hop states, and what the compiler checks at build time, per manifest (`component_manifest.py`) once the three refusals are gone:

| Hop | Written by | States | Build-time rules | Run-time rule in `route()` |
|---|---|---|---|---|
| Declaration | the provider, `capabilities` | a name, a path (required), rights (required) | the name is unique among all capabilities (`:476-478` already counts both kinds); the path is absolute, below `/`, with no trailing `/` and no empty, `.` or `..` segment, as `check_path` holds a use's path (`:251-260`); it is not `/svc` and not under it, by segments (`_is_prefix`, `:488-492`, so `/svcx` is fine); no two directory declarations of one component are equal or one under the other; known rights words, not empty; no `subdir` or `type`; `path` and `rights` refused on a protocol declaration | held against the hop before it: AccessDenied at the provider, verb Declare. The declared path and the joined subdir together must fit `MAX_PATH_LENGTH`: NotFound at the provider, verb Declare |
| Expose | the provider, or any component passing a child's capability up, `expose` | a source (`self` or `#child`), `as`, and for a directory rights and a subdir | `from = self` names a declared directory (`:665-671` already compares kinds); target names unique; `rights` and `subdir` as an offer's, refused on a protocol expose; no `type` | a hop: rights contain the hop before, the subdir goes in front; AccessDenied or NotFound at the child, verb Expose |
| Offer | the parent, `offer` | a source (`parent`, `self`, `#child`), `as`, rights, subdir | `from = self` names a declared directory (`:582-589`); rights words; relative subdir; the cycle check (`:690`) covers directory offers from children | rights contain the hop before; subdir joined in front |
| Use | the consumer, `use` | a name, a path, rights (required), subdir | absolute path, no nesting among use paths (`:538-549`); no `type` (`:523-527`) | the first hop |

The whole-tree check of 2026-10-29 compares protocol types: `check_types` skips every other use (`component_manifest.py:1203`) and `route_protocol_use` walks protocol routes alone (`:1042`). A directory has no type, because every lookup of `route()` matches the kind, so a directory use can reach no protocol (`route_test.cpp:637`); it is held to its rights at every hop. Its remark that a protocol use, offer and expose state no rights and no subdir (`:893-895`) stays true, because the compiler keeps a protocol expose bare.

Decision 9 (c) gives the compiler a second walk over the same tree, for directory uses, which compares rights and only warns:

- **The walk.** `check_rights()` stands beside `check_types()` (`:1183`), takes the same tree (`build_tree()`, `:1134`) and the same lookups (`_find_offer`, `_find_expose`, `_find_capability`), and has a twin of `route_protocol_use` for directories, `route_directory_use`. From the use it goes up through the offers and down through the exposes to the declaration or to a built-in, as `route()` does, and holds each hop that states rights against the last hop that did: `(rights & last) == last`. The built-in `boot` takes its rights from `builtins.json`, as the type check takes its types. A route that does not resolve is left alone, as a protocol route is: the first failure on the walk is the answer, and only an AccessDenied is reported. The subdir joins are not walked, so a joined path that is too long is not reported either.
- **The warning.** It names the use and the rights it asks for, each hop with the rights it states, the hop that holds less, and the fix. It goes to stderr, begins `warning:`, and the compile goes on: the exit status is 0 and the `.cm` is written.

```text
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "denied" with rights rx*,
         but the route holds less at an offer:
           use    #routing_checks "denied"                     rx*
           offer  root.cm "boot" from parent to #routing_checks as "denied"   r*   <- holds less
         The root will answer AccessDenied for this use. Offer rights that contain rx*, or ask the use for fewer.
```

- **The run time is unchanged.** The root still routes the use when it starts the consumer, logs one line and closes the entry with an AccessDenied epitaph (`realm.cpp:515-536`). That line is the signal that matters, for two reasons the build cannot help: ninja shows an action's output only when the action runs, so a build that finds `root.cm` up to date prints nothing, and the warning is easy to miss; and a deliberate failure warns too.
- **Deliberate failures warn on every compile.** routing_checks' `/denied` is one, and so are the new negative entries of the Testing section (an offer wider than its expose, an expose wider than its declaration): `root.cm` prints three warnings each time its action runs. A marker on a use or an offer that says "this failure is meant" and silences its warning is the later step; this design does not add one (open question). The switch that exists, `type_check = false` (`--no-type-check`), turns both walks off, which is what the fixture trees that exercise the run time want.

What changes in the compiled format and in the decoder. `VERSION` goes from 2 to 3 for both records at once (`format.hpp:112`, `component_manifest.py:50`), and each layout has a twin in `format.hpp` and `component_manifest.py`:

| Offset | `Capability`, 40 bytes (was 24) | `Expose`, 56 bytes (was 40) |
|---|---|---|
| 0 | `kind` (u32) | `kind` (u32) |
| 4 | `flags` (u32, was `reserved`) | `flags` (u32, was `reserved`) |
| 8 | `name` (String) | `source` (Ref) |
| 16 | `type` (String) | `source_name` (String) |
| 24 | `path` (String), new | `target` (Ref) |
| 32 | `rights` (u64), new | `target_name` (String) |
| 40 | none | `subdir` (String), new |
| 48 | none | `rights` (u64), new |

- `Capability` gains `path` and `rights`, and `flags` in place of `reserved`. `rights` sits at 32, a multiple of 8 as every `u64` of the format is, and `type` stays at 16. The asserts at `format.hpp:227-228` become 40 and the offsets above; the Python twin is `CAPABILITY = "<IIIIIIIIQ"` (`component_manifest.py:138`).
- `Expose` gains `subdir` and `rights`, and `flags` in place of `reserved`. It becomes the layout of `Offer` (56 bytes, `rights` at 48, `format.hpp:233-235`), so its Python twin becomes `OFFER`'s struct string (`:140`) in place of `EXPOSE` (`:141`). The asserts at `:236-237` become 56, with `target` still at 24. `RightsStated` is the flag offers and uses already use.
- `component::Capability` gains `std::string_view path` and `ustl::Option<Rights> rights`, and `component::Expose` gains `std::string_view subdir` and `ustl::Option<Rights> rights` (`decl.hpp:39`, `:73`; the views at `decl.cpp:310`, `:340`).
- The decoder bounds-checks the two new strings like the others, and puts the flags and rights of both records through the function a use already goes through: unknown flags, rights stated without the flag, and unknown right bits are refused (`decl.cpp:82`, `:120`, `:158`). The `reserved` checks of the two records go with the field. Version 2 is refused.
- The decoder does not check what the strings mean: that a directory declaration states a path and rights, that a protocol declaration states none, or that the path is absolute. That is the compiler's, as it is for a directory use (`realm.cpp:519-521`). A record that states no rights narrows nothing, and the root answers InvArgs for a declared path that is empty or does not start with `/`, as `serve_svc` does for a protocol use not under `/svc` (`realm.cpp:718-722`).
- `Use` and `Offer` do not change.

What changes in `route()`:

- **The expose is a hop.** In "from is the child", once the expose is found and its source is not the parent, it is applied: `hops.apply(expose->rights, expose->subdir)`. A failure is AccessDenied or NotFound at the child, verb Expose, with the capability's name there. The sentence at `route.hpp:259-260`, "An expose states no rights and no subdir, so it is no hop for the rules below", is rewritten to say that an expose may state both and is a hop like an offer. An expose that states neither narrows nothing, which is every protocol expose.
- **The declaration is a hop.** In "from is self", when the capability is a directory, `hops.apply(capability->rights, {})` is AccessDenied at the provider, verb Declare. A declared path, a `/` and the joined subdir together longer than `MAX_PATH_LENGTH` are NotFound at the provider, verb Declare, so the root never builds a path no Open could carry. The sentence at `:264-265`, "A declaration states no rights and no subdir", is rewritten. The type check stays protocol-only.
- **The hop list and the errors.** "Applying a hop" (`:267`) names the expose and the declaration beside the use, the offer and the built-in, with their instance and verb: the child and Expose, the provider and Declare. The error list (`:307-319`) gains "AccessDenied, Expose, at a child", "NotFound, Expose, at a child, when the joined subdir is too long" and "AccessDenied, Declare, at an instance". The rights rule is unchanged: stated rights only grow toward the source, so a use's r* may meet an offer's r*, an expose's r* and a declaration's rw*, and the error names the hop that holds less.
- **The answer.** `Route` gains `std::string_view path`, a view of the provider's manifest bytes: the declared path of a directory, empty for a protocol and for a built-in. The root builds the Open from it and the joined subdir.
- Nothing else changes for protocols.

### Where it lives in the provider
The provider's outgoing directory holds the directory at the path its declaration names: `export/data` for `/export/data`, beside the protocols' `svc/<name>`. No prefix is added, as Fuchsia's `source_path` is the place itself (`sdk/fidl/fuchsia.component.decl/capability.fidl:64-76`). A directory capability's name is therefore not in the program at all; the path is written in the manifest and written again in the program, and it is the one string the two share for a directory.

What `Outgoing` has to offer so that a program can put a tree there:

- `add_directory(path) -> NodeId` takes the absolute path as declared, makes the directory at its end and every missing directory on the way (`export`), and returns the last node. The program fills the tree under it with the `Tree` calls it already knows (`add_directory`, `add_file`, `add_service`, `tree.hpp:124-131`). That needs `tree()` to be reachable, or `Outgoing` to wrap the three adders. Refused: InvArgs for a path or segment that is not valid (`is_valid_path`, `tree.hpp`) and for `/svc` or anything under it, which would meet the protocols' service nodes; AlreadyExists when the path is equal to, above or below a directory already added; NoResource when the storage is full. The compiler refuses the same overlaps in a manifest, and `Outgoing` holds them again because a program can be wrong without one.
- The storage is bigger, by constants (decision 8 (b)). `Outgoing::Storage` holds `2 + MAX_PROTOCOLS` nodes (10) and `MAX_CONNECTIONS` connections (4) today (`outgoing.hpp:69`, `:76`, `:98`); the constants become 32 and 32, with a `MAX_NODES` beside them, of which `2 + MAX_PROTOCOLS` are the root, `svc/` and the protocol nodes and the rest, 22, are the directory capabilities' nodes: a node per segment of a declared path (a shared `export` once) and the whole tree under it. The connections are the root's own connection O plus every Open or Clone of a directory made through it. A directory provider spends one on each consumer's entry, one on every subdirectory or file a consumer opens, and one on each clone `fsio::export_namespace()` makes for a spawned child (`fsio.cpp:406`). The cost, from the sizes measured on 2026-10-30:

| Item | Today | At 32 and 32 | Cost |
|---|---|---|---|
| Connections | 4 | 32 | 28 more, each a `Connection` (40 B), a `Binding` (24 B) and a Loop slot in the program's table (24 B): 88 B, so 2,464 B |
| Nodes | 10 | 32 | 22 more at 72 B each: 1,584 B |
| `Outgoing::Storage` | 9,496 B | about 12.9 KB | +3,376 B; the program's Loop table +672 B; about 4 KiB together |

The storage is a `LazyInit` in `.bss` (`echo_server.cpp:279`), so it costs no image bytes. A program's Loop slot table follows `Outgoing::MAX_CONNECTIONS`: echo's is `MAX_CONNECTIONS + MAX_CLIENTS` (`echo_server.cpp:58`), so it grows from 8 to 36 slots with no change to echo's source, and its `static_assert` against the port's observer limit still holds. The `Connection` of 40 B is today's: the handler node of decision 7 adds a cookie, a serial, a pending flag and a held channel, which are not measured. The limit: the constants are global, so a filesystem that needs far more connections or nodes makes every provider pay for them until the constants are made parameters, which is compatible with the defaults.
- A Subtree node (step 6, decision 7) is added the same way, at a declared path: `add_subtree(path, handler)`, with a handler class like `ioservice::Handler`, and the same refusals.

### The Open on its way
**Events at the consumer's start** (`Realm::run`, `realm.cpp:497-544`; D8 (a) kept):

1. The root makes the channel pair (N, S) for the use.
2. `route()` walks the use. A route that fails logs one line and closes S with an epitaph, as now (`:515-517`, `:535`).
3. A route that ends at the built-in `boot` is served by `m_boot->serve()` as now.
4. A route that ends at a provider is written on the provider's outgoing client end `m_outgoing`, fire and forget (I1): `Open(path, flags, S)`. A new `forward_directory()` stands beside `forward()` (`:789-833`). The old branch, which answers `NotSupported` (`:518-523`), goes.
5. N is handed to the consumer as an NsDir handle named by the use's path, as now (`:537-543`).

What the Open carries:

| Field | Value | Why |
|---|---|---|
| path | the declared path without its leading `/` (`export/data`) and, when the joined subdir is not empty, `/<subdir>` | `Route::path` and `Route::subdir`. `route()` has checked that the two fit `ours_io::MAX_PATH_LENGTH` (1024); `encode_open` still refuses a longer path with InvArgs (`ours_io.hpp:293`), which `forward()` already turns into an epitaph on S (`realm.cpp:816-819`). A declared path that is empty or not absolute is InvArgs in the same way |
| flags | the use's rights as the `Perm*` bits, and `ProtocolDirectory` | `Flags` and `Operations` share their bit values (`ours_io.hpp:75`, `:127`); the protocol bit makes a file or service at the declared path answer NotDir |
| object | S | consumed by the write, whatever it answers |

**What the provider's server does** with that Open on O (`ioservice::Server::open`, `server.cpp:226-349`), in order: the path is valid; O has TRAVERSE; the requested rights are a subset of O's, which hold every right, so the only ceiling on the first Open is the route's own check; no creation flag; the walk from the outgoing root down the declared path and the subdir, `export/data/shared/public/docs` in the example below; NotFound if a segment is missing, which is also what a typo between the manifest's path and the program's path reads as; then the node's own rules. A directory node makes a connection C1 with exactly the requested rights; a file or service node answers NotDir; a Subtree node (step 6) hands the rest of the path away. Every later Open through C1 is checked against C1's rights (check 3, `:245`), so rights only shrink from the entry on. The provider trusts the root for the first hop, as any outgoing directory does: only the root holds the client end of O.

**What the consumer sees** when the provider is not in the state the root expects:

| Provider when the consumer starts | The root | The consumer's entry |
|---|---|---|
| Resolved, not started yet (a later sibling) | writes the Open; it queues on the server end that the provider takes at its start (`realm.cpp:364-387`) | works once the provider serves its DirectoryRequest; requests written on N before then queue behind it |
| Started | writes the Open | works |
| Exited, and the root knows (its Exited state is set, or its outgoing end closed) | `forward()`'s rule: NotFound epitaph on S (`:793-796`) | closed with NotFound; fsio reads it once and records it (`fsio.cpp:338-385`) |
| Failed to start (the launch itself failed) | the instance is Exited and its DirectoryRequest closed (`:407-426`); NotFound | the same |
| Exited, and the root does not know yet | the write answers PeerClosed, and the root logs "cannot forward" (`:825-832`) | PEER_CLOSED with no epitaph; fsio answers PeerClosed |
| Exits after the entry works | nothing: the root is not involved | N and every connection of C1 see PEER_CLOSED |

The fifth row is a real window. `start()` runs every eager child inside one callback (`realm.cpp:428-439`), and the root learns of an exit only when its Loop runs (`on_terminated`, `:845`). A provider that dies while its siblings are being started is invisible to the consumers started after it. It is the second of the design's worst interleavings (`docs/design/261004/component-framework.md:205-208`), moved to the start. It makes "a provider that exits before its consumer starts" unusable as a deterministic test; the tests use "declared but not served" and "exits after it works" (Testing).

Two bounds, both accepted with D8 (a). A provider sees an Open for every directory use that routes to it, used or not. The queue of its outgoing channel holds 64 messages (`kernel/object/include/ours/object/channel_dispatcher.hpp:28`), shared with the protocol Opens, so a 65th write fails and that entry reads PEER_CLOSED. The dev image has one. And the handles are unchanged: two per directory use, charged to the root (`pools.hpp:88-93`). A provider-sourced entry takes less than a `boot` entry, which holds a connection and a Loop slot of the root's (`boot_server.cpp:162-178`), because the root forwards S and keeps nothing.

**Invariants.**

- The root never waits for a provider. The forward is one write, and a failed write fails that entry alone (I1).
- Rights only shrink: the use's rights are at most every hop's, checked once by `route()`; from the entry on, every server checks the requested rights against the connection's before anything else.
- A path is split once per server, and no server sees a part another one consumed: the root's join is a prefix, each server consumes what it knows and passes the rest on unchanged. A valid path has no `..` (`tree.cpp:175`), so a part cannot climb out of the directory it was opened at.
- After the start, the root holds nothing per directory entry.

### One Open crossing mount points
A "mount point" here is any place where the server that answers an Open changes. There are three kinds, and the first is the only one a client sees.

| Where | Who splits the path | The part kept | The part passed on | Counterpart |
|---|---|---|---|---|
| The client's namespace | `fsio::Namespace::resolve` | the longest bound prefix, unique because bindings never overlap (`namespace.hpp:53-58`) | the rest, as the path of an Open written on the entry's channel | fdio's namespace (`sdk/lib/fdio/namespace/local-filesystem.cc:56`, `:241`) |
| The provider's outgoing directory | its `Server`, walking the declared path and the subdir | the segments up to the declared directory (`export`, `data`) | the joined subdir, which the route computed | `DirConnectorOutgoingRouter` joins `subdir` to `source_path` the same way (`src/sys/component_manager/src/model/component/instance.rs:1682`) |
| A Subtree node in a tree | the `Server` that reaches the node | the part up to the node | the rest, `.` when the Open names the node itself | `RemoteDir::open` forwards the rest and the flags (`remote.rs:108-123`) |

The example, with `store` as the future filesystem and the manifests above, then `fsio::open("/notes/a/b.txt", PermReadBytes, S2)` in `client`, where `a` is a Subtree node inside store's own tree (a second crossing):

Objects: client's entry N; the pair's server end S; store's O; store's Subtree node at its declared path `/export/data`; store's connection C1; a second server for `a`.

Events, in order:

1. At the start of `client`, the root makes (N, S) and routes the use. The hops, from the use toward the source: the use, r* and `docs`; the offer, r* (contains r*) and `public` in front, so `public/docs`; the expose, r* (contains r*) and `shared` in front, so `shared/public/docs`; the declaration, `rw*` (contains r*) and the path `/export/data`. The route is store, `data`, path `/export/data`, subdir `shared/public/docs`, rights r*.
2. The root writes `Open("export/data/shared/public/docs", r* | ProtocolDirectory, S)` on store's outgoing end and hands N to `client` as `/notes`. It keeps nothing.
3. In store, O's slot fires. `Server::open` passes its checks, walks `export`, reaches `data`, a Subtree node, and stops there. The rest is `shared/public/docs`.
4. Store's handler gets `(rest, flags, S)`. It resolves `shared/public/docs` in its tree, possibly after waiting on a disk, makes C1 on S at that directory with r*, or writes an epitaph on S.
5. `client` calls `fsio::open("/notes/a/b.txt", ...)`. `Namespace::resolve` answers (N, `a/b.txt`), and fsio writes `Open("a/b.txt", flags, S2)` on N. It may do so before step 4 finished: the message waits in S's channel.
6. C1 receives it and checks it against r*. Walking `a/b.txt`, it reaches `a`, a Subtree node of store's own tree. It passes `b.txt` and the flags to the code that owns `a`, which may be another filesystem on another channel.
7. That server makes the file connection on S2 with the requested rights, which are at most r*. Reads go from `client` to it directly.

At no point does a server see a path longer than it consumed, and at no point does the root or fsio walk across a mount point for the client (decision 6). The dev image's first provider has no Subtree node, so steps 3 and 6 are plain directory walks there.

### What a provider serves
`ioservice::Tree` holds three kinds of node in storage the program provides: Directory, File and Service (`tree.hpp:37-45`). Nodes are never removed (`tree.hpp:21`, `:196`), a File is a `VmoFile` with a VMO and the bytes in this process (`:49-65`), and `Server::dispatch` answers each message at once and returns an `Outcome` (`server.hpp:77-110`). A real filesystem needs more than that, and the seam has to leave it room:

| A filesystem needs | ioservice has | What the seam must give it |
|---|---|---|
| A name looked up by its own code, possibly after waiting on another server | a walk over nodes added by the program | the Open, with the rest of the path, handed to the program |
| A read of `(offset, count)` answered from its own state | `Server::read` copies from `VmoFile::bytes` | the program serves its own file connections |
| A directory listing from its own state, with a cursor | `Server::read_dirents` lists a node's children | the same |
| A request parked while a disk or another server answers | a synchronous `dispatch` | a program that owns the connection can park; the Open is one-way, so forwarding it needs no reply path |
| Nodes created and removed at run time | no removal, fixed storage | the same |
| Methods beyond the subset (attributes, writes) | none | the same; the ours.io codec grows with the filesystem |

The common answer is to hand the Open over, as a Service node already hands the channel of an Open over (`server.hpp:94-99`). The first option below does that for directories; the others put the filesystem's operations inside the core.

- **A. A Subtree node.** `NodeKind::Subtree` holds a value like a Service node. `Tree::resolve(directory, path)` stops at it and returns the node and the rest of the path. `Server::open` runs its checks 1 to 7 on the outer connection first, then answers a new `Action::Subtree` carrying the value, the rest, the flags and the channel, and allocates no connection. `LoopServer::Delegate` gets a second function for it, called inside the Binding's callback like `connect()`; the `rest` views the request buffer and is valid for the call only, so a handler that parks the Open copies it. The program serves the channel itself, with the ours.io codec that exists, or forwards it, which is what Fuchsia's remote node does. Cost: a filesystem repeats `Server::open`'s check order unless that order is made a function the program can call, which is a refactor for the filesystem task to ask for.
- **B. Per-operation handlers in the core, answered at once.** `Server` becomes generic over a node source with lookup, read and listing. Cost: a rewrite of the core and of its tests, and a filesystem that has to wait cannot be written.
- **C. B plus parked requests, the core buffering the later messages.** A `Pending` outcome, a per-connection "waiting" state, and a completion call; while a request is pending the connection's next message is read and held by the core. Cost: the whole of B, a state machine in the core, and the buffer: at least one message per connection, 4,096 bytes and 64 handle values, 4,352 bytes (`wire::MAX_MESSAGE_BYTES`, `MAX_MESSAGE_HANDLES`, `wire.hpp:75-76`), or 557,056 bytes for the root's 128 connections and about 68 KiB for a provider's 16.
- **C′. One handler interface that answers now or later, and a Loop that can leave a slot unarmed.** The maintainer's design of 2026-10-30, which replaces B and C. Every operation of the handler returns `Done(result)` or `Pending(ticket)`, a synchronous handler always returns `Done`, and the completion is `complete(ticket, result)`. The connection that took a pending request is not read until the answer is written: `loop::Action::Suspend` leaves its slot in use and unarmed, and `Loop::resume(Key)` arms it. The later messages stay in the kernel's channel (64 of them, `channel_dispatcher.hpp:28`), where they cost the server nothing. Cost: a Loop extension, tickets, and one request outstanding per connection. The next section is its design.

Decision 7 (2026-10-30) is A, then C′: A first, because it is needed anyway as the way from the outgoing directory to wherever a filesystem serves from and adds one node kind and one delegate function; C′ after it, for a filesystem that waits on a disk and should not repeat the core's checks.

### Waiting for a reply: the handler node, Suspend and resume (C′)
The Subtree node hands the whole channel to the program, and with it everything the core does: the Open checks, the rights of later calls, the cursors and the replies. C′ keeps those in the core and asks the program only for content, which may take time. It needs the Loop to leave a connection unread while it waits. Today `loop::Action` is `Rearm` or `Close` (`loop.hpp:36-42`), so a connection that took a request and answers `Rearm` is called with its next message at once, with the first one unfinished. Suspend is not a continuation: the callback that suspends returns, and what runs at the resume is a new packet and a new call, so the root's rule that the Loop has no continuations (I2) still holds.

**Objects.**

- **Handler node.** A node kind in a `Tree` (the name is a placeholder), at a declared path or below it. The walk stops at it as it stops at a Subtree node, and the rest of the path is the handler's. It holds a value that tells the handler which of its trees this is.
- **NodeHandler.** An abstract class the program implements (the name is a placeholder: `ioservice::Handler` is the protocol handler of `Outgoing` already), with a protected non-virtual destructor like `Delegate` (`loop_server.hpp:54`) and `Handler` (`outgoing.hpp:50`), whose operations are the table below.
- **Answer.** `Done(result)` or `Pending(ticket)`, where `Pending` hands back the ticket the operation was given (the table below writes it `Pending`). A result is a status and the operation's payload. A synchronous handler never answers `Pending`.
- **Cookie.** A `u64` the handler returns when it opens an object. The connection keeps it with the object's kind, directory or file; the core passes it back on every call and never reads it.
- **Ticket.** The core's token for one pending request: the `ConnectionId` (index and generation, `server.hpp:42`) and a serial the connection bumps for each pending request. The core issues it and passes it to each operation that can wait; `Pending` hands it back. The serial is what keeps a late second `complete()` of an earlier request from completing the next one.
- **Pending state.** A flag in the `Connection` with the ticket's serial and, for an Open, the request's server end S, which the core holds until the Open is answered.
- **Suspended slot.** A Loop slot in use whose wait is not armed.
- **`complete(ticket, result)`.** The core call that ends a pending request. Like `dispatch()`, it touches no channel: it checks the ticket, builds the reply in the shared `ReplyBuffer`, and returns an `Outcome` for the glue to write.

The operations of the handler, each given the ticket where it can wait:

| Operation | Called when | The handler gets | The handler answers |
|---|---|---|---|
| `open` | an Open walks to the node | the rest of the path, the flags | `Done(Opened{kind, cookie})`, `Done(status)` or `Pending` |
| `read` | a Read on a file connection that holds ReadBytes | the cookie, the connection's offset, the count (at most `MAX_TRANSFER_SIZE`) | `Done(bytes)`, `Done(status)` or `Pending` |
| `read_dirents` | a ReadDirents on a directory connection that holds Enumerate | the cookie, the cursor the core keeps for it, the byte limit | `Done(entries, next cursor)`, `Done(status)` or `Pending` |
| `get_backing_memory` | a GetBackingMemory the connection's rights allow | the cookie, the flags | `Done(vmo)`, `Done(status)` or `Pending` |
| `clone` | a Clone | the cookie | a new cookie, at once |
| `release` | the connection ends for any reason | the cookie | nothing |

`clone` and `release` cannot wait: a handler that must flush when a connection ends parks the flush in its own state. The core keeps the protocol: `Server::open`'s checks 1 to 7 run on the connection the Open came on before `open` is called, check 8 is the walk to the handler node, and check 9 is applied to the kind the handler reports (a file for a directory request answers NotDir, and the reverse NotFile); the rights of a Read or a ReadDirents are checked before the handler is asked; the seek offset, the dirent phase (the `.` entry first) and the encoding of every reply stay in `Server`.

**What the Loop gains.**

- `Action::Suspend`. After `on_signals()` returns it, `run()` marks the slot suspended and does nothing else: it arms no wait, closes no handle, bumps no generation. The slot stays in use, so `add()` never takes it, and its key stays valid.
- `Loop::resume(Key) -> Status`.
- A flag in `Slot`. The README counts 24 bytes: a handle (4), signals (4), a pointer (8) and a generation (4), with 4 bytes of padding that a flag fits in.
- Step 1 of `run()` also drops a packet that names a suspended slot. None should exist, since no wait is armed.
- The invariants: "one `on_signals()` per packet, then re-arm" gains "close or suspend", and "at most one live wait per slot in use" gains "and none for a suspended one". The handle is still closed only in step 4 (I3), and the destructor closes a suspended slot's handle like any other.

What `resume(key)` does, by the state of the slot the key names:

| The slot | `resume` answers | Effect |
|---|---|---|
| in use, suspended, generation current | Ok | the flag is cleared and the wait armed with the same key and signals; the packet reaches the Waiter like any other, behind the port's queue (I4); if the channel already holds a message or PEER_CLOSED, that packet is queued at once, since the wait is level-triggered |
| the same, and arming fails | the status of the arm | as step 4 does for a failed re-arm: the handle is closed, the generation bumped, the slot freed, and the Waiter's `on_rearm_failed(cause)` is called before `resume` returns |
| in use but not suspended: armed, its packet queued, or its own Waiter running | BadState | nothing changes; this is a second `resume`, and also a `resume` called inside the `on_signals()` that is about to answer `Suspend` |
| free, or a generation other than the key's, or an index past the table | NotFound | nothing changes; the key is stale, the slot was closed since it was handed out |

A second `resume` therefore answers BadState while the slot is still armed, and NotFound once its Waiter has answered `Close` and the slot has been freed. Both are harmless, and neither disturbs the slot's new life. A Waiter that answers `Suspend` and is never resumed keeps its slot and its handle for good.

**Events: a Read that waits on a block device.** `store` serves its tree through a handler node. `blk` is a block device that `store` reaches through its `/svc` (illustrative: the block protocol is not designed here). The client holds a file connection C1 on `a/b.txt` with r*.

Objects: the client's channel; the slot Sc with its `LoopServer::Binding` for C1; `store`'s handler H; the channel B to `blk` with its slot Sb, whose Waiter Wb belongs to H; the ticket T, C1 with serial 1.

1. The client writes `Read(100)` on its channel. Sc fires, and the Binding's `on_signals()` reads the message and calls `Server::dispatch(C1, ...)`.
2. `dispatch` finds a file connection with a cookie that holds ReadBytes, and calls `H.read(cookie, offset, 100, T)`.
3. H needs block 17. It writes a request on B, which does not wait (I1), notes T and what to do with the answer in a table of its own, and answers `Pending(T)`. The table can be indexed by `T`'s connection index: a connection has one request outstanding, so there are at most as many as connections.
4. `dispatch` sets C1 pending and returns the outcome `Pending`. The Binding answers `loop::Action::Suspend`.
5. `run()` marks Sc suspended, with nothing armed, and goes on to the next packet. Meanwhile a second `Read` from the client waits in the channel, and so would a Close, or the client going away.
6. `blk` answers on B. Sb fires, and Wb reads the answer, finds T's note, and calls `LoopServer::complete(T, bytes)`.
7. `Server::complete` checks that C1 is current, pending, and at serial 1; clears the flag; moves C1's offset by the bytes; builds the Read reply in the `ReplyBuffer`; and returns it as a `Reply` outcome.
8. The glue writes the reply on C1's channel, then calls `Loop::resume(Sc's key)`. The reply goes first, so that the next request is never started before the previous one is answered.
9. The slot is armed. The second `Read` makes it fire at once; its packet queues behind the others, and C1 serves it in turn. Wb answers Rearm for B as for any packet.

The glue needs Sc's key: `LoopServer::watch()` drops the key that `add()` returns today (`loop_server.cpp:64`), and the `Binding` keeps it from then on.

**When the client goes away while the request is pending.** A suspended slot is not armed, so the Loop does not see PEER_CLOSED and nobody is called.

1. The handler's work goes on to its end, as it must: it cannot tell, and there is no cancellation.
2. `complete()` builds the reply and the glue writes it. The write answers PeerClosed (`channel_dispatcher.hpp:59`). The glue tells the delegate with `write_failed()`, as `handle()` does for any failed reply (`loop_server.cpp:128-139`), but does not release the connection. Handles in a reply that failed were consumed with it.
3. The glue calls `resume()` all the same. The slot is armed, PEER_CLOSED is asserted already, and the packet comes at once, with Readable as well if the client wrote more before it went; those messages are read and their replies fail, as for any client that closes with messages queued. When PEER_CLOSED comes alone, `handle()` releases the connection (`loop_server.cpp:101-104`), which bumps its generation and calls `release(cookie)` on the handler, and answers `Close`; the Loop closes the handle and frees the slot.
4. Why `complete()` does not release the connection itself: a `Binding` is one per connection index, and a connection's Binding is registered again only after the Loop has freed its slot (`loop_server.hpp:84-90`). A release from `complete()` would free the index while the old slot is still in use and points at the same Binding. A new connection could take the index and the Binding, and the old slot's late packet would then be handled as the new connection's: its PEER_CLOSED would release a connection that is alive. So the rule stays: a connection is released only by its own callback, or by `on_rearm_failed` after the Loop freed the slot.
5. The ticket was spent at step 2. A second `complete()` for it finds the serial moved on, or the generation changed once the release is done, and answers NotFound without touching anything; the handler discards its result.
6. The one case no notification covers: the handler's own wait never ends, because `blk` died or never answers. Then the connection stays suspended with a client that has gone, and nobody can tell. The rule that closes it is the handler's: a handler that answers `Pending` must hold a wait in the Loop that ends in `complete()`, on the answer or on the failure of what it waits for, and a failure completes the ticket with an error status.

Compared with `on_rearm_failed`: that call exists because the Loop can free a slot behind its owner's back, and the owner holds state for the slot. Suspend frees nothing, so it has no such call; the slot is in use until the owner resumes it. The only way a suspended slot goes is a `resume` whose arm fails, and that uses `on_rearm_failed` unchanged: the Waiter is told after the slot is freed, and it releases the connection as it does for any failed re-arm. That call runs inside `resume`, which runs inside `complete()`, which runs inside the handler's own Waiter, so `release(cookie)` can reach the handler from within its own `complete()` call, after the reply was written. The handler must not use the cookie, or the ticket, once `complete()` has returned.

**Limits.**

- One request outstanding per connection, by construction: a connection with a pending request is not read, and the core also answers `Drop` to a `dispatch` for one, which only a glue bug could make. Later messages wait in the channel, in order, up to 64; the sender's 65th write answers NoResource (`channel_dispatcher.hpp:28`). There is no pipelining. For a Read it means two Reads on one connection run one after the other; two connections run side by side.
- A pending Open suspends the directory connection it came on, so the Opens behind it on that connection wait for the lookup. The new connection is made on S when the Open completes, and a refusal is written on S as an epitaph.
- The core allocates nothing per pending request: the ticket is the connection's own id, and the reply is built in the buffer that every callback shares, in the callback that completes.
- No timeout, since the Loop has no timers (`loop.hpp`'s opening comment: no tasks, no timers, no threads).

**The handler node and the Subtree node.**

| | Subtree node (A) | Handler node (C′) |
|---|---|---|
| Who serves the channel | the program, from the first message | the core, for the life of the connection |
| What the program is asked | the Open's rest of path, flags and channel; everything after | `open`, then `read`, `read_dirents` and `get_backing_memory` on a cookie |
| Open checks 1 to 7 (syntax, TRAVERSE, subset of rights, CONNECT, creation and cut flags) | run on the outer connection; the program repeats them for the rest | run once, by the core, before the handler is asked |
| Check 9, the node's own rules | the program's | the core's, on the kind the handler reports |
| Rights, seek and dirent cursors on later calls | the program's | the core's |
| Waiting on a disk | the program's own state machine | `Pending` and `Suspend` |
| A method the subset does not have yet | the program, with the codec | the core must learn it, and the handler gains an operation |
| What it costs | a filesystem repeats the checks | the interface, the Loop extension, and one request at a time per connection |

### The first provider: `examples/files`
A new example beside `examples/echo` and `examples/hello`, which this design does not touch (decision 10 (b)). The names are proposals: the directory `examples/files`, the program `files_server` (as `echo_server`, `examples/echo/BUILD.gn`), the component `files` in dev's `root.cm`. It uses nothing and provides directories only, so it has no `/svc`. It serves a small static tree with `Outgoing::add_directory`, each file a `VmoFile` whose bytes are constants of the program and whose VMO the program makes with `vmo_create` and `vmo_write` (the first open question).

What it serves under its declared path `/export/docs`:

| Path | Content | What it is for |
|---|---|---|
| `hello.txt` | one line | a read, and GetBackingMemory |
| `numbers.txt` | a few lines | a read that spans lines, and the order of a listing |
| `sub/nested.txt` | one line | a use's subdir, and an expose's subdir |
| `stop` | a service node | the exit case, if decision 11 (c) is taken |

What it declares and exposes, and what dev's `root.cm` offers to routing_checks from it. Two of the five uses work and three fail on purpose, and the build warns for the two that fail by rights (decision 9 (c)):

| Declared as | Exposed as | Offered as, used at | Expected |
|---|---|---|---|
| `docs`, `/export/docs`, `r*` | `docs`, `r*` | `files`, `r*`, used at `/files`, `r*` | works: read, list, rights shrink |
| | `docs`, `r*`, subdir `sub` | `files-sub`, `r*`, used at `/files-sub` | only `nested.txt` is there |
| | `docs`, `r*` | `files-denied`, `rx*`, used at `/files-denied`, `rx*` | AccessDenied at the expose: the offer asks more than it states |
| | `docs-wide`, `rx*` | `files-wide`, `rx*`, used at `/files-wide`, `rx*` | AccessDenied at the declaration: the expose states more than it allows |
| `ghost`, `/export/ghost`, `r*` | `ghost`, `r*` | `ghost`, `r*`, used at `/ghost` | declared and never served: NotFound from the provider |

That adds five directory uses to routing_checks' four, within the fifteen a component may have and the sixteen bindings of its namespace (`pools.hpp:67`, `fsio::MAX_BINDINGS`).

**How it exits** is decision 11, because the provider-exit case needs the provider to stop when the test says so. The options are in the table: a `Stop` protocol like echo's (a codec library and a second capability), an exit after a first successful read (only a handler node sees a read, so it waits for step 9), a service node in the directory (recommended), and an exit when the last consumer connection closes (a new hook, and timing the client does not control). With the recommended one, the provider adds a service node `stop` under `export/docs` and a handler for it that exits on `connect()`. `Outgoing` needs one more call for that, `add_service(path, handler)`, which registers the handler in the table that `add_protocol` fills (`outgoing.cpp:31`) and adds the node with `Tree::add_service`; the table's size, `MAX_PROTOCOLS`, is then a limit on both. The client connects to `/files/stop` with `PROTOCOL_SERVICE`; the connection's `r*` holds CONNECT, so a consumer that can read `docs` can also stop the provider. That is acceptable in an example and wrong in a service, which is one more reason the node is the example's own.

### Consequences for the other pieces
| Piece | Change |
|---|---|
| `fsio` | None. An NsDir is an NsDir. `mark_closed` and `export_namespace` already treat a closed entry (`namespace.hpp`, `fsio.hpp:100-115`). A cloned entry is a Clone on the provider's connection, so the provider's server must answer Clone and Close, which `Server` does (`server.cpp:497-519`). |
| Compiler | The three refusals go. A directory declaration takes `path` and `rights` (the key list at `:471` grows, and `path` and `rights` are refused on a protocol); the path rules and the non-overlap check join `_capabilities`; the keys of `expose` (`:660`) gain `directory`, `rights` and `subdir`, the last two refused on a protocol expose; the format twins change; the error texts that cite D3 change. The type check and `route_protocol_use` stay protocol-only; `check_rights()` and `route_directory_use` are new, with the warning (decision 9 (c)). `test_component_manifest.py` gains the cases below. |
| `component` | Format 3 (`Capability` 40 bytes, `Expose` 56), the decoder and its views, the expose and declaration hops in `route()`, `Route::path`, and the spec and error list. Every fixture manifest is rebuilt under the new version. |
| `ioservice` | `Outgoing::add_directory(path)` with its refusals, and sized storage; step 6 adds the Subtree node; steps 8 and 9 add the handler node, the `NodeHandler` interface, tickets, `Server::complete`, `LoopServer::complete`, the `Binding`'s key and `Outgoing::add_handler`. Its README and the comments of `server.hpp` and `loop_server.hpp` follow. |
| `loop` | `Action::Suspend`, `Loop::resume`, the `Slot` flag, the spec of `run()` (step 1, step 3 and the invariants), the `Waiter` comment, `README.md`, and the tests (step 7). |
| Root | `forward_directory()`, which builds the path from `Route::path` and `Route::subdir`, and a new comment at `realm.cpp:518`. `Realm::connect` is untouched: directories are never routed per Open. |
| Guide | `01-concepts.md:257`, the section from `:392`, and "The Loop and its slots" (from `:123`: the four steps of `run` and the four consequences, which gain `Suspend` and `resume`); `04-tree-servers.md:5` and `:34`, whose argument "why a tree server of your own cannot be reached" becomes "how it is reached"; `README.md:24`, `:39`, `:60`, `:119`; `06-checklist.md:74`; a section for the handler node once it exists. |
| Architecture and design notes | `glossary.md:9` (the capability line), `:49` (the Loop line) and `libraries.md:17`, `:18`; `component-framework.md` gets a revision note on D3, the non-goal at `:52`, `:116` ("the source is always the built-in `boot`"), `:301` and the departure table. |
| Change records | One per step that lands code, in `docs/changes/<YYMMDD>/`, by module: `component`, `component-manifest`, `ioservice`, `loop`, `init`, `routing-checks`, `examples-files`. The Loop's carries the new rules in full, as its spec does. |
| Tests and examples | A new example, `examples/files` (below), added as a child of dev's `root.cm` with its offers (`kernel/products/image/dev/BUILD.gn`); and routing_checks, whose manifest gains the directory uses and whose code gains the checks of the Testing section. Echo, `examples/hello` and the guide's chapters on protocol servers are not touched. |

### Staging
One step per commit; each builds, passes the host tests, and boots both images (`ours-verify`).

| Step | Does | Verified by |
|---|---|---|
| 1 | `component`: format 3, the decoder, the expose and declaration hops in `route()`, `Route::path`, and the spec. The compiler writes version 3 with the new fields empty. | Host tests that patch bytes, as `route_test.cpp` does today: a declaration narrower than the offer is AccessDenied, wider is routed, an expose is held against the hop before it and its subdir goes in front, the path comes back, a kind mismatch is NotFound. Images unchanged. |
| 2 | Compiler: directory capabilities with a path, directory exposes with rights and subdir, and offers from self or a child; the path rules; the build-time rights walk and its warning (decision 9 (c)); Python tests; fixtures with a directory provider. | `test_component_manifest.py`; `route_test.cpp` over compiled fixtures, replacing the patched test; `FixtureRoutesTest` still agrees, and now also on the rights answers. Images unchanged. |
| 3 | `Outgoing`: `add_directory(path)`, and the constants raised to 32 connections and 32 nodes (decision 8 (b)). Echo's source does not change; its Loop table follows the constant. | `outgoing_test.cpp` on the fake kernel: an Open of `export/data/x` makes a connection with the requested rights; a file reads; a service node at the declared path answers NotDir; the refused paths (below); the 32 connections run out with NoResource. |
| 4 | Root: `forward_directory()`; `examples/files` is added: it declares, exposes and serves directories, and is a child of dev's `root.cm`; routing_checks uses and reads them; the guide and notes of the table above that are about directories. | On the target: read, list, rights shrink, subdir, the widened offer and expose, the unserved path (Testing). |
| 5 | The provider-exit case, by the mechanism of decision 11 (recommended: the service node `stop`). | On the target: `Stop`, then PEER_CLOSED on a held connection and PeerClosed from `open()`. |
| 6 | Decision 7, A: the Subtree node, its delegate function, `Outgoing::add_subtree`. | Host tests with a handler that parks an Open and answers later; on the target the first provider serves its declared path through a Subtree handler that opens its own tree on S. |
| 7 | Decision 7, C′, the Loop: `Action::Suspend`, `Loop::resume`, the `Slot` flag, step 1's drop of a packet for a suspended slot, the spec, the README, the guide chapter. Useful without a filesystem. | Host, `loop_test.cpp` on the fake kernel: suspend, resume, a stale key, a second resume, a resume inside the Waiter that suspends, a peer closed while suspended, a resume whose arm fails (Testing). Target: the shell's `check_loop()` (`system/app/shell/shell_tests.cpp:7094`), on both images, gains a suspended slot that is not called while its channel holds a message, is called after `resume`, and answers BadState to a second `resume`. |
| 8 | Decision 7, C′, the core: the handler node kind, the `NodeHandler` interface with `Done` and `Pending`, the ticket and its serial, the pending state of a `Connection`, `Server::complete`. Images unchanged. | Host only, since the core touches no channel: a synchronous handler serves Open, Read, ReadDirents and GetBackingMemory with the core's checks run first; a handler that answers `Pending` and a `complete` give the same replies; a `dispatch` on a pending connection is `Drop`; a stale ticket, a duplicate of a spent one and a ticket of a released connection are refused and leak no handle (Testing). |
| 9 | Decision 7, C′, the glue and the first use: `LoopServer` answers `Suspend` for a `Pending` outcome, `complete()` writes the reply and resumes, the `Binding` keeps its key, `Outgoing::add_handler(path, handler)`; `files` serves a second declared directory through a handler node whose Read waits one Loop turn. | Host, `loop_server_test.cpp`: replies in order across a pending request; a client that closes while pending; a second request behind a pending one. Target: routing_checks reads the slow file, writes two Reads back to back, and closes with a Read outstanding, more times than the provider has connections (Testing). |

The delay on the target is of the provider's own making, so that `Pending` runs on real kernel objects without a disk: the handler makes a channel pair at start, writes one message on one end when it answers `Pending`, and a slot on the other end completes the ticket when it fires, a Loop turn later, with other slots served between.

Deliberately left out: remote nodes with a policy (non-goals), restart of a provider, writable trees and their consistency, anything about disks, executable files from a provider, a marker that silences the warning of a deliberate failure, pipelining and a timeout for a pending request, and the shared Open-check function that a Subtree node's users would want (the handler node does not need it).

## Alternatives considered
- **The filesystem as a built-in of the root, as `boot` is.** Rejected: the root runs on one thread and never waits (I1, I2), and a filesystem waits on a disk. It would also put a second server's state in the most privileged process.
- **A protocol that hands out a directory channel, which the client binds into its namespace by hand.** Rejected: nothing in a manifest says who may reach the filesystem, there is no rights ceiling to hold an offer against, and S5 chose routing for exactly this. It is decision 1 (c).
- **A fixed place, `dir/<name>`, for every directory capability (the recommendation for decision 2).** Not taken: the maintainer chose the declared path, Fuchsia's way, on 2026-10-30. It would have saved a path string per record, the overlap and `svc` rules in the compiler and in `Outgoing`, and a path the program must spell as the manifest does: the name would have been the one string both write, as it is for a protocol.
- **Two handler interfaces, one that answers at once and one that can wait (my options B and C).** Not taken: the maintainer's C′ has one, since a synchronous handler answers `Done` always.
- **The core buffering a pending connection's later messages (C).** Not taken: at least 4,352 bytes a connection, and the Loop's `Suspend` leaves the messages in the channel for nothing.
- **Keeping a PEER_CLOSED wait armed while a slot is suspended.** Not taken: a vanished client would be seen at once and its request cancelled, but a slot's signals are fixed for its life (`Slot::m_signals`), the Waiter would run while a request is pending, and the handler would need a `cancel(ticket)`. See the open questions.
- **A remote node in the library, owning a client end.** Rejected: it adds a channel per node and a policy for a dead remote that the program is better placed to choose; the Subtree node leaves both to the program.
- **Routing a directory entry on its first message (D8 (b)).** Kept as decision 5 (b). It costs a Loop slot per entry and a second path, and gains only the provider's state at the time of use.
- **Nested bindings in a client's namespace, or the root splitting paths across mounts.** Rejected, decision 6: `fsio` forbids overlapping bindings on purpose (`namespace.hpp:53-58`), and a root that resolves paths is the VFS process S1 turned down.
- **An expose that only passes the capability on, with narrowing left to the parent's offer (the recommendation for decision 4).** Not taken: the maintainer chose to let an expose state rights and a subdir, as Fuchsia's `ExposeDirectory` does (`sdk/fidl/fuchsia.component.decl/expose.fidl:130-136`), on 2026-10-30. It would have saved 16 bytes on every `Expose` record, the fields every protocol expose leaves empty, and a hop in the rules.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| 1. The boundary: do components provide directories | (a) yes, in the shape of protocols: declare, expose from self or a child, offer from parent, self or a child; (b) yes, but `offer ... from = self` stays refused for directories, which costs a special case that `route()` does not have; (c) no: keep D3 and hand directory channels out through a protocol, which costs routing, the rights ceiling and any manifest line saying who reaches the filesystem | (a) | Not asked | (a), 2026-10-30 |
| 2. Where a directory lives in the provider | (a) fixed `dir/<name>`, which costs the provider a layout it did not choose; (b) the path the declaration names, which costs a path string per record, overlap and `svc` rules in the compiler and in `Outgoing`, and a path the program must spell as the manifest does; (c) `<name>` in the outgoing root, which costs a collision with `svc` | (a) | Not asked | (b), 2026-10-30 |
| 3. Rights on the declaration | (a) required, and one more hop; (b) optional, absent meaning every right, which costs the ceiling; (c) none, the provider's server enforces, which costs the check for a widening offer | (a) | Not asked | (a), 2026-10-30 |
| 4. Narrowing at an expose | (a) none: an expose passes the capability on and the parent's offer narrows, which costs a provider the power to hide part of a directory from its parent; (b) rights and subdir at an expose as Fuchsia has them, which costs the Expose record 40 to 56 bytes, a hop in the rules, and fields every protocol expose leaves empty | (a) | Not asked | (b), 2026-10-30 |
| 5. When the root connects a directory use to a provider | (a) at the consumer's start (D8 (a)), which costs the provider an Open per use whether used or not and leaves the window of the fifth row; (b) on the entry's first message, which costs a Loop slot per entry and a second code path; (c) `boot` at start, a provider on first message, which costs both paths | (a) | Not asked | (a), 2026-10-30 |
| 6. How a path crosses mount points | (a) the client's namespace stays flat and non-overlapping, and a Subtree node inside a provider's tree is the only place a path crosses again; (b) nested bindings in the client's namespace, which costs shadowing rules and a client that splits paths; (c) the root resolves paths for clients, which costs the VFS of S1 on a single thread | (a) | Not asked | (a), 2026-10-30 |
| 7. The seam a filesystem plugs into | (A) a Subtree node that hands the Open and the rest of the path to the program, which costs a filesystem the Open checks; (B) per-operation handlers in the core answered at once, which costs the core's rewrite and cannot wait on a disk; (C) B plus parked requests, the core holding a pending connection's later messages, which costs at least 4,352 bytes a connection and a state machine drawn before a filesystem exists; (C′) one handler interface whose operations return `Done` or `Pending`, with `Action::Suspend` and `Loop::resume` so that a connection waiting for its answer is not read, which costs a Loop extension, tickets, and one request outstanding per connection | A as step 6, then C′ as steps 7 to 9 | Not asked | A, then C′, 2026-10-30 |
| 8. How a provider sizes its outgoing directory | (a) `Outgoing::Storage` takes the node and connection counts as template parameters, which costs every user two numbers (the nodes follow the declared paths and the trees under them); (b) bigger constants with defaults, 32 connections and 32 nodes where there are 4 and 10, which costs 88 B for each added connection and 72 B for each added node (about 4 KiB for 32 and 32, in `.bss`) and makes every provider pay for what the largest needs; (c) the program passes spans, as `LoopServer::Memory` does, which costs four arrays where there is one struct | (a) | Not asked | (b), 2026-10-30 |
| 9. A build-time check of directory rights | (a) none: a widened offer or expose is found when the consumer starts, by a log line and an AccessDenied epitaph; (b) a whole-tree walk that fails the build, which costs a per-use exemption for `/denied` and the negative fixtures; (c) a walk that only warns, which costs a second place a message can hide, and a warning on every compile of `root.cm` for the deliberate failures | (a) | Not asked | (c), 2026-10-30 |
| 10. The first provider on the target | (a) echo gains a directory: it has `Stop` already, which the exit case needs; the cost is the guide's model of a protocol server and its line citations; (b) a new example component, `examples/files`, which costs a component, its manifest and a way to exit | (a) | Not asked | (b), 2026-10-30 |
| 11. How the first provider exits, so that the provider-exit case can be tested | (a) a `Stop` protocol like echo's, which costs a codec library (`examples/files/protocol`), a protocol capability with its expose, offer and use, and a client table in the provider; (b) the provider exits by itself after a first successful read, which costs the step order (only a handler node sees a read, step 9) and a provider that dies on a read; (c) the client asks through the directory: a service node `stop` under the declared path, which the provider answers by exiting, which costs one `Outgoing` call to register a handler for a service node and a directory with a service in it; (d) the provider exits when its last consumer connection closes, which costs a hook for a connection that ended and a test whose timing the client does not control | (c) | Not asked | (c), 2026-10-30, taken by the implementer: it is a hook of the example's test, not a choice of the design |

Reasons for the recommendations.

- **1 (a).** `route()` treats the two kinds alike already, and every refusal in the compiler is an extra rule. (b) adds a rule to remove nothing at run time. (c) is the alternative in which a filesystem is reachable but not governed.
- **2 (a).** It is D3's rule for protocols, and there is already one silent failure from a name written twice (`docs/repairs/261029/component-manifest.md`, item 1); (b) makes the program spell a path the manifest also spells. Fuchsia requires the path (`sdk/fidl/fuchsia.component.decl/capability.fidl:64-76`), so (a) departed from it. The maintainer chose (b) on 2026-10-30, over this recommendation; it costs what the options row says: a path in every directory declaration, two sets of path rules, and a typo between manifest and program that reads as a NotFound the provider's server writes on the consumer's entry, with no line in any log.
- **3 (a).** The walk's rule is that stated rights only grow toward the source, so a declaration that states none is an unbounded top. Fuchsia refuses a directory declaration without rights (`src/sys/lib/cm_fidl_validator/src/lib.rs:1379-1406`) and holds the request against them (`src/sys/lib/routing/src/rights.rs:22-33`, `:108`; `instance.rs:1665`).
- **4 (a).** The parent started the child and may narrow in its own offer, so what an expose could narrow is reachable in the next hop. (b) adds fields to every Expose record for a power that only matters between a provider and a parent it does not trust. The maintainer chose (b) on 2026-10-30, over this recommendation; it costs the `Expose` record 16 bytes (40 to 56), fields every protocol expose leaves empty, changes in three places of `route()`'s spec (the walk, the hop list and the error list), and a hop that the build-time rights walk of decision 9 follows. It also gives a provider one thing the other option did not: two exposes of one declaration, with different rights or subdirs, which the non-overlap rule would otherwise forbid declaring twice.
- **5 (a).** The maintainer chose it for `boot` (D8) and named this cost then (`component-framework.md:377`). The window is real but a protocol has it too: an Open lost to a dying provider is the second worst interleaving of the design. (b) is Fuchsia's way (`serve_processargs/src/namespace.rs:145-175`) and the answer to revisit if the 64-message bound or the window ever matters.
- **6 (a).** It keeps the root off the data path and the client's table trivially correct (longest prefix is unique). The cost is that two filesystems cannot be unioned by the client; a provider that wants that composes in its own tree. Linux has no counterpart worth following: its mounts live in the kernel's table, which S1 rejected.
- **7 (A, then C′).** The options differ in what the core must know about a filesystem, and none is known. A commits to the least: the Open and the rest of the path leave the core, and it is needed whatever follows, as the way from the outgoing directory to wherever a filesystem serves from. C′ is the maintainer's design of 2026-10-30, which replaces my B and C. B cannot wait on a disk. C can park a request, but a connection that took one would have its next message processed while the first is unfinished unless the core holds that message, at least 4,352 bytes for each of its connections. C′ leaves the message in the channel by not arming the slot; the Loop could not do that, since `Action` was `Rearm` or `Close`, so the cost moves there: `Suspend`, `resume`, and one request outstanding per connection. It also gives one handler interface instead of a synchronous one and an asynchronous one. Fuchsia's vfs has both a remote node (`remote.rs`) and a node trait, with `Vnode` in C++ (`src/storage/lib/vfs/cpp/vnode.h:82`) and an async `Directory::read_dirents` in Rust (`rust/src/directory/entry_container.rs:102-107`); A is the first, and C′ is the second with `Done` and `Pending` written by hand where Rust has `Poll::Ready` and `Poll::Pending`.
- **8 (a).** It is the style the sibling pools already use, with the sizes in the program that knows them (D27). The numbers matter: one entry per consumer, one connection per open subdirectory or file, one per exported clone, and a node per segment of each declared path. The maintainer chose (b) on 2026-10-30, over this recommendation: the constants go to 32 and 32 and keep their place in `Outgoing`, which costs 88 B for each connection added (a `Connection` of 40 B, a `Binding` of 24 B, a Loop slot of 24 B in the program's table) and 72 B for each node (a `Node`), measured on 2026-10-30; about 4 KiB for 32 and 32, in `.bss` (the storage is a `LazyInit`, which costs no image bytes), and echo's Loop table follows the constant. The limit is that the constants are global: a filesystem that needs far more makes every provider pay for it until the constants become parameters, which can be done later and stays compatible with the defaults.
- **9 (a).** The type check landed because a mismatch had no signal (`docs/design/261029/protocol-identity.md:9-19`). A rights failure has one already, and the build would need an exemption for the deliberate `/denied`. The maintainer chose (c) on 2026-10-30, over this recommendation: the build walks the tree for rights and prints a `warning:` for each route that holds less, without failing. It costs a second place to read a message, since ninja shows an action's output only when the action runs; three warnings on every compile of dev's `root.cm` for the deliberate failures until a marker exists; and a second walk in Python to keep equal to `route()`, which `FixtureRoutesTest` guards. The run-time line stays the main signal.
- **10 (a).** Smallest change that gives both a positive and an exit case; the cost is paid by the guide, whose citations of echo move. The maintainer chose (b) on 2026-10-30, over this recommendation, so that echo, `examples/hello` and the guide's chapters on protocol servers stay as they are. It costs a new component with a manifest and a program, and the question of how it exits, which is decision 11.
- **11 (c).** It needs no codec library and no second capability, works at step 5 before the handler node exists, and the client chooses the moment, which makes the case deterministic. The client connects to `/files/stop`, a service node, which the connection's `r*` allows (`CONNECT` is in it), and the provider exits when it takes the channel. That also runs a service node reached through a provider's directory, which nothing else does. (a) is the pattern the repository knows, and doubles the example's surface. (b) ties the exit to step 9 and models a provider that dies on a read. (d) needs a new hook and leaves the timing to the order in which the client lets go.

Decisions 2 and 4 follow Fuchsia, as the maintainer chose, and no longer depart from it; 5 (a) keeps D8's departure (the root connects at the start, Fuchsia on the first message) and is still the maintainer's to make.

## Testing and verification
**Host tests.**

- `component`: the decoder reads format 3; a capability's path and rights and an expose's subdir and rights are bounds-checked; rights stated without the flag or with an unknown bit are refused on both records; version 2 is refused. `route()`:
  - a directory use ends at a provider with the use's rights, the joined subdir and the declared path; a protocol route returns an empty path;
  - a declaration narrower than the offer is AccessDenied at the provider, verb Declare; wider is routed;
  - an expose is a hop: an expose wider than the declaration is AccessDenied at the provider, verb Declare; an expose that narrows, then an offer or a use that asks for more, is AccessDenied at the child, verb Expose; an expose's subdir goes in front of the offer's and the use's;
  - rights are held against the hop before, not the use; an expose that states none narrows nothing;
  - the declared path with the joined subdir past 1024 bytes is NotFound at the provider, verb Declare, and an expose's subdir too long is NotFound at the child, verb Expose;
  - a directory use never reaches a protocol capability and the reverse is NotFound; a provider with no program is NotFound; BadType stays protocol-only.
- The compiler (`system/test_component_manifest.py`):
  - a directory capability with a path and rights compiles and the Reader sees the 40-byte record; a directory expose with rights and a subdir compiles and the Reader sees the 56-byte record;
  - refused, with a message that names the key: no `path`, no `rights`, `subdir` or `type` on a declaration; `path` or `rights` on a protocol declaration; a relative path, `/`, a trailing `/`, an empty, `.` or `..` segment; `/svc` and `/svc/x` (while `/svcx` is accepted); two declarations at one path, and one under the other (while `/export/data` and `/export/data2` are accepted);
  - `rights` and `subdir` on a protocol expose are refused, and `type` on any expose; a directory expose from self or a child compiles; one that is not declared, or declared as a protocol, is refused;
  - a directory offer from self and from a child compiles, and the cycle check covers it; the whole-tree type check still skips directories; `FixtureRoutesTest` agrees with the C++ walk on the new fixtures.
  - the rights walk (decision 9 (c)): a use that asks for more than the offer states warns, with the exact text of the warning above; so do an offer wider than the expose behind it, an expose wider than the declaration, and a use wider than `boot`'s `rx*` built-in; a chain that only narrows, a route that does not resolve, and a protocol use warn nothing; the exit status is 0 and the `.cm` is written; `--no-type-check` turns the walk off; `FixtureRoutesTest` compares the walk's AccessDenied answers with `route_test.cpp`'s on the fixtures that narrow on purpose.
- `ioservice` on the fake kernel: `add_directory("/export/data")` makes `export` and `data`, and `/export/a` with `/export/b` share `export`; it refuses `/svc`, `/svc/x`, an invalid segment, a path equal to, above or below one already added; `export/data/x` serves; an Open with rights beyond the connection's is AccessDenied; a file under it reads; a service or file at the declared path with `ProtocolDirectory` answers NotDir; the connection table running out answers NoResource on the Open's channel. Step 6: a Subtree node at a declared path stops the walk, the rest is `.` for the node itself, the flags arrive unchanged, the outer checks run before the handler, and a handler that parks an Open and answers later works.
- `loop` on the fake kernel (step 7):
  - a Waiter that answers `Suspend` leaves its slot in use with no wait armed, which the fake kernel can count, and with its handle open; a message that arrives is not delivered;
  - `resume` arms it, and the waiting message is delivered, behind the packets already queued;
  - `resume` answers BadState for a slot whose Waiter is running, for an armed slot and for a second `resume`, and NotFound for a freed slot, for the old key of a reused slot and for an index past the table; none of them changes the slot;
  - a peer that closes while the slot is suspended is seen after the `resume`, as PEER_CLOSED alone, and the Waiter answers `Close`;
  - a `resume` whose arm the fake kernel refuses closes the handle, frees the slot, bumps its generation, calls `on_rearm_failed(cause)` once before `resume` returns, and answers the cause;
  - `add()` never takes a suspended slot; the destructor closes its handle; a packet naming a suspended slot is dropped; Waiters that answer only `Rearm` and `Close` behave as before.
- `ioservice` core (step 8): the checks 1 to 7 run before `open`; the kind a handler reports meets the flags (NotDir, NotFile); a Read without ReadBytes is refused before the handler is asked; the offset the handler gets is the connection's, and moves by the bytes of a completed Read; a `dispatch` on a pending connection is `Drop` and releases it; `complete` with a stale generation, with an earlier serial after a later request began, and twice for one ticket are refused without touching the connection; a result with a VMO handle that is refused leaves the handle to the caller; `release(cookie)` is called once for every end of a connection, pending or not; a synchronous handler gives the same replies as the Tree-backed nodes for the same content.
- `LoopServer` (step 9): replies come in the order the requests were sent across a pending one; a client that closes while pending gets no reply, the write fails and `write_failed()` is told, `complete()` leaves the connection alone, the connection is released by its own callback after the `resume` and the slot closed, and `release(cookie)` is called once; no new connection takes the index before then; a second request written behind a pending one is served after it; a pending Open suspends its directory connection and the new connection is made on S at completion.
- The root has no host test (`system/service/init/test/` holds `autorun_test.cpp` only), so `forward_directory()` is checked on the target.

**On the target** (`ours-verify`, both images; `boot_check.py` waits for `routing_checks: passed`, `boot_check.py:93`). routing_checks gets a few more entries, in the way of `check_denied` (`routing_checks.cpp:317`) and `check_echo` (`:793`). The compile-time negative cases (overlapping declared paths, a path under `svc`) cannot be on the target, since the build refuses the manifest; they are the compiler's tests above.

| Case | Expected |
|---|---|
| Read a file and list the directory through the entry | the bytes; the names in order |
| Open for write, or for more rights than the use has | AccessDenied from the provider's connection |
| A use with a `subdir` | the subdirectory's files only |
| An offer wider than the expose behind it (root.cm offers `rx*` where `files` exposes `r*`) | the entry arrives closed; every `open()` answers AccessDenied (read once from the epitaph, then recorded) |
| An expose wider than the declaration (`files` exposes the declaration again under another name with `rx*` over a declared `r*`) | the same |
| A use whose route passes an expose with a subdir | the files of that subdirectory only |
| A path declared in the manifest but not served by the program | NotFound, written by the provider's server on S |
| A capability nobody offers | NotFound, as `ours.test.Unoffered` is today |
| The provider asked to stop, after the entry worked (connecting to the service node `stop`, decision 11 (c)) | PEER_CLOSED on a held connection, then PeerClosed from `open()`; waited for as `check_echo` does (`routing_checks.cpp:793`); the root logs `init: files exited with 0` |
| A Read on the slow file, which the handler answers a Loop turn later (step 9) | the bytes; a Read on another connection of the provider is answered meanwhile |
| Two Reads written back to back on one connection (step 9) | two replies, in order |
| A Read written and the channel closed at once, repeated more times than the provider has connections (step 9) | the provider keeps answering: no NoResource from connections that leaked |

**Not verifiable yet.** A provider that dies in the window of the fifth row (the root cannot be made to lose that race on purpose); a provider restarted; the 64-message queue overflowing; a filesystem on a disk; a file's VMO from a provider; an executable from a provider.

## Open questions
- Whether a `VmoFile` for a provider's static file needs a real VMO. `Tree` documents one with Duplicate (`tree.hpp:49-65`), and `Server::get_backing_memory` duplicates it without checking (`server.cpp:456`). What the vDSO answers for an invalid handle was not established; the first provider can make one with `vmo_create` and `vmo_write` (both exist, `syscalls.gni:881`, `:1156`).
- The names of the node kinds and of the interface: "Subtree", "handler node" and `NodeHandler` are the document's placeholders; `docs/changes/261008/ioservice.md` renamed the library because a name suggested a mounting layer, and "mount" was left out on purpose.
- Whether 32 connections and 32 nodes are enough for the first filesystem, and whether the 64-message queue of an outgoing directory needs a build-time bound on the directory uses that can route to one provider. Neither is measured. Making the constants parameters later is compatible with the defaults.
- Whether the shared Open-check function (the cost of a Subtree node) is asked for now or by the filesystem task. The handler node does not need it.
- The pager for a file's VMO (`.agents/tasks/page-source`) and a VMEX holder for executables are what a filesystem needs next; neither is a built-in, and this design does not choose them.
- `docs/repairs/261029/component-manifest.md` item 1 (a name declared in the manifest and not served by the program) applies at once to the declared path of a directory, which is written in both places. Decision 9 is the build-time half of rights only; whether a path and a protocol name could be written once, for example in a header the program and a generated manifest fragment share, is not decided.
- Whether the decoder should refuse a directory declaration whose path is empty or not absolute, as it refuses a stray flag. This document keeps the decoder structural (`component-framework.md:284`) and puts the refusal in the root (InvArgs) and the compiler.
- The per-connection limit. One request outstanding is by construction; pipelining would need a queue of tickets per connection and an order for their replies, and a pending Open suspends its whole directory connection. Whether a filesystem's workload wants more is not known.
- The ticket's lifetime and who issues it. This document has the core issue it, as the connection's id and a serial, so that the handler's table can be indexed by connection and a stale completion is caught by a generation. A handler-chosen ticket would be simpler to read and would need a search, and the stale check would be the handler's.
- Whether `Suspend` needs a timeout. The Loop has no timers, and a handler whose wait never ends holds a connection and a slot for good; the rule that it completes on the failure of what it waits for is the only bound.
- Cancellation. A client that goes away while its request is pending is seen only after the answer is written, so the work runs to its end. A PEER_CLOSED-only wait while suspended, with a `cancel(ticket)` on the handler, would cancel it, at the cost of a second set of signals per slot.
- Whether the `Slot` flag fits in the padding. By the field sizes it does (24 bytes stay 24), but no `sizeof` is asserted today.
- Whether `clone` and `release` may stay unable to wait, or a filesystem needs a flush on Close that answers late.
- A marker that says a failure is deliberate and silences its build warning (decision 9 (c)): where it goes (the use, the offer, the expose), what it is called, and whether the root should log a deliberate failure differently. Until then `root.cm` warns three times on every compile for routing_checks' negative entries.
- Whether the existing switch `type_check = false` should keep covering both walks, as this document has it, or whether the rights walk gets a switch of its own.
- Decision 11 (c) shares the table of `Outgoing`'s handlers between protocols and service nodes, so `MAX_PROTOCOLS` limits both, and lets any consumer of `docs` stop the provider.
