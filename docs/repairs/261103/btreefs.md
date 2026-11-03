# btreefs outstanding repairs

None. The cache-exhaustion item (one unlink or truncate transaction
exhausting the 48-entry shadow cache at ~16 removes, files over ~64-80 KiB
undeletable) is fixed: `Transaction::checkpoint()` writes the shadow blocks
through per loop turn, the rights gates are in, and the 256 KiB unlink and
truncate tests pass — `docs/changes/261103/btreefs-rights.md` carries the
record.
