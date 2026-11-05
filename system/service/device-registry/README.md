# device-registry

The device tree: one directory where every block device answers, whatever
driver drives it. A driver publishes one leaf per device after bring-up
(`prot::dev`'s Registry.Publish — class, proposal, protocol, and the server
end of its outgoing), and the registry mounts it at
`dev/class/<class>/<n>`: a service node whose opens forward into the
published directory under the leaf's protocol name. A client lists
`/dev/class/block`, opens a leaf as a service (`fsio::connect`), and speaks
`ours.block.Block` — the same protocol whichever disk answers, which is the
whole point.

## What the registry owns

- **The name.** The driver proposes; the registry grants the proposal or the
  class's lowest free three-digit number, and the reply carries what was
  granted. Two drivers proposing 000 get 000 and 001. The reason the
  registry picks: the publish carries a transferred directory handle, which
  cannot travel twice, so a driver-side retry would need a fresh channel
  pair per attempt.
- **The forwarding.** Each mount holds the published directory's server end
  and a per-leaf handler; an open of the leaf encodes
  `Open(svc/<protocol>)` with the client's own flags and writes it into the
  published channel. The registry holds nothing else of the device.
- **The tree.** ioservice Tree + Server under a SubtreeHandler: `.`, `class`,
  `class/block`, the leaves below. The top level outside `class/` is
  reserved ground (the design's reservation): Publish composes paths only
  under `class/`, and no publisher can squat what the driver-manager round
  will hang there.

## What the registry does not do

No claim, no arbitration, no power over the devices: a publish grants the
power to name a leaf and to serve whatever the publisher serves. Hardware
arbitration is the driver-manager round's Claim; the tree records what
answers.
