# Component framework libraries

Which library under `system/lib` does what, by layer, and which programs link which layers. Read it with the [glossary](glossary.md), which explains the words, and the [design](../../design/261004/component-framework.md), which explains the decisions.

## Layers

Dependencies run downward, with one exception listed at the end. A library may also use another of its own layer: `ours.process` uses `ours.io`.

| Layer | Library | What it is | Depends on |
|---|---|---|---|
| Framing | `wire` | The message header, transaction ids, method ordinals and epitaphs every protocol shares | `ustl` |
| Protocol codecs | `ours.io` | Open, Close, Clone, Read, GetBackingMemory and ReadDirents for files and directories | `wire` |
| | `ours.process` | The Launcher protocol and the processargs layout | `ours.io`, `wire` |
| | `ours.resource` | The protocols that hand out kernel resources; its `:client` target adds the client call | `wire`; `:client` also `fsio` |
| | `examples/echo/protocol` | The Echo example's protocol, a model for a new one | `wire` |
| Client | `fsio` | The process's namespace, `open()`, `connect()` and the file calls on top of them | `ort`, `ours.io`, `ours.process` |
| Server | `ioservice` | Answers ours.io for a tree held in memory: the connection table and the checks, with no channel of its own. Its `:loop_server` target serves it on a `loop` (`LoopServer`), and its `:outgoing` target is a provider's outgoing directory on that, with a handler per protocol (`Outgoing`) | `ours.io`, `wire`; `:loop_server` and `:outgoing` also `loop` |
| | `loop` | The single-threaded event loop on a port | `ustl` |
| Component manager | `component` | The manifest format, its decoder, and the router | `ustl` |
| | `bootfs` | Reads the flat BootFS image | `ustl` |
| | `launch` | Builds a process from an ELF file and starts it | `bootfs`, `eftl`, `elfload`, `ours.process` |

## Who links what

A program links the layers it plays a part in, so the split follows what each program needs.

| Program | Plays | Links |
|---|---|---|
| `examples/echo` | A provider | `ioservice:outgoing`, `loop`, the Echo codec, and through `ioservice` `ours.io` and `wire` |
| `system/driver/pc-ps2` | A client | `fsio`, `ours.resource:client`, `wire` |
| `system/service/init/test/routing_checks` | A client | `fsio`, `ours.io`, `ours.process`, `ours.resource:client`, `wire` |
| `system/app/shell` | A client that launches programs | the client's set, plus `launch`, `loop`, `bootfs` |
| `system/service/init` | The root: server, router and launcher | `ioservice`, `loop`, `component`, `bootfs`, `launch`, `ours.io`, `ours.process`, `ours.resource`, `wire` |

A provider such as echo needs none of `fsio`, `component` or `launch`, which is a large part of the framework's code; a client needs none of the server layer.

## Why it is split this way

- **One codec library per protocol.** The codecs are written by hand today, and the design plans a generator for them (S6, G3 and G4). The generator will emit one library per protocol, as Fuchsia emits one per FIDL library, so a merged codec library would be split again then.
- **The server core touches no channel.** `ioservice` takes a message and returns an `Outcome`, so its whole test suite runs on the host. The glue that reads and writes channels is `ioservice::LoopServer`, in a target of its own for that reason; the root's /boot and /svc and a provider's outgoing directory are both served by it.
- **Client and server are separate libraries.** `fsio` is what a program calls to reach others; `ioservice` is what a program runs to be reached.

## Where it is not converged yet

- `ours.resource` holds a codec and, in `:client`, a client call on `fsio`, so a codec library depends upward on the client layer; the other codec libraries hold codec alone.

## Reading order

`wire`, then `ours.io`, then `ioservice` with its `:loop_server` and `:outgoing`, then `fsio`, then `component`: each step adds one idea to the last.
