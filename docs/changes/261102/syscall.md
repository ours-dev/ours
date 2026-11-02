# syscall changes

## clock_get_monotonic returns the steady clock through a trap (2026-11-02)

**Files**
- `system/public/ours/syscalls.gni` — one entry appended, so it takes the next call number; the stub, the declaration and the shim are generated from it
- `kernel/object/include/ours/object/clock.hpp`, `kernel/object/clock.cpp` — `object::monotonic_now()`, the one reading a program and the kernel share
- `kernel/lib/syscall/src/time.cpp` — `syscall::clock_get_monotonic()`, which returns that reading
- `kernel/lib/syscall/include/syscall/handlers.hpp` — the comment that said every handler returns `Status`
- `kernel/lib/syscall/BUILD.gn`, `kernel/object/BUILD.gn` — the new sources
- `kernel/object/include/ours/object/wait.hpp` — a finite deadline is still `NotSupported`, and the comment no longer says a program cannot read the clock
- `system/app/shell/shell_tests.cpp` — `check_clock()`, run first on both images
- `docs/repairs/261005/pc-ps2.md` — item 2's cause no longer says there is no call that reads a clock; the missing piece there is still a sleep

**Symptom**
None, a new feature. No program could read the monotonic clock. `object_wait_one` and `port_wait` therefore accept only `0` and `INSTANT_INFINITE` as a deadline.

**Cause**
None, a new feature. The call was specified as `Instant ours_clock_get_monotonic()` in `docs/design/260923/userspace-interface.md` (section 2.5), returning `SteadyClock::now()` through an object-layer facade. A stored nanosecond in the vDSO was the other shape considered, and it was rejected for this slice: the dev images are one-shot, so nothing ticks that a writer could ride, and a value written on its own timer is stale by up to that period and disagrees with the clock the kernel's timers read. The reading a program gets has to be the reading `SteadyClock::now()` computes.

**Change**
The handler does not name a clock. It asks the object layer, which is the layer a system call is allowed to call:

```cpp
auto clock_get_monotonic() -> Instant {
    return object::monotonic_now();
}

auto monotonic_now() -> Instant {
    return time::SteadyClock::now().time_since_epoch().count();
}
```

`Instant` is signed nanoseconds (`system/public/ours/types.hpp`). The generated shim stores it in `frame->ax`, and the exit path pops the whole `rax`, so the value is not cut to 32 bits. The vDSO entry is the ordinary trap stub. The shell reads it twice, with at most a thousand calls between them, and fails the boot unless the first reading is past the epoch and the second is later.

Ruled out for this change: publishing `Clock`'s anchor into a vDSO data page and computing the time in the vDSO from a counter read. That is how the value stays current without a periodic writer, and it is the next slice. It cannot be this one, because the only enrolled clock source is the HPET and ring 3 cannot read it.

**Reference**
Zircon `sys_clock_get_monotonic_via_kernel()` (`zircon/kernel/lib/syscalls/clock.rs`), which returns `current_mono_time()`. The fast path that does not trap, `compute_monotonic_time()` over `fasttime::internal::TimeValues` (`zircon/kernel/lib/fasttime/include/lib/fasttime/internal/time.h`), is the slice not taken here.

**Verification**
`python3 scripts/format_cpp.py --check` was clean on the touched C++ files, and `python3 scripts/format_gn.py` reported both `BUILD.gn` files already formatted. clang-tidy against `out/clock-mono.x86` reported nothing on the new lines of `clock.cpp`, `time.cpp` and `shell_tests.cpp`. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed: dbg and rel both reached the end with no error line, host tests 151 passed, and both serial logs contain `shell: clock checks passed`.

**Deliberately not done**
The vDSO data page, the published anchor and enrolling an invariant TSC as a `ClockSource`. Ring 3 cannot sample the HPET, and QEMU's TCG does not report an invariant TSC, so the non-trapping read has no counter to sample on the machines this check boots. `deadline_after`, `nanosleep` and a finite deadline on `object_wait_one` are separate calls. A kernel timer that stores a nanosecond into the vDSO was rejected: on a one-shot build it is a new tick, and the stored word lags `SteadyClock::now()`.

**How it was found**
None.

**The user's prediction**
None. The user approved this slice: the trap that returns `SteadyClock::now()`, with the anchor page and the TSC source decided separately.

