# panic-crashlog: build the MVP crash record in four commits

**Date**: 2026-10-04
**Status**: Accepted

## Request
The user asked on 2026-10-04 for repair item 5 to be built now, as an MVP, with the decisions in DESIGN.md: a `kernel.panic.reboot` option (D1 a), an image-declared region (D2 a), and the record logged at boot and handed to userboot as a read-only VMO (D3 b). The scope ends at "basically usable": one panic, one reboot, one report.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D0 to D3 | See DESIGN.md | See DESIGN.md | | 2026-10-04, see DESIGN.md |

## Steps
1. Done (`496cb3ea`). Phys reserves the image-declared region. A new `MemoryType::Crashlog` and a phys option `kernel.crashlog=<size>@<address>`; `MemoryProvisioner::reserve_crashlog()` after the boot image and package; both dev images declare 16 KiB at 64 MiB. Verified by the phys and kernel boot log lines and `ours-verify`.
2. Done. The record format as a library, `kernel/lib/crashlog`, with a host test: write a record into a byte span (payload, CRCs, magic last), read it back, reject a wrong magic, version or CRC, and clear it.
3. Done. The panic writes the record and, with `kernel.panic.reboot`, resets the machine. The next boot validates, logs and clears it. `scripts/qemu_boot.py` gains a way to run without `-no-reboot`, and `kshell_run.py` a check that the second boot logs the recovered record.
4. The recovered record goes to userboot as a read-only "crashlog" VMO, a 7th bootstrap handle, and userboot logs its first line.

## Files
- `kernel/phys/lib/bootmem/include/bootmem/types.hpp`, `kernel/phys/include/ours/phys/boot_options.hpp`, `kernel/phys/boot_options.cpp`, `kernel/phys/include/ours/phys/memory_provisioner.hpp`, `kernel/phys/memory.cpp`, `kernel/phys/physload.cpp`, `kernel/products/image/dev*/BUILD.gn` — step 1
- `kernel/lib/crashlog/` — step 2
- `kernel/top/panic.cpp`, kernel boot options, an arch reset, `scripts/qemu_boot.py`, `scripts/kshell_run.py` — step 3
- `kernel/lib/userabi/`, `system/public/ours/process_args.hpp`, userboot — step 4

## Test plan
- Step 1: the boot log shows `crashlog: reserved [0x4000000, 0x4004000)` from phys and `Reserved 4 frames (16 KiB) of crashlog` from the kernel, on both images.
- Step 2: the host test passes.
- Step 3: on dbg with reboot on, `crash panic` prints its report, the guest resets, and the second boot logs the recovered message under a fixed marker.
- Step 4: the second boot's userboot logs the record's first line from its VMO.
- Every step: `ours-verify` on both images.
