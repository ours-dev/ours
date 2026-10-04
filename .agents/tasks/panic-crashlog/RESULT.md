# panic-crashlog: a panic's report survives a warm reset and the next boot reports it

**Date**: 2026-10-04

## Delivered
- `496cb3ea` — phys reserves the image-declared range (`kernel.crashlog=<size>@<address>`) as `MemoryType::Crashlog`; both dev images declare 16 KiB at 64 MiB.
- `18a19986` — `kernel/lib/crashlog`: the record format, magic written last, CRC-checked, with nine host tests.
- `92103f63` — the panic writes the record and writes the cache back; with `kernel.panic.reboot` (on in dev.dbg_1) it resets through 0xCF9, the i8042 and a triple fault; the next boot logs the record and clears it. `qemu_boot.py` gains `REBOOT=1` and `kshell_run.py` `--reboot`.
- `f7e2cd14` — the recovered record goes to userboot as a read-only VMO (`HandleType::VmoCrashlog`), and userboot prints its first line.

## Verification
- `kshell_run.py --out out/kernel-panic --image dbg --reboot "crash panic"`: ok. The report, `--- rebooting ---`, OVMF and GRUB again, then the second boot logs the 1304-byte record and userboot prints `crash: deliberate panic from the kernel shell`.
- The same with `crash panic-smp`: ok.
- `boot_check.py`: dbg and rel PASS at every step.
- The measurement the design asked for first: on QEMU q35 with OVMF and 512 MiB, 16 KiB at 64 MiB survives a warm reset.
- Not measured: anything on real hardware, where the cache write-back matters and QEMU cannot show it.

## Deviations from the design
- The design's Step 0, measuring before building, was folded into step 3. Measuring needed the reservation and a reset anyway, and the first address tried survived.
- The template `vpanic(fmt, args)` now renders and delegates to `vpanic(std::string_view)`, so the panic sequence exists once.

## Found along the way
- Since `ead5d12f` (another session's), the rel image runs the interactive user shell with the kernel shell off, so `crash` cannot be typed on rel; the panic tests run on dbg. Not a defect, but worth knowing before trusting a rel crash test.
- The x86 `arch/x86/cache.hpp` was a TODO placeholder; it now holds `clean_cache_range()`.
- Recorded as repairs in `docs/repairs/261004/kernel-panic.md`: the suspend wait's spin bound, the missing reboot command that would test the clear end to end, and the rel image's lost panic tests.

## Not done
- A third boot reporting nothing, i.e. that the clear sticks across a reset: there is no reboot command to reset without a panic. Covered by the host test only.
- userboot does not pass the record on; no reboot delay like Linux's `panic=<seconds>`; no check that the declared range is RAM. All are listed under "Deliberately not done" in `docs/changes/261004/kernel-panic.md`.