**Three questions you should be able to answer**
- Why does a nanosecond stored in the vDSO and rewritten on a timer disagree with a deadline the kernel arms from `SteadyClock::now()`?
- Why is the non-trapping read impossible while the steady clock's source is the HPET?
- Why does `syscall::clock_get_monotonic` call `object::monotonic_now()` instead of `SteadyClock::now()`?

## The monotonic anchor is published into the vDSO (2026-11-02)

**Files**
- `system/public/ours/time_values.hpp` — the page layout and `monotonic_from`, the same scale `ClockSource::ticks_to_time` uses
- `kernel/lib/userabi/vdso/vdso.S`, `kernel/lib/userabi/vdso/clock.cpp` — the page in read-only data, and `ours_clock_get_monotonic`, which scales a counter sample when the page says the counter is readable and otherwise traps
- `build/abi/gen_syscalls.py`, `system/public/ours/syscalls.gni` — the `vdso` attribute: the generated stub is the hidden `ours_clock_get_monotonic_via_kernel`
- `kernel/include/ours/time/anchor_page.hpp`, `kernel/kernel/time/anchor_page.cpp` — writes the steady clock's anchor into the page, sequence odd then even
- `kernel/kernel/time/clock.cpp`, `kernel/include/ours/time/clock.hpp`, `kernel/include/ours/time/clock_source.hpp`, `kernel/lib/mtl/include/mtl/ratio.hpp` — the reading that gets published, and the ratio's two halves
- `kernel/mm/vm_object_paged.cpp`, `kernel/mm/include/ours/mm/vm_object.hpp`, `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `store_u64`, one store for the sequence word, because `write()` copies a byte at a time
- `kernel/lib/userabi/src/userboot.cpp` — finds the page by its magic after laying the vDSO out, and binds it
- `kernel/lib/userabi/vdso/BUILD.gn` — the C++ body, and `-z norelro`, because an empty `.got` plus a RELRO pad was a writable segment
- `system/lib/time/test/read_test.cpp` — `monotonic_from` against the kernel scale, including a wrapped counter

**Symptom**
None, a new feature. The call trapped and returned `SteadyClock::now()`. Nothing in the vDSO carried the anchor a later non-trapping read scales.

**Cause**
None, a new feature. A stored nanosecond goes stale between writes, and this kernel has no periodic tick to hang a write on. The anchor is already what `Clock::now` scales, and it changes only when the source changes, so publishing that is what keeps a read in the vDSO equal to a read in the kernel.

**Change**
The page is 48 bytes in the vDSO's read-only data, magic `0x0C10C1A11E000001`, then a sequence, the anchor ticks, the anchor nanoseconds, the ratio, and `accessible`. `lay_out_vdso` scans the VMO for the magic, because the packaged image is stripped. `SteadyClock::notify` and the bind both publish. The publish stores an odd sequence, the other words, then an even sequence. `accessible` is written 0.

```text
read:
  seq1 = load(seq)          # acquire
  if seq1 is odd: retry
  copy the anchor
  seq2 = load(seq)
  if seq1 != seq2: retry
  if accessible == 0: trap to SteadyClock::now()
  else: ns = anchor_ns + scale_up(counter - anchor_ticks, numerator, denominator)
