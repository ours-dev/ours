# macro-fs: the composing server, two filesystems, and writes across them

**Date**: 2026-10-30

## Delivered

- `077672c1` feat(ioservice): remote entries and Open remainder forwarding — `NodeKind::Remote`,
  `Tree::walk`, `Action::Forward`; nine host tests.
- `682dd3cf` feat(component): a component may declare and expose a directory — the three compiler
  refusals lifted, acceptance tests in their place.
- `805e18f3` feat(init): route a directory use into a provider's outgoing directory — the same
  fire-and-forget forward a protocol's Open takes.
- `220cff47` fix(init): the forwarded Open of a provider directory carries no trailing slash.
- `0491a22e` feat(fshost): the composing server, with /boot mounted and a shell read across two
  servers.
- `50c8358a` feat(ours.io): File.Write, and fsio::write for it.
- `987e9810` feat(memfs): the first writable filesystem, mounted at /vfs/tmp.

Change records: `docs/changes/261030/{ioservice,component-framework,macro-fs,macro-fs-write}.md`.
Design: `docs/design/261030/macro-fs.md`.

## Verification

`boot_check.py` at the final commit: build ok; dbg PASS (44 passed, 0 failed); rel PASS (42 passed,
8 skipped, 0 failed, 6/6 required); host-tests PASS 148. The rel log carries the full flows:
`/vfs/boot/bin/shell` read across two servers, `/vfs/tmp/hello` created, written and read back
across three (`shell: vfs checks passed`), and `routing_checks: passed` with its three `/vfs`
checks. Not measured: forwarding latency (no performance work was asked for).

## Deviations from the design

- The design's Plan (`.agents/tasks/macro-fs/PLAN.md`) had the mount table's shape as step 5 with a
  user decision first; it stays open and is not started.
- ioservice's `Forward` failure now reaches the delegate through `write_failed` — an interface
  extension the debugging needed and the semantics wanted (a forwarded Open is a message written to
  a peer); fshost reports it.
- The exposed name is a provider mount's root: init's forwarded Open carries `tmp`, so a provider
  must answer an Open of its exposed name as its root. Discovered on target, now in the design.

## Found along the way

Fixed in the commits above and recorded in the change records: the trailing slash in init's
forwarded directory Open; shell.cm missing the `vfs` use; the empty-bodied listing loop in
check_vfs; memfs's exposed name treated as a file name; memfs's served ceiling missing
GetAttributes/UpdateAttributes. Nothing left unfixed; no `docs/repairs` entries.

## Not done

- The dynamic mount table (step 5): its shape is the user's decision, and nothing can mount
  dynamically before a block device exists. Recorded in the design's Open questions.
- Block devices, partitions, disk filesystems: out of scope, as the plan says.
- memfs file removal and subdirectories: deferred until asked for.
