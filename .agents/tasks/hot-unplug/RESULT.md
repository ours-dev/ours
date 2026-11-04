# hot-unplug: delivered — the registry takes devices back out

**Date**: 2026-11-05

## Delivered

- `feat(ioservice): a service node can be taken back out` (134cc80c) —
  Tree::remove_service, the unlink and the retired slot; Server's guard.
- `feat(dev): Unpublish, and the registry takes a leaf back out`
  (dd7e1195 / 916a61cf merged forms) — the codec, the registry's unmount
  path, the registry suite's three un-publish cases.
- `feat(device-registry): the death watcher, and the un-publish`
  (2a0c3d06) — the PeerClosed watcher per mount, the seat-exhaustion
  degradation, the double-report case, Core::connect's guard, the
  hostile un-publish frames.
- `feat(dev): the typed un-publish, on the client` (ea3dea85) +
  `test(dev): the un-publish client's refusals` (61fc5e4f) —
  Client::unpublish and its refusals to the bar.
- `fix(device-registry): the class directory's name outlives the mount
  slot` (2e80ee83, earlier in the round) — the phantom-rename defect the
  coverage probe caught.

## Verification

- Host suites: ioservice 141, registry 24, dev_client 12, dev codec 14,
  block_client 19, fshost 8, resource 12, acpi 16 — all green.
- Coverage (llvm-cov, scripts/cov_host.py): every round file ≥95% lines —
  rpc 100, dev_client 95.7, dev.cpp 100, registry.hpp 97.3→ (final 98.3 on
  the registry unit), block 97.1, acpi 97.9, resource 97.0, fshost 100,
  tree.cpp/server.cpp 95.9.
- Dual-disk on target: dbg 46 / rel 44, both leaves mounted and answering,
  routing_checks passed.

## The coverage bar drove

- The phantom-rename defect (class directories' names viewed the mount
  slot's storage) — fixed by the class-name arena.
- The guaranteed-exit restructures (grant scan, class_name, build)'s
  unwraps.
- The seat-exhaustion case (saturate the Loop's slots to 45: the mount
  mounts and answers, the console says the publisher is unwatched).

## Deviations from the design

- The task list grew a step the plan did not name: the phantom-rename fix
  (the class-name arena) — the coverage probe caught it mid-round, and it
  gated the merge like everything else.
- The kernel wait_async contract pin from the plan turned out unnecessary:
  the death watcher never depends on a packet after a handle close (it
  answers Close, and the publisher's exit is what fires it).

## Found along the way

- The plan-mode exploration's finding stood: Server::dispatch dereferenced
  a tree node's kind unwrapped — a removed node would have been UB, not a
  status. The guard is part of step 1.
- The stale-binary trap bit three times in this round (edits landing in
  the same second as the last build). The rule that held: touch the
  source, rebuild, THEN read the failures.

## Not done

- The drivers do not unpublish on device loss at run time (no hardware
  detection exists — the software layer only).
- registry.hpp's 11 remaining zero lines are the structurally
  unreachable guards (startup layout, invalid service values, the
  class_name tail), recorded as such.