```

A sum that does not fit in an `Instant` saturates at its maximum. The counter subtract is unsigned, so a wrap is a large delta and saturates rather than going backwards.

Ruled out: a C++ definition of the page in the same file as the read. The compiler could then treat `accessible` as the 0 it initialized, and the load would not be a load. The bytes are assembly, and the C++ file only has an extern declaration.

**Reference**
Zircon `compute_monotonic_time` and `VDso::SetMonotonicTicksOffset` (`zircon/kernel/lib/fasttime/include/lib/fasttime/internal/time.h`, `zircon/kernel/lib/userabi/vdso.cc`). Linux `__cvdso_clock_gettime` / `vdso_calc_ns` (`lib/vdso/gettimeofday.c`) is the same shape with a `(mult, shift)` pair, which this clock does not use: `ClockSource::ticks_to_time` scales with a ratio through a 96-bit intermediate so the anchor does not have to be refreshed on a tick.

**Verification**
`system/lib/time/test:read` exits 0. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed: dbg and rel both reached the end, both logs contain `shell: clock checks passed`, host tests 152 passed. clang-tidy on `clock.cpp`, `anchor_page.cpp` and `read_test.cpp` reported nothing on the new lines after the one naming suppression on the assembly symbol. The first boot failed with `userabi: the vDSO has a writable segment at +0x3000`; `-z norelro` removed that segment and the second boot passed.

**Deliberately not done**
`accessible` stays 0, so every read on these images still traps. Turning it on means enrolling an invariant TSC as the steady clock's source, which changes every `now()` in the kernel, and QEMU's TCG does not report one. `deadline_after`, `nanosleep` and a finite deadline on `object_wait_one` are still separate calls.

**How it was found**
None.

**The user's prediction**
The user asked for a timeline variable in the vDSO, loaded volatile. The page that landed is the anchor, not a nanosecond. On the machines this check boots the flag is off and the read traps, which is what `shell: clock checks passed` shows: the value moves, and it moves because of the trap.

**Three questions you should be able to answer**
- Why does the reader retry when the sequence is odd, or when the two loads differ?
- Why would scaling `rdtsc` by an anchor pinned to the HPET not be a time?
- Why is the sequence word stored with `store_u64` rather than `write()`?

## The kernel finds TimeValues from vdso.published (2026-11-02)

**Files**
- `kernel/lib/userabi/vdso/gen_published_table.py` — reads the unstripped vDSO with `llvm-readelf` and writes `name st_value size`
- `kernel/lib/userabi/vdso/BUILD.gn`, `kernel/phys/kernel_elf_image.gni`, `kernel/phys/kernel_elf_binary.gni` — the table is a `package_entries` file named `vdso.published`, next to the stripped `vdso`
- `kernel/lib/userabi/src/userboot.cpp` — `lay_out_vdso` reads that file, subtracts `image_base`, checks the magic, and no longer scans
- `kernel/lib/userabi/vdso/vdso.S` — the comment that said the kernel finds the record by the magic
- `kernel/lib/userabi/vdso/test/gen_published_offsets_test.py` — a fixture whose `st_value` is not the file offset
- `BUILD.gn` — `//:default` names the host test directly

**Symptom**
None, a new feature. `lay_out_vdso` scanned the stripped vDSO for `TIME_VALUES_MAGIC`. A second record would have needed its own magic, and a missing symbol failed at boot rather than at the build.

**Cause**
The packaged vDSO is `llvm-strip --strip-all`. `.symtab` is gone. `.dynsym` remains, and `ours_time_values` is `.hidden`, so it is not in it. The unstripped link output still has the symbol. Compiling the address into the kernel would let a kernel and a vDSO from different builds disagree. The number has to travel with the file it describes.

**Change**
`gen_published_table.py` looks the allow-list up in `.symtab` and refuses a symbol that is not in allocated, non-writable, non-executable data. The kernel package carries the result as `vdso.published`. `lay_out_vdso` parses `ours_time_values 0xf80 56`, computes `st_value - image.image_base`, and checks the magic at that offset:

```text
ours_time_values 0xf80 56
offset = 0xf80 - image_base
read 8 bytes at offset, require TIME_VALUES_MAGIC
bind_anchor_page(..., offset)
```

`find_time_values` is gone. `TimeValues`, `publish_anchor` and the HPET page are unchanged.

**Reference**
Zircon `gen-rodso-code.sh` reads `nm` of the unstripped vDSO and emits `VDSO_CODE_END` style constants (`zircon/kernel/lib/userabi/BUILD.gn`). This tree puts the same fact in the kernel package instead of a compiled header, because the package is what `KernelPackage::file()` already hands `lay_out_vdso`. Linux does not do this: `update_vsyscall()` writes a vvar mapping the kernel already has an address for.

**Verification**
`vdso.published` for this vDSO is `ours_time_values 0xf80 56`. The host test `gen_published_offsets_test` passes: the fixture's `st_value - image_base` is not the file offset, a missing name fails, and a symbol in `.text` fails. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed: dbg and rel both reached the end, and both logs contain `shell: clock checks passed`. Host tests 152 passed.

**Deliberately not done**
ticks-per-second, a boot offset, and a version word. The user allowed them in the same table; they are more lines once a call needs them. A second VMO is not built.

**How it was found**
None.

**The user's prediction**
The user asked why the kernel could not use the symbol table. The packaged file's `.symtab` is removed by strip, and the hidden symbol is not in `.dynsym`. They then chose to carry the address in the kernel package rather than compile it into the kernel.

**Three questions you should be able to answer**
- Why is `ours_time_values` absent from the `.dynsym` the stripped vDSO still has?
- Why is the VMO offset `st_value - image_base` rather than `st_value` or the file offset?
- Why does a magic check remain after the offset is known?

