# device-namespace changes

## Title: one directory where every block device answers, whatever driver drives it (2026-11-04)

**Files**
- `system/service/device-registry/**` — the registry: one component whose outgoing directory *is* the tree (`dev/class/block/<n>`); the publish protocol served on a ProtocolServer; a Mount pool copying the three strings; the grant rule (proposal or the class's lowest free number); a forwarding Handler per mount; the tree as ioservice Tree + Server + a SubtreeHandler.
- `system/lib/protocols/include/prot/dev.hpp`, `src/dev.cpp`, `test/dev_test.cpp` — the publish codec: three strings back to back (class, name, protocol), the handle's presence marker before them, the reply carrying the granted name.
- `system/lib/protocols/include/prot/dev_client.hpp` — the client plumbing (granted-name span + NUL).
- `system/driver/virtio-blk/virtio_blk.cpp`, `system/driver/nvme/nvme.cpp` — the publish block after bring-up; their manifests use `ours.dev.Registry`.
- `kernel/products/image/dev/BUILD.gn` — the child, the Registry offer to both drivers and routing_checks, the `dev` directory offer (r*) to routing_checks.
- `system/service/init/test/routing_checks/**` — the `/dev` use, `check_dev_tree()`: list, open each leaf as a service, GetInfo through it; the leaf-count verdict line.
- `system/service/init/pools.hpp` — MAX_INSTANCES 12 → 13 (the registry is the seat).

**Symptom**
None — a feature. Before: two disks answered on two per-driver names
(`/svc/ours.block.Block`, `/svc/ours.block.Nvme`), hardcoded in root.cm; a
second disk of one kind had no name at all, because an offer names a driver,
not a device.

**Cause**
None — the structure followed the drivers as they landed, not a rule. The
rule now written down: a root offer names a *driver*; a *device* is named by
presence in a tree the drivers publish into.

**Change**
The grant rule is the round's one non-obvious mechanism:

```cpp
// The driver proposes once; the registry grants the proposal or the
// class's lowest free number, and the reply carries what was granted.
auto const published = registry.publish("block", "000", prot::block::BLOCK_NAME,
                                        mount, std::span<char>{granted});
```

The reason the reply owns the name: the publish carries a transferred
directory handle, which cannot be sent twice, so a driver-side retry would
need a fresh channel pair per attempt -- the registry picking the name turns
"propose, maybe retry" into "propose once, be told". Two drivers proposing
000 in one boot get 000 and 001, and the boot log names the tree's growth:
`device-registry: block/000 mounted from a publisher`.

**Reference**
- The tree is Fuchsia devfs's class layout (`/dev/class/block/000`, the
  leaves connectable, driver_manager hosting); the registry component is the
  role driver_manager plays there, at this system's component scale.
- The grant rule is the registry's own answer to a distributed-name problem;
  no counterpart named, this project's own design.

**Verification**
Dual-disk (the verdict image): `block/000` mounted from nvme, `block/001`
from virtio-blk; `the tree answers block/000 with 131072 blocks` and the
same for block/001 -- GetInfo through both leaves, one protocol, two
drivers; `routing_checks: passed`, 7/7 required, and the virtio chain's
GPT/btreefs checks passing alongside. Diskless: the registry serves an
empty tree, `the device tree carried 0 leaf(s)`. Host: the dev codec suite
joins, 162 passed. clang-format clean on everything changed.

**Deliberately not done**
- Unmount on publisher exit is declared (the registry holds the server ends;
  a dead driver's channels close) but not yet verified by a test: no check
  kills a driver mid-boot.
- By-name subtrees (`/dev/nvme0n1`), hardware topology views: the tree's
  top level outside `class/` is reserved ground (DESIGN.md's reservation).
- The old `/svc/ours.block.*` checks (`check_block`, `check_nvme`) stay:
  they retire once the tree has answered both disks for a round.

**How it was found**
None — a feature round; the debugging incidents (the pool seat, the
leaf-open-as-service) are recorded in the task's RESULT.md.

**The user's prediction**
None — the user approved the design's recommendations; no prediction was
asked for this round.

**Three questions you should be able to answer**
- Why does the reply carry the granted name instead of the driver retrying
  with 001?
- What does a leaf open forward, and where does the protocol name in it come
  from?
- What does check_dev_tree see diskless, and what line says so?
