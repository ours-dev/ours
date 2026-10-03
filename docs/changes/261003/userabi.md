# userabi changes

## Title: The vDSO has no writable segment (system-abi-surface step 10, part 1) (2026-10-03)

**Files**
- `kernel/lib/userabi/vdso/BUILD.gn` — the vDSO is linked with `-z rodynamic`.

**Symptom**
The vDSO had three loadable segments: read-only (the ELF header, `.dynsym`, the hash tables, `.dynstr`), code (`.text`), and a writable one holding `.dynamic`. The kernel never maps any of it writable, so the third segment's flags described something that never happens. And it stood in the way of the next part, which maps the vDSO by segment and trusts the code segment alone.

**Cause**
The objects:
- `.dynamic`: the table a dynamic linker reads to find a shared object's symbols and relocations, and on most systems writes back into (`DT_DEBUG`, for debuggers). So linkers put it in a writable segment by default.
- The vDSO: a shared object with no dynamic linker of its own and no relocations, mapped from the one VMO every process shares.

The events:
1. lld puts `.dynamic` in a writable `PT_LOAD`, its default, behind `GNU_RELRO`.
2. Nothing ever writes it here: the loader reads the vDSO's symbols from its file, and the mapping is read and execute.
3. The image carried a writable segment that only made its layout harder to reason about: a third page, and a third kind of segment for every mapper to handle.

**Change**
`-z rodynamic` puts `.dynamic` in the read-only segment. The vDSO now has two loadable segments, read-only at `[0, 0x722)` and code at `0x1730`, and its image is two pages instead of three:

```text
LOAD  0x000000 0x0000000000000000 ... 0x000722 R   0x1000
LOAD  0x000730 0x0000000000001730 ... 0x000158 R E 0x1000
DYNAMIC 0x0004f8 0x00000000000004f8 ... R
```

`llvm-readelf -r` finds no relocations, so nothing needs `.dynamic` writable.

**Reference**
Fuchsia's toolchain passes `-z rodynamic` for all user code by default (elfldltl's `svr4-abi.h` says so), and erases it only for the kernel (`zircon/kernel/switch/BUILD.gn`). Zircon's vDSO, as user code, is linked with it.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); the logs read "vdso laid out once, 2 page(s)" and "vdso mapped [A00000, A02000)".
- The vDSO ABI check builds clean: the exports are unchanged.

**Deliberately not done**
- The same flag for every user program, as Fuchsia does: their layout is not what this step is about.
- Mapping the vDSO by segment and trusting only its code: part 2.

**How it was found**
Step 10 of `.agents/tasks/system-abi-surface/DESIGN.md`: mapping by segment needs to know what the segments are, and the writable one had no reason to exist.

**The user's prediction**
None. The user asked for whichever option is the better engineering practice, not the simpler one.

**Three questions you should be able to answer**
- Why do linkers put `.dynamic` in a writable segment by default, and why does the vDSO not need that?
- What did the third page of the vDSO's image hold, and why is it gone?
- What would the kernel have to do with a writable segment when it maps the vDSO by segment?
