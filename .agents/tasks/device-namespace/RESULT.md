# device-namespace: delivered — one tree, two leaves, one protocol

**Date**: 2026-11-04

## Delivered

- `feat(protocols): prot::dev, the device tree's publish codec` (b90268b5) —
  the Registry protocol, the granted-name reply, the host suite (4 tests).
- `feat(device-namespace): the registry, the publishers, the tree check`
  (872d49a0) — the device-registry component, both drivers publishing, the
  root.cm offers, `check_dev_tree()` with its leaf-count verdict, the
  instance pool's seat.
- The change record (`docs/changes/261104/device-namespace.md`) and the
  registry's README.

## Verification

- The review round (0c77b3f3): the core extracted to registry.hpp and driven
  on the fake kernel -- 9 registry tests + 8 dev codec tests (13 new), the
  strictness fix, fake_kernel's peer_closed; full verify green after.

- Dual-disk (the verdict image): both leaves mounted (`block/000` from nvme,
  `block/001` from virtio-blk — the grant rule naming both), GetInfo through
  each answering 131072 blocks, `routing_checks: passed` 7/7, the virtio
  chain's GPT/btreefs checks green alongside.
- Diskless: `the device tree carried 0 leaf(s)`, the check's skip line, full
  dbg 46 / rel 44 / host 162 green.

## Deviations from the design

- The publish protocol gained a third string (the wire protocol name a
  leaf's open is forwarded under) and its reply gained the granted name —
  both post-design revisions made during implementation, the second because
  the mount handle cannot travel twice.
- `check_dev_tree` opens leaves through `fsio::connect` (ProtocolService),
  not a plain path open: a leaf is a service node, and the first dual-disk
  run's AccessDenied taught it.

## Found along the way

- **init's instance pool ran out again** (the registry the thirteenth
  instance): the boot died resolving meta/root.cm with NoResource, the same
  failure nvme's seat fixed at 12. Now 13; the pattern ("every new component
  bumps the pool with its own commit") is now in the pool comment's history.
- The registry agent hit the user's quota limit mid-task; its three files
  had landed and were verified by build+boot, and the orchestrator finished
  the wiring (includes, fsio dep, the two repair classes).
- Two further Write-payload corruptions (the agent's earlier prompts aside,
  the change record draft corrupted twice) — all caught by the recorded
  verify-disk procedure, nothing landed corrupted.

## Not done

- Unmount-on-exit declared but untested (no check kills a driver mid-boot).
- The reserved top level (topology views, by-name subtrees) — later rounds.
- The `/svc/ours.block.*` checks stay until the tree has answered both disks
  for a round; retiring them is the next round's first commit.
