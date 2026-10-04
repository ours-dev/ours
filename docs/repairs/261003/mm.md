# mm outstanding repairs

## 1. Setting a frame flag overwrites the others: `mark_reserved()` clears `Initialized`, `mark_pinned()` clears `Reserved`

**Severity**
Low: no wrong result found today. The only reader of the flags it erases is the reserved-frame stamping, which then skips frames and undercounts. It becomes Medium once anything trusts a frame's flags.

**Symptom**
Latent. Every page-table frame stamped at boot ends with only `Pinned` set, without `Reserved`. Every reserved frame ends without `Initialized`. The guard added on 2026-10-03, that no Reserved frame reaches the buddy (`free_frames_phys_range()`, `kernel/mm/early_mem.cpp`), cannot see a page-table frame for that reason.

**Cause**
`PageFrame::set_flags()` (`kernel/mm/include/ours/mm/pm_frame.hpp`) is `m_status.set<Index::Flags>(flags)`. `BitFields::set()` writes the whole field: an assignment, not an OR. So `mark_reserved()`, `mark_pinned()` and `mark_active()` each replace every flag the frame had. `stamp_reserved_frames()` (`kernel/mm/memmap_init.cpp`) calls `mark_reserved()` and then, for a page table, `mark_pinned()`. It also skips a frame without `Initialized`, which `mark_reserved()` has just cleared. So a frame two reservations share, or a hole `init_memmap_reserved()` marked first, is skipped and not counted.

**Change**
Make the `mark_*` setters OR their flag in (`set_flags(flags() | Flags::X)`), and keep an explicit replace for the one place that means it, if any. Then check the per-type counts `stamp_reserved_frames()` logs (`Reserved N frames of ...`) before and after: they may grow, which is the fix showing.

**Reference**
Linux's page flags are independent bits set with `SetPageReserved()` and the like (`include/linux/page-flags.h`), each a `set_bit()` on `page->flags`.

**Acceptance**
- After boot, a page-table frame has both `Reserved` and `Pinned`, and a reserved frame keeps `Initialized`.
- The stamping counts are explained.
- `ours-verify` passes.