## Three more published words (2026-11-02)

**Files**
- `kernel/lib/userabi/vdso/vdso.S` — `ours_published_ticks_hz`, `ours_published_boot_offset`, `ours_published_version_word`
- `kernel/lib/userabi/vdso/BUILD.gn` — those names on the allow-list
- `kernel/lib/userabi/vdso/clock.cpp` — `ours_ticks_per_second`, `ours_boot_offset`, `ours_published_version`, each one load
- `system/public/ours/syscalls.gni` — the three calls, `vdso` so the body does not trap
- `kernel/kernel/time/anchor_page.cpp`, `kernel/include/ours/time/anchor_page.hpp` — publish the frequency and the boot offset with the anchor
- `kernel/kernel/time/clock.cpp`, `kernel/include/ours/time/clock.hpp` — `frequency_hz` on the reading, `SystemClock::boot_offset`
- `kernel/object/clock.cpp`, `kernel/lib/syscall/src/time.cpp` — the trap bodies, which the vDSO does not call
- `kernel/lib/userabi/src/userboot.cpp` — bind the two offsets, require the version word to be 1
- `system/app/shell/shell_tests.cpp` — the clock check also requires a positive rate, a zero boot offset, and version 1

**Symptom**
None, a new feature. The published page had only the monotonic anchor.

**Cause**
None, a new feature. The user allowed these three records in the same table. Each is one aligned word, so a store does not need the anchor's sequence.

**Change**
`ours_published_ticks_hz` is the steady clock's source frequency, rewritten on the same publish as the anchor. `ours_published_boot_offset` is `SystemClock::boot_offset()`, which is `REAL_BASE` and is 0. `ours_published_version_word` is the constant 1 in the image; boot refuses any other value. The calls are:

```text
ours_ticks_per_second()   -> load ours_published_ticks_hz
ours_boot_offset()        -> load ours_published_boot_offset
ours_published_version()  -> load ours_published_version_word
```

**Reference**
Zircon publishes `ticks_per_second` inside `fasttime::internal::TimeValues` (`zircon/kernel/lib/fasttime/include/lib/fasttime/internal/abi.h`). The boot offset is the amount added to the monotonic clock to get the boot clock (`zx_clock_get_boot`). A version word is this tree's, so a later layout can be told apart from this one.

