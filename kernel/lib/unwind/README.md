# libunwind

A freestanding, allocation-free DWARF CFI unwinder: parses `.eh_frame` and
`.debug_frame` byte streams, indexes FDEs by PC range, and evaluates DW_CFA
programs to step one frame at a time.

## Scope

| Header | Contents |
|---|---|
| `unwind/cursor.hpp` | Bounds-checked byte cursor: ULEB128/SLEB128 + DW_EH_PE pointer encodings |
| `unwind/cfi.hpp` | `Section`, CIE/FDE parsing (`identify_entry`, `parse_cie`, `parse_fde`) |
| `unwind/registers.hpp` | The `Registers{pc, sp, bp}` triple a CFA walk needs |
| `unwind/fde_index.hpp` | `FdeIndex`: caller-owned entry array, binary-search lookup |
| `unwinder.hpp` | `Unwinder::step()` + the `MemoryReader` seam for stack-slot reads |

## Shape

- **Allocation-free core.** `FdeIndex::build` streams entries to a caller
  sink; the kernel backs the array with `ktl::Vec` (`kmalloc`) and host
  tests with `std::vector`. The unwinder itself never allocates, which
  keeps it callable from interrupt context (see libprof).
- **Two section dialects.** `.eh_frame` uses PC-relative encodings and is
  safe to consume wherever the image is loaded; `.debug_frame` uses
  absolute encodings and is only parseable when addresses match the runtime
  layout. The parser handles both plus DWARF v1-v4 CIEs and the `z`/`L`/`P`/`R`
  augmentation family.
- **One quirk worth knowing:** the x86-64 psABI writes the FDE encoding as
  `0x1B` ("pcrel|sdata4"), i.e. format nibble `0x0B`; the cursor treats
  `0x0B` as a 4-byte signed read alongside the spec's `0x07` (sdata4).
- **Unsupported rules bail, not guess.** `def_cfa_expression` /
  `val_expression` / personality-relative and indirect encodings poison the
  step instead of producing a bogus frame; the caller keeps the frames
  unwound so far.
- **Safety bounds.** Every read is section-bounds checked, evaluation runs
  on a 4096-op budget, and a step validates SP monotonicity, alignment and
  a 1 MiB frame-delta cap. Stack-slot reads go through the caller's
  `MemoryReader`, so the interrupt-context user bounds them to the current
  kernel stack.

## Seams

One: `MemoryReader` (`unwinder.hpp`) — the callback that vetoes reads
outside a validated window. The kernel wires it to the interrupted thread's
stack; tests back it with a flat buffer.

## Tests

Hosted gtest (`tests/`, `//kernel/lib/unwind:tests`): hand-crafted CFI byte
streams covering pointer encodings, CIE/FDE parsing, index build/find, and a
simulated two-frame call chain walked end-to-end (14 cases).
