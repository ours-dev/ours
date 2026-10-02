# launch-plan-exercise: how a writable segment becomes a private VMO and its mappings

**Date**: 2026-09-29

The writable segment S3 of the `TypicalLayout` test in `system/lib/launch/test/plan_test.cpp`: `vaddr` 0x3f00, `file_offset` 0x2f00, `file_size` 0x180, `mem_size` 0x2180, RELRO `{0x3f00, 0x100}`. Its `first` is 0x3000 and its `end` is 0x7000, so its private VMO is 0x4000 bytes. The numbers are the ones the test expects.

```mermaid
flowchart LR
    subgraph FILE["File (the executable in BootFS)"]
        F1["0x180 bytes from file_offset 0x2f00<br/>first 0x100: RELRO, last 0x80: .data"]
    end

    subgraph VMO["S3's own private VMO, 0x4000 bytes, starting at the page start of first = 0x3000"]
        direction TB
        V0["VMO 0x0000-0x0f00<br/>zero: the rest of this page, not the segment's"]
        V1["VMO 0x0f00-0x1000<br/>RELRO, 0x100 bytes"]
        V2["VMO 0x1000-0x1080<br/>.data, 0x80 bytes"]
        V3["VMO 0x1080-0x4000<br/>.bss, all zero"]
        V0 --- V1 --- V2 --- V3
    end

    subgraph AS["Address space (vma_map)"]
        M3["mapping 3<br/>vaddr 0x3000, size 0x1000<br/>vmo_offset 0x0000, Perm::R"]
        M4["mapping 4<br/>vaddr 0x4000, size 0x3000<br/>vmo_offset 0x1000, Perm::RW"]
    end

    F1 -- "Copy: file_offset 0x2f00, size 0x180<br/>vmo_offset 0xf00 (= vaddr - first)" --> V1
    V0 -. "page 0" .-> M3
    V1 -. "page 0" .-> M3
    V2 -. "pages 1-3" .-> M4
    V3 -. "pages 1-3" .-> M4
```

## Reading it
- Every writable segment has a private VMO of its own, and offsets in it count from 0 at that segment's `first`. Nothing accumulates from one segment to the next.
- `Copy.vmo_offset` (0xf00) is where the segment's first byte sits in its VMO: a VMO is whole pages, and the segment starts partway into its first page. The bytes before it stay zero.
- `Mapping.vmo_offset` says where in that one VMO a mapping starts. RELRO cuts S3's VMO into two mappings of different protection: page 0 is wholly RELRO, so it is `R` at offset 0; pages 1 to 3 are `.data` and `.bss`, `RW` at offset 0x1000 (`page - first`). With no RELRO the segment is one mapping at offset 0.
- `Mapping.segment` is the index of the segment, 3 for both mappings, not the number of the private VMO among the private VMOs.
