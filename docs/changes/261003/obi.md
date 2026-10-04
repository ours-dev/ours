# obi changes

## Title: obit compresses with upstream LZ4, vendored at v1.10.0, and proves each block against the boot decoder (2026-10-03)

**Files**
- `vendor/lz4` (new submodule, `https://github.com/lz4/lz4.git` at tag v1.10.0, `ebb370ca`), `.gitmodules`.
- `build/secondary/vendor/lz4/BUILD.gn` (new) — `//vendor/lz4:lz4`, `lib/lz4.c` and `lib/lz4hc.c` only (BSD-2-Clause, `lib/LICENSE`).
- `kernel/lib/obi/cli/lz4_block.{hpp,cpp}` (new) — `obit::lz4_compress_block()`: `LZ4_compress_HC` at `LZ4HC_CLEVEL_DEFAULT`, then a decode with `obi::compression::decompress()` and a byte comparison.
- `kernel/lib/obi/cli/pack.cpp`, `kernel/lib/obi/cli/BUILD.gn` — obit uses it in place of `compress_literals()`, and links `//vendor/lz4`.
- `kernel/lib/obi/include/obi/compression.hpp` — the comment says the literal encoder is kept for the tests only.
- `kernel/lib/obi/test/pack-smoke.py` — scenario 3: the same files packed with and without lz4 extract to the same bytes, and the compressed image is less than half the size.
- `docs/repairs/260925/obi.md` — item 3 removed. `docs/repairs/261003/obi.md` (new) — the decoder's per-byte division, now that matches dominate.

**Symptom**
Every BootFS item was marked `Compression::Lz4`, but the encoder wrote literals only: the bytes as they were, behind a few length bytes. The rel image was 1214736 bytes. The decoder in physload and userboot had never decoded a match on a real image.

**Cause**
The objects:
- LZ4's raw block format: sequences of literals followed by a match, an offset back into the output and a length.
- `compress_literals()`: an encoder that emits one sequence of all literals and no match. It is valid LZ4 and compresses nothing.
- `obi::compression::decompress()`: a full block decoder, matches included, compiled into physload and userboot.

The encoder was written small on purpose, waiting for the user's approval of a third-party dependency (`layering.md`: every new third-party library needs it). The user approved upstream LZ4 on 2026-10-03, which they had used in the project's earlier CMake tree.

**Change**
- **Vendoring.** `vendor/lz4` is a submodule pinned to v1.10.0, as `vendor/cxxopt`, `vendor/googletest` and `vendor/nlohmann_json` are, with its BUILD.gn in GN's secondary tree. It is built for host tools only, and is the tree's first C target; upstream compiles clean with the host flags and `-Werror`.
- **The encoder.**

  ```cpp
  auto const written = LZ4_compress_HC(src, dst, input_size, LZ4_compressBound(input_size),
                                       LZ4HC_CLEVEL_DEFAULT);
  // then: decompress() with the boot chain's decoder, and compare with the input
  ```

  The decode-and-compare runs on every item obit packs, so each image the build produces is proven to round-trip through the code that will read it at boot. A host round-trip test would prove the same only for its own inputs.

**Reference**
- Upstream `LZ4_compress_HC()` and `LZ4_decompress_safe()` (`vendor/lz4/lib`).
- Fuchsia's zbi tool compresses STORAGE_BOOTFS with zstd or LZ4's frame format (`zbi.cc`). OBI uses the raw block, with the decoded size in the item descriptor.

**Verification**
- `ninja -k 0` on `out/bringup.x86`: no error or warning. The in-build `pack-smoke` and `differential-smoke` actions pass.
- Image sizes:

  | | before | after |
  |---|---|---|
  | rel `kernel.dev.obi` | 1214736 | 621920 |
  | its BootFS item | 217941 | 109617 |
  | its kernel package item | 1060914 | 370814 |
  | dbg_1 `kernel.dev.dbg_1.obi` | 1292840 | 633240 |

- `boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed). This was the first boot whose physload and userboot decoded LZ4 matches, and the BootFS sizes userboot reports are unchanged (0x2e000 and 0x35000 bytes).
- `pack-smoke` run against an obit built from before this change fails scenario 3, as it should: `lz4 did not compress: 234584 bytes against 233664 uncompressed`.

**Deliberately not done**
- The decoder's speed (`docs/repairs/261003/obi.md`, item 1): a division per match byte. It is to be measured first.
- The second literal-only encoder in `kernel/lib/lz4`, used only by `lz4-wrapper-smoke`, is left as it is.
- The `vendor-import` skill's out-of-date instructions are corrected in a separate commit.

**How it was found**
None: the repair item, and the user's choice of upstream LZ4.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a literal-only LZ4 stream decode correctly but never exercise the decoder's match path?
- Why is the decode-and-compare in obit worth more than a host test of the encoder?
- Why is upstream LZ4 built only for host tools, while the kernel keeps its own decoder?
