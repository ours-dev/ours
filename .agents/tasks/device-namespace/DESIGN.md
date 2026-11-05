# device-namespace: one directory where every block device answers, whatever driver drives it

**Date**: 2026-11-04
**Status**: Proposed

## Problem

A machine with two disks answers on two names: `/svc/ours.block.Block` (virtio)
and `/svc/ours.block.Nvme` (the root.cm offer's rename). The wire protocol is
identical, but the *name* a client opens is per-driver, hardcoded in root.cm,
and does not scale — a second virtio disk or a second NVMe controller has no
name at all, because a root offer names a *driver*, not a *device*. The
reference shape is Fuchsia's devfs: one directory tree (`/dev/class/block/000`,
`001`, …) that driver_manager hosts, devices appear in at runtime, and every
node connects to the same protocol. Linux's `/dev/nvme0n1` beside `/dev/vda`
is the same bargain: names name devices, one interface speaks to all of them.

What forces it now: the block layer's next rounds (virtio multi-disk, one
driver N controllers) are blocked on "how does a client say which disk",
and the answer must not be more root.cm renames.

## Goals and non-goals

Goals:

- A client enumerates one directory and opens any block device it finds there,
  speaking the one `ours.block.Block` protocol.
- A driver with N devices publishes N entries, at runtime, without manifest
  edits per device.
- A boot with no devices of a kind shows an empty tree — presence is what the
  client sees, not which offers root.cm wrote.

Non-goals:

- Hardware claim/arbitration (which driver may drive a controller) — the
  driver-manager round's `Claim` (`.agents/tasks/driver-manager/DESIGN.md`,
  nothing built); the namespace composes with it later: the manager decides
  *who drives*, the namespace records *what answers*.
- Cross-class paths (`/dev/nvme0n1` style by-name subtrees); class
  directories first.
- Kernel changes: none. The tree is served by user space on the existing
  ioservice directory machinery.

## Design

The objects:

- `ours.dev.Registry` — the publish protocol. A driver calls
  `Publish(class, name, directory)`: the class ("block"), the instance name
  ("000", "001" — the driver numbers its own devices), and the server end of
  a directory the registry mounts into the tree.
- The **registry** — one component (`system/service/device-registry`,
  following acpi-route's shape: eager child, protocol out, directory out)
  whose outgoing directory *is* the device tree: `class/block/000`,
  `class/block/001`, …, each leaf a **service node** — the primitive `/svc`
  already uses: opening it forwards an Open (carrying the protocol's name)
  into the directory the driver published, and the reply is a channel that
  speaks `ours.block.Block`. The published directory is the forwarding
  target, not something the client walks through; the leaf is the endpoint.
  Readdir reflects what was published; a driver's exit unmounts its entries
  (the registry holds the server ends — a dead driver's channels close, and
  the node answers PeerClosed).
- The **client** — root.cm offers the registry's `dev` directory to any
  component that enumerates (routing_checks first), with rights `r*`:
  enumeration is its own bit (`_ENUMERATE`, checked by the directory
  server's readdir), and `r*` carries it beside read. The client readdirs
  `/dev/class/block`, opens each entry, speaks `ours.block.Block`.

The events, one boot with two disks:

1. init starts the registry (eager); its outgoing directory is an empty
   `dev` tree.
2. init starts the drivers; each connects to `ours.dev.Registry` (root.cm
   offers it), brings its controller up, and publishes one directory per
   device: virtio publishes `block/000`, nvme publishes `block/001` —
   publish order, not driver identity, names the nodes.
3. routing_checks opens the registry's `dev` directory (root.cm offer),
   readdirs `class/block`, finds two entries, opens each, and runs the one
   block check against both — the same code, no per-driver names.
4. A driver that finds no hardware publishes nothing; the tree shows the
   other disk alone.

The registry is built on what exists: ioservice directories (the same tree
kind bootfs and `/svc` use), the outgoing-directory server (the driver side
already exposes directories — btreefs's `disk` proves the forwarding), and
manifest offers with `subdir` joins (`Realm::forward_directory` already
composes them). New: the `ours.dev.Registry` protocol and the registry's
composition logic (mount a published directory at a path, unmount on close).

The tree's layout carries the reservation: `Publish` takes a class name and
an instance name, never a path, and the registry composes `class/<class>/<n>`
— so the top level outside `class/` is unallocated ground no driver can
claim, held for what the driver-manager round will hang there (a hardware
topology view beside the class view). When the manager lands it publishes
through the same protocol — it is the best-informed publisher, not a second
mechanism — and the tree gains its region by extending the registry, not by
replumbing it.

## Alternatives considered

- **Manifest-static directories** — each driver exposes a `dev` directory,
  root.cm offers them under `dev/nvme`, `dev/virtio`. Zero new machinery
  (`forward_directory` serves it today), but names stay per-driver and
  hardcoded: a second virtio disk still has no name, and the tree describes
  the image's offers, not the machine's devices. Rejected as the *end*
  shape; noted as the fallback if the registry round stalls.
- **Registry inside init** (a built-in, like `ours.resource.*`) — fewer
  moving parts, and init is this system's driver_manager counterpart, the
  layer Fuchsia hosts devfs in. Rejected for now: init is the trusted
  router of everything; a replaceable component keeps the tree's policy
  (naming, classes) out of the TCB, and acpi-route set the precedent. The
  door stays open: the protocol is the same either way.
- **The driver manager's Claim tree** — devices appear only after a driver
  claims them from the manager. Right in the end state, but the claim
  machinery is unbuilt (P3); the namespace does not need it and should not
  wait for it.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Who hosts the tree | child component (registry) / built-in in init | child component | — | user proceeded on the recommendations 2026-11-04 |
| What a device publishes | the driver's outgoing directory end / a bare protocol channel per device | the outgoing directory end — forwarding is directory-native; the client-visible leaf is the protocol endpoint (service node), not a per-device directory wrapper | — | user proceeded on the recommendations 2026-11-04 |
| Node naming | `class/block/000` numbered by publish order / by driver-given names | publish order, class dirs (Fuchsia devfs shape) | — | user proceeded on the recommendations 2026-11-04 |
| The tree's top level | registry composes every path; drivers name only a class and an instance / publishers name paths | reserve the top level: Publish mounts only under `class/`, `class/` outside stays unallocated for future views (the manager's topology), and no driver can squat them | — | user proposed the reservation 2026-11-04 |

## Testing and verification

- routing_checks gains `check_dev_tree()`: readdir `/dev/class/block`, expect
  one entry per attached disk (0 diskless, 2 dual-disk), open each and run
  the existing block GetInfo through it.
- The dual-disk boot (`DISK=` + `NVME_DISK=`) is the round's verdict image:
  both devices in one tree, both silent-passing.
- `check_block`/`check_nvme` keep working unchanged during the migration;
  they move to the tree only once it answers both.

## Open questions

- Unmount semantics on driver exit: registry notices the published
  directory's channel close (PeerClosed on next open? eager removal on the
  channel-wait the Loop already runs?). Decide with the ioservice internals.
