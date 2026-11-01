# gpt-reader: read a disk's partition table over the block protocol

**Date**: 2026-11-01
**Status**: Proposed

## Request
The user approved the step as introduced: a GPT reader library on top of
ours.block, so the macro filesystem's next layer can know which partitions a
disk carries and what kind each is. Scope ends at reading and validating;
mounting, writing GPTs and choosing the filesystem inside a partition are
later steps.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|

## Steps
1. `system/lib/ours.gpt`: the pure parser and the CRC32. Takes the bytes of
   LBA 0, the header LBA and the entry array, and answers the validated
   partition list, or which check failed. Pure code with no kernel calls, so
   a host test can feed it a synthetic image and check every refusal. Also
   the pieces of the layout it must know: protective MBR's 0x55AA and its
   one partition of type 0xEE covering the whole disk, the `EFI PART`
   signature, the header's CRC32 over its first 92 bytes, and the entry
   array's CRC32 over size × count bytes.
2. `system/lib/ours.block`: the client side, one `Connection` class -- open
   by protocol name through the namespace, GetInfo, Read, Write over the
   hand-written codec, closing what it opens. routing_checks hand-rolls
   these calls today; the parser's caller should not.
3. A script to lay a GPT onto a disk image (`scripts/make_gpt_disk.py`), and
   routing_checks gained with it: parse the table through the protocol and
   check the partitions it finds against what the script wrote.
4. Verify: host tests (the parser's), boot_check both images, a disked boot
   with the GPT image whose checks pass.

## Files
- `system/lib/ours.gpt/` — new library: parser, CRC32, BUILD.gn, host test.
- `system/lib/ours.block/` — client Connection.
- `scripts/make_gpt_disk.py` — lays a GPT onto an image.
- `system/service/init/test/routing_checks/routing_checks.cpp` — the GPT check.

## Test plan
Host: ours.gpt's test, fed a synthetic image — the happy table, and one test
per refusal (bad MBR magic, bad signature, bad header CRC, bad entries CRC,
entry past the disk). Boot: `boot_check.py` both images; a disked boot whose
routing_checks prints the GPT check passing. Skip-on-diskless stays as the
block checks have it.
