# gpt-reader: the partition table read over the block protocol

**Date**: 2026-11-01

## Delivered
- `36215620` — ours.block's Client: the codec's client side over one channel
  (txid, buffers, wait, epitaph-as-status).
- `bfeb7694` — ours.gpt: IEEE CRC-32, header and entry parses, `read(Reader&)`
  template over whatever reads blocks; 13 host tests.
- `ea676244` — routing_checks `check_gpt()` and check_block's two disk shapes;
  `scripts/make_gpt_disk.py` writes the GPT image.

The full record is `docs/changes/261101/ours-gpt.md`.

## Verification
Host 156 tests pass (13 ours.gpt, CRC-32 against its known answer 0xCBF43926);
`boot_check.py` dbg PASS (44) and rel PASS (42, 6/6 required); three disk
shapes booted — no disk, plain image, GPT image — `routing_checks: passed` on
all three.

## Deviations from the design
None.

## Found along the way
The Client's first diskless rel boot answered InvArgs instead of a clean skip:
a closed connection's epitaph is a message until asked, and the Client must
answer the epitaph's status, not feed its 24 bytes to the reply decoder. Fixed
in the Client's own commit.

## Not done
Mounting, writing GPTs, and the choice of the filesystem inside a partition:
the next step's decisions, for the user.