**Verification**
`python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed. dbg and rel both contain `shell: clock checks passed`, which now also requires the rate to be positive, the boot offset to be 0, and the version to be 1. Host tests 152 passed.

**Deliberately not done**
Nothing writes `REAL_BASE`, so the boot offset stays 0. A second VMO is not built.

**How it was found**
None.

**The user's prediction**
The user said the three records could be added. They are.

**Three questions you should be able to answer**
- Why can these three words be stored without the anchor's odd/even sequence?
- Why is the version word's symbol not named `ours_published_version`?
- What would a non-zero boot offset mean?

## VDSO_EXPORT collects published words into one section (2026-11-02)

**Files**
- `kernel/lib/userabi/vdso/vdso_export.hpp` — `VDSO_EXPORT(type, name, initial)`
- `kernel/lib/userabi/vdso/clock.cpp` — the three words use it
- `kernel/lib/userabi/vdso/vdso.S` — `ours_time_values` moved into `.vdso_data`
- `kernel/lib/userabi/vdso/gen_published_table.py`, `kernel/lib/userabi/vdso/BUILD.gn` — `section:.vdso_data` lists every object symbol in that section
- `kernel/lib/userabi/vdso/test/gen_published_offsets_test.py` — the fixture is in `.vdso_data`; a `.text` symbol is not collected; an empty section fails

**Symptom**
None, a new feature. Each published word was a hand-written `.quad` in `vdso.S`, and the table's allow-list named them one by one.

**Cause**
None, a new feature. The user wanted a mark that the linker gathers into one read-only section, and that section is the page user programs already map.

**Change**
`VDSO_EXPORT` emits the symbol in `.vdso_data` with flags `a`: allocated, not writable, not executable. The vDSO's first `PT_LOAD` stays read-only and now contains `.vdso_data`. The generator takes `section:.vdso_data` and writes every object symbol there, sorted by name. Adding a word is the macro, a kernel store at the offset the table reports, and a function that loads it. The kernel still looks up the names it writes; a name it does not know stays at its image value.

**Reference**
None. The section is this tree's. Linux keeps time data in a vvar mapping rather than a section of the vDSO ELF.

**Verification**
The generated table lists `ours_published_boot_offset`, `ours_published_ticks_hz`, `ours_published_version_word`, and `ours_time_values`. `.vdso_data` is `PROGBITS` with flag `A` inside the read-only `PT_LOAD`. The host test passes. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed: dbg and rel both contain `shell: clock checks passed`.

**Deliberately not done**
A record that is not one 8-byte word. `ours_time_values` is still the hand-written object in the same section, because the macro only emits `.quad`.

**How it was found**
None.

**The user's prediction**
The user asked for a `VDSO_EXPORT` mark that collects variables into one section and exposes that section read-only. That is the macro and `.vdso_data`.

**Three questions you should be able to answer**
- Why is the section flag `a` and not `aw`?
- Why does the generator list every object in `.vdso_data` instead of an allow-list?
- What still has to be written by hand for a new 8-byte word, after the macro?

## The kernel version string is in the vDSO (2026-11-02)

**Files**
- `system/public/ours/version.hpp` — `OURS_KERNEL_VERSION_STRING`, currently `"ours-0.1.0"`
- `kernel/lib/userabi/vdso/vdso_export.hpp` — `VDSO_EXPORT_STRING`
- `kernel/lib/userabi/vdso/clock.cpp` — `ours_kernel_version()` returns that string
- `system/public/ours/syscalls.gni`, `system/public/ours/vdso.hpp`, `build/abi/gen_syscalls.py` — the call, and a pointer result stored with `reinterpret_cast`
- `system/app/shell/shell_tests.cpp` — the clock check requires the vDSO string to match the header

**Symptom**
None, a new feature. There was no kernel version string.

**Cause**
None, a new feature. `ours_published_version_word` is the layout number of the published records, not the kernel's version. The user asked for the version string as well.

**Change**
`VDSO_EXPORT_STRING` puts a NUL-terminated string in `.vdso_data`. `ours_kernel_version()` returns a pointer to it. The text is `OURS_KERNEL_VERSION_STRING` in `<ours/version.hpp>`, so the image and a program that includes the header name the same bytes. The trap stub returns null: the string a program reads is the one in the vDSO mapping.

**Reference**
None.

**Verification**
`python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed. dbg and rel both contain `shell: clock checks passed`, which compares `ours_kernel_version()` to `"ours-0.1.0"`.

**Deliberately not done**
A version that tracks git. The string is the constant in the header.

**How it was found**
None.

**The user's prediction**
The user said the kernel version string should be exposed to user space. It is, through the vDSO.

**Three questions you should be able to answer**
- Why is `ours_published_version_word` not this string?
- Why does the trap stub return null?
- Why is the string defined in a header the vDSO and the shell both include?

## The kernel version string includes the build's short commit (2026-11-02)

**Files**
- `build/version/gen_kernel_version.py` — reads `git rev-parse --short=12 HEAD` and writes the header
- `system/public/BUILD.gn` — the action, output beside the generated vDSO declarations
- `kernel/lib/userabi/vdso/clock.cpp`, `system/app/shell/shell_tests.cpp` — include the generated string
- `system/public/ours/version.hpp` — no longer a fixed string; the pointer type stays

**Symptom**
None, a new feature. The published string was the constant `ours-0.1.0`.

**Cause**
None, a new feature. The user asked for the short commit of the tree that was built, recorded in the same string.

**Change**
`OURS_KERNEL_VERSION_STRING` is `ours-0.1.0+` and 12 hex digits. A dirty work tree appends `-dirty`, because that hash is then the commit the tree started from. The header is generated once, next to `vdso_decls.inc`, and the vDSO and the shell include the same file. A depfile lists git's `HEAD` and `index`, so the next build after a commit or an edit rewrites it.

**Reference**
None.

**Verification**
This build's header is `ours-0.1.0+782aedaed10e-dirty`. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed. dbg and rel both contain `shell: clock checks passed`, which compares `ours_kernel_version()` to that string.

**Deliberately not done**
A version that changes without a rebuild when only the git index changes under a build that does not re-run the action. The depfile covers `HEAD` and `index`.

**How it was found**
None.

**The user's prediction**
The user asked for the short commit hash of the built version to be recorded. It is the suffix of the version string.

**Three questions you should be able to answer**
- Why does a dirty tree append `-dirty` instead of only the hash?
- Why is the header generated in the default toolchain and included from `gen/system/public/public`?
- What does the shell check compare the vDSO string against?
