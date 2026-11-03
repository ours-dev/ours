# protocols-library: delivered — one library, one namespace, bytes unchanged

**Date**: 2026-11-04

## Delivered

- `docs(protocols-library): the design and the plan` — the round's docs.
- `refactor(protocols): every wire protocol in one library, namespace prot`
  (86c21db6): the six libraries → `//system/lib/protocols`, namespace
  `prot::<family>`, includes `<prot/...>`; 121 files changed, git shows
  every moved file as a rename (86-99% similarity), the six old dirs gone
  (`ours.gpt`/`ours.btreefs` deliberately kept).
- `chore(gitignore): the security hook's state directory stays local`
  (8042a8f5) — `.mimosa/` swept in by a whole-tree add, untracked and
  ignored.
- This record and the change record (`docs/changes/261104/protocols.md`).

## Verification

- Full `boot_check.py`: dbg 46 / rel 44 / host 161, diskless; dual-disk rel
  PASS — both disk chains silent-passing, `routing_checks: passed`.
- clang-format (repo formatter, per-lib .clang-format with `<prot/` as the
  internal group) and format_gn clean on everything changed.
- Every moved file diff-verified against the transformed original (the only
  hunks: guard/include/namespace/doc renames); 161 host tests include all
  six protocol suites.

## Deviations from the design

- The BUILD.gn's client source_set holds `src/{resource,acpi}_client.cpp`
  only — block's client is header-only, as it was before (the plan's file
  list said "two codec sources"; the real inventory was eight sources).
- A `:client` dep was needed on seven targets the first pass missed (shell,
  nvme, virtio-blk, btreefs, acpi-route, routing_checks — where a `:client`
  dep had been collapsed into bare `protocols`); the link caught it
  (undefined `prot::resource::get`), fixed by pointing those targets at
  `:client`, whose public_deps carries the codec transitively.

## Found along the way

- **The abstraction audit came back clean** (three independent movers checked
  per file): no device names, no driver-framework types, no kernel internals
  beyond `ours::Status`/`Uandle`/wire/ustl in any codec; the client plumbing
  reaches syscalls + fsio by design (user-only target); test-only
  kernel-adjacent constants (`ours::ROOT_INTERRUPT_DOMAIN` in acpi_test,
  `ours::handle_info`/`HandleType` in process_test) are shared user API in
  tests, not protocol surface.
- **Two mid-round incidents, both handled by the recorded procedure**: a
  Write payload corrupted partway (block.hpp — the provider-side failure mode
  already on record; verified on disk, deleted, rewritten in chunks,
  diff-verified), and an agent prompt corrupted partway (stopped before it
  ran, redispatched clean).
- One concurrency limit note: five agents dispatched at once exceeded the
  user's concurrency limit; two failed fast without running. Redispatched
  one at a time afterwards.
- One whole-tree `git add -A` swept the Mimosa hook's state directory into
  the commit; untracked and ignored now.

## Not done

- `ours.gpt`/`ours.btreefs` namespaces (`ours_gpt`, `ours_btreefs`): still
  carry `ours_` though they are formats, not protocols — a later mechanical
  rename if wanted.
- The mixed abstraction in routing_checks/btreefs (raw wire frames beside
  typed clients) predates the round.
- exp/nvme and exp/protocols are both unmerged to baseline; the device-
  namespace round (exp-device-namespace) will rebase onto this.
