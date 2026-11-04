# client-rpc: delivered — one pipeline, every client on it

**Date**: 2026-11-04

## Delivered

- `feat(rpc): the call pipeline, as an object` (0807a99c) — rpc::Channel,
  the 5-test host suite over the fake kernel.
- `refactor(protocols): the clients conduct through rpc::conduct`
  (61c81e3d) — block/dev onto Channel; resource/acpi onto the free
  `rpc::conduct` with their own verdicts; the conduct/call split (the
  resource reply's handle is the payload — call() would close it).
- `feat(protocols): the fshost client, and the raw frames retire`
  (8d7a9533) — prot::fshost::Client, routing_checks' mount/check_block/
  check_nvme onto typed clients.
- The change record and the task's RESULT.

## Verification

- Host: rpc 5 / registry 9 / dev 8 — all green through the real core.
- Dual-disk on target: dbg 46 / rel 44 / host 166, `routing_checks:
  passed`, the tree answering both leaves, the mount through the typed
  fshost client.
- The dangling-view bug the first conduct shape carried (14 on-target
  failures, every resource consumer) is fixed by caller-owned reply
  buffers; the story is in the change record.

## Deviations from the design

- The design said the four clients migrate onto Channel; the round's
  discovery is that two of them must NOT — their replies carry a payload
  or enforce a no-handle rule, so the core split into `conduct` (raw,
  caller-owned buffers) and `call` (translate + close strays). DESIGN.md's
  "Open questions: None" was wrong for one step; the answer is in the
  change record.

## Found along the way

- **The final verify caught a real leak**: the conduct/call restructure had
  dropped the success path's handle-close (only the epitaph and bad-header
  paths closed); StrayHandleClosed failed on the fresh build and passed at
  the round's own commit on a stale binary. Fixed in 9f1f50a6 — one close
  on the path every reply takes. Two verify-matters lessons in one round.

- The dangling-view bug (above) — caught by on-target verification, not
  by the host suites, which passed on dead-frame luck.
- The migration agent's halt was the round's save: it refused to migrate
  resource through call() and flagged the payload contract, which became
  the conduct/call split.

## Not done

- process.Launcher (server-side conductor; stays), io (fsio is the
  client), echo (deliberately wire-probed).
- The IDL/generator path — deferred by the user's sequencing; rpc::Channel
  is its prototype.
