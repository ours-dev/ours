# obi outstanding repairs

## 1. The LZ4 decoder divides once per match byte

**Severity**
Low: a cost at boot, paid by physload for the kernel package and by userboot for the BootFS. Not a wrong result. How large is to be measured before anything changes.

**Symptom**
Not measured. Since obit compresses with upstream LZ4 (2026-10-03), most of a decompressed item comes from matches rather than literals: the kernel package is 370814 bytes stored for about 850 KiB decoded. Every match byte costs an integer division.

**Cause**
`obi::compression::decompress()` (`kernel/lib/obi/include/obi/compression.hpp`) copies a match with

```cpp
for (usize index = 0; index < match_length; ++index) {
    output[output_offset++] = output[match_offset + index % offset];
}
```

The `% offset` is not needed. When a match overlaps its own output (offset < length), a byte-by-byte forward copy from `match_offset + index` already reads bytes the same loop wrote, which is exactly LZ4's semantics. The division only made sense while the encoder emitted no matches at all, so nothing measured it.

**Change**
Measure first, and bring the numbers to the user:
- the cycles physload spends in `decompress()` for the kernel package, and userboot for the BootFS (a TSC read before and after, logged once);
- a host benchmark in `benches/` of `decompress()` on the two real items, against upstream's `LZ4_decompress_safe()` on the same input.

Candidates:
- drop the `%`;
- copy non-overlapping matches with `memcpy` and overlapping ones byte by byte, as upstream does;
- wild copies in 8- or 16-byte steps. That needs output slack, which the OBI format does not promise.

**Reference**
- Upstream `LZ4_decompress_generic()` (`vendor/lz4/lib/lz4.c`): a fast path for offsets of 8 bytes or more, and a byte loop for overlaps.
- Linux `lib/lz4/lz4_decompress.c` is the same code.

**Acceptance**
- The benchmark's numbers are in the change record, before and after.
- The decoder still passes the OBI host tests and `pack-smoke`.
- Both images boot under `ours-verify`.
