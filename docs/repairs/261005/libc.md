# libc outstanding repairs

## 1. The kernel's memset and memcpy are byte loops, so every zeroing costs a store per byte

**Severity**
Low: correct, but slow, and it sets the price of every value-initialization in the kernel.

**Symptom**
`ours::libc::memset` in vmours is an eight-way unrolled loop of `movb` stores: 4096 stores to zero 4 KiB, where the user copy that follows a message's zeroing moves the same 4 KiB with one `rep movsq`, 512 quadword stores.

**Cause**
The kernel is built with `-mno-sse` and libc with `-fno-builtin`, so the generic C loops in `system/lib/libc/src/string_impl.cpp` are what run. They are `WEAK_LINK` in `string.cpp`, a hook the design keeps for an architecture's faster definitions, and none is provided.

**Change**
Strong x86 definitions in an arch assembly file: `rep stosq` with a `rep stosb` tail for memset, `rep movsq` for memcpy (Nehalem, the default CPU model, has no ERMS). DF is already clear on entry (IA32_FMASK). Measure first, as the user decides: a one-thread user program writing and reading one channel in a loop at 0, 64, 256, 1024 and 4096 bytes, with 0 and 1 handles, timed in batches of about 1000 round trips with rdtsc (the guest TSC moves in 1 us steps under TCG), five boots per build, alternating builds, comparing the build before 6d805b4d, HEAD, and HEAD with the payload left unzeroed (a measurement-only patch). The 0-byte cell is the control. Add a build with the fast memset only if the zeroing shows.

**Reference**
Zircon `kernel/lib/libc/string/arch/x86/memset-stosq.S`, chosen by code patching.

**Acceptance**
A host or kernel test of the tail and the return value passes, and the channel measurement shows the effect.
