# client-rpc: the round's four steps

**Date**: 2026-11-04
**Status**: Proposed

## Request

The user's consolidation: stop pasting the call pipeline per client -- one
rpc core under the typed clients, the two missing families covered,
routing_checks off raw frames. The IDL/generator path stays future work;
this core is its prototype.

## Decisions

See DESIGN.md's table (all three settled 2026-11-04).

## Steps

1. **The core.** `//system/lib/rpc` -- `rpc::Channel` (call, next_txid,
   ownership), user-only, plus its host suite over the fake kernel. Verified
   by the suite green.
2. **The migrations.** block/resource/acpi/dev clients conduct through the
   core; their public APIs unchanged; their buffer/txid members go.
   Verified by the protocol suites and the registry suite green unchanged.
3. **The two families.** `prot::fshost::Client` (Mount); routing_checks'
   mount check speaks it. Verified by the mount check passing through it.
4. **The raw frames retire.** routing_checks' check_block/check_nvme move
   onto prot::block::Client; the local `call()` helper and the
   wire-level probes that test refusals stay (they test the wire). Verified
   by routing_checks passing.
5. **The record.** docs/changes/261104/client-rpc.md, RESULT.md, the rpc
   README.

## Files

- Created: `system/lib/rpc/**`.
- Changed: the four protocol client headers, `routing_checks.{cpp,BUILD.gn}`.

## Test plan

Every step green on `boot_check.py --out out/rpc.x86`; steps 3-4 also
dual-disk. clang-format on everything changed; pre-commit green.
