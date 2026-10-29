# Component framework glossary

The words used when talking about how components declare, find and reach each other's capabilities, one line each, grouped by where the thing lives. The running example is the dev image, where routing_checks connects to the protocol echo serves. The decisions behind these terms are in [component-framework.md](../../design/261004/component-framework.md).

## Declared in a manifest

- **component**: a program together with its manifest; echo, routing_checks and shell are components.
- **manifest (`.cm`)**: the declaration file `component_manifest()` compiles at build time, naming the program, children, uses, offers, exposes and capabilities. It lands in the BootFS under `meta/`, and its URL has the form `fuchsia-boot:///#meta/echo.cm`.
- **capability**: anything that can be routed. ours has two kinds, protocols and directories; today the only directory is the root's built-in `boot`, so components provide protocols only.
- **protocol**: a message format spoken over one channel, such as `ours.examples.echo.Echo`.
- **protocol type**: the string that says which protocol a capability, a use or a root built-in speaks, written `type` in a manifest and equal to the entry's own name when left out. It exists because an `offer` may rename with `as`, after which the name no longer says what the protocol is. A route must end at a capability of the type its use states; the build checks it over a manifest's whole tree and `route()` checks it on every Open, answering `BadType`.
- **use**: "I need this", written in the component's own manifest and always from its parent. A protocol use always lands at `/svc/<name>`.
- **offer**: "the parent gives this to a child", written in the parent's manifest. Its source is the parent's own parent, the parent itself or another of its children; it may rename with `as`, and for a directory narrow the rights or add a subdir.
- **expose**: "the child hands this up to its parent", written in the child's manifest, from itself or one of its own children.
- **capabilities (declaration)**: "I serve this myself"; where a route that goes down into a component ends.
- **built-in**: a capability the root serves itself: the `boot` directory, `ours.process.Launcher` and the `ours.resource.*` protocols. It is what `from: "parent"` means at the root.

## In the root

- **root**: init on the dev image, the owner of the component tree and the only place routing happens.
- **Realm**: the root's object for the whole tree ([realm.hpp](../../../system/service/init/realm.hpp)): the instance pool, the resolver that reads manifests, the ELF runner that starts processes, the supervision of their exits, and every component's `/svc`. It runs on one thread, so nothing in it is locked. In general a realm is a component together with all its descendants; ours has one Realm, the tree from the root down.
- **instance**: one instantiation of a component, the root's record of it. Its states are Unresolved, Resolved, Started and Exited. Instances live in a fixed pool and are never destroyed.
- **moniker**: an instance's path in the tree, such as `echo`, with `.` for the root. It is used only in logs and errors.
- **resolve**: read and decode an instance's manifest, create its children Unresolved, and make its outgoing channel pair.
- **start**: pack an instance's startup handles and call `launch::start` to create its process.
- **route**: `component::route()` ([route.hpp](../../../system/lib/component/include/component/route.hpp)), the walk from one use up through the parents' offers and down through the children's exposes to the capability's source, checking rights at every hop.

## In a component's process

- **processargs**: the first message a new process reads, sent by whoever started it: argv, a table of names, and typed handles.
- **namespace**: the process's own table from a path prefix to the client end of a directory channel; `/boot` and `/svc` are entries in it. fsio fills it from the startup handles before main. There is no global mount table.
- **NsDir**: the startup-handle type of a namespace entry. The n-th NsDir handle belongs to the n-th name in the processargs name table.
- **`/svc`**: the namespace entry the root serves for each component, with one service node per protocol the component uses. Its contents differ per component: it is that component's view, not a registry.
- **outgoing directory**: the directory a component serves for others, whose client end only the root holds; the component's protocols sit under its `svc/`. The root keeps the client end as `m_outgoing` and forwards routed Opens into it.
- **DirectoryRequest**: the startup-handle type that carries the outgoing directory's server end to the component.

Two things are called svc: a component's `/svc` namespace entry, which holds what it uses, and the `svc/` subdirectory of its outgoing directory, which holds what it serves. Outgoing serves the second.

## Libraries and wire formats

Which library sits at which layer, and which programs link which, is in [libraries.md](libraries.md).

- **ours.io**: the message format of files and directories, with `Open`, `Read`, `ReadDirents`, `GetBackingMemory` and the rest ([system/lib/ours.io](../../../system/lib/ours.io)). It is a codec only and decides nothing about who may reach what.
- **ioservice**: the server library that answers ours.io for a tree of nodes held in memory ([system/lib/ioservice](../../../system/lib/ioservice)). A node is a Directory, a File or a Service. The root's `/boot` and `/svc` and echo's outgoing directory are all ioservice trees. It is not a mounting layer: a client puts a server's tree at a path in its own namespace.
- **service node**: an ioservice node whose Open hands the request's channel to the program serving the tree, together with the value the node was added with. From then on that channel carries the protocol's own messages, not ours.io.
- **fsio**: the client library, holding the namespace and providing `open()` and `connect()`.
- **Outgoing**: `ioservice::Outgoing` (`//system/lib/ioservice:outgoing`), which a component uses to put its protocols under the `svc/` of its outgoing directory. It was the `svc` library until 2026-10-28.
- **LoopServer**: `ioservice::LoopServer` (`//system/lib/ioservice:loop_server`), an ioservice tree served on a Loop: one slot per connection, with a `Delegate` that says what an Open of a service node comes to.
- **Loop**: the single-threaded event loop, a port and a set of callbacks; the root and echo both run on one.
- **epitaph**: the last message written on a channel before it is closed, saying why, such as NotFound or AccessDenied.

## How they fit together

1. At build time each manifest is compiled into `meta/<name>.cm`.
2. The Realm resolves the root from `root.cm` and creates its children as instances.
3. Starting an instance, the root puts its NsDir handles (`/boot`, `/svc` and the rest) and its DirectoryRequest into the new process's processargs.
4. Before main, fsio binds the NsDir handles into the process's namespace.
5. The process calls `fsio::connect("/svc/X")`. The root routes the use behind that node and forwards the Open to `svc/Y` in the provider's outgoing directory, where the provider's handler takes the channel.
