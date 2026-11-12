# mm outstanding repairs

## 1. A frame descriptor is only valid if its storage was zeroed first

**Severity**
Medium. Correct today only because `SparseMemMap::activate_section()` asks for `Gaf::Zero`; a memmap allocated any other way would not boot.

**Symptom**
Latent, cannot be triggered today. With the `Gaf::Zero` taken off the memmap allocation at `kernel/mm/sparse_memmap.cpp:173` (tried on 2026-11-12, as the acceptance test of the order-field item): the dbg image panics in `PageFrame::init()` with "Duplicated to initialize a frame", and the rel image takes an invalid-opcode trap at an assertion's `ud2` in `FrameBuddy::release()` (`+0x3decc`) while the reserved frames are being stamped.

**Cause**
`PageFrame::init()` is not a constructor: it writes the zone type, section, node, role, order and flags, and leaves the rest of the descriptor as it found it. Three things still read that leftover storage.

- `init()` itself opens with `DEBUG_ASSERT(!(flags() & Flags::Initialized))`, which reads the Flags field before the function has written it. On zeroed storage the check means "not initialised twice"; on anything else it reads a random bit.
- The list hook `m_hook` is an intrusive hook, and an unlinked hook has a defined representation. The buddy links and unlinks through it.
- `m_references`, `m_compound` and the role body `m_body` are read by their owners on the assumption that a fresh frame has them at zero.

`PageFrame() = default` is what lets a memmap be allocated as an array before `init()` runs, so the default constructor establishes nothing either. The invariant "a descriptor starts out empty" is therefore kept by the allocation call in another file.

**Change**
Make `init()` establish the whole descriptor: construct the hook, zero the reference and compound words and the body, and write every field of the status word, without reading any of them first. The double-initialisation check then needs a source that is not the field being initialised, or goes. Ruled out: keeping the dependence and documenting it on `activate_section()`; the virtual-flat memmap (`VflatMemMap`) is a second allocation site that would have to remember the same thing.

**Reference**
Linux `__init_single_page()` in `mm/mm_init.c`: `mm_zero_struct_page()` first, then every field it is responsible for, so that the caller's allocation does not need `__GFP_ZERO`.

**Acceptance**
Boot both images with the `Gaf::Zero` removed from the memmap allocation in `SparseMemMap::activate_section()` as a one-off experiment; both must pass `verify`.
