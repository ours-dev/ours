# elfload changes

## Title: elfload says why it refused through a Diagnostics the caller passes, not through logz4 (2026-10-02)

**Files**
- `system/lib/elfload/include/elfload/diagnostics.hpp` (new) — `elfload::Report` (a severity, fixed text, an optional name and an optional number) and `elfload::Diagnostics` (a function pointer and its argument that receives them).
- `system/lib/elfload/include/elfload/program.hpp`, `binder.hpp`, `src/program.cpp`, `src/binder.cpp` — `check_program()`, `Binder::from()` and `Binder::bind()` take a `Diagnostics` first; their 17 `log::` calls become reports.
- `system/lib/elfload/BUILD.gn` — no dependency on logz4.
- `kernel/lib/userabi/src/userboot.cpp` — `log_elfload()`, which writes each report as the log line elfload used to write itself, and the three calls pass it.

**Symptom**
The K23 launcher could not link elfload into a user program. elfload logged through logz4, and logz4 in ring 3 needs `operator new` (there is no user allocator), libc++'s floating-point `to_chars`, `__libcpp_verbose_abort`, `__udivti3` and the `logz4_domains` linker section, all of which only the kernel provides: the shell's link stopped on seventeen undefined symbols, every one referenced from `logz4.log.cpp.o` or `logz4.domain.cpp.o`.

**Cause**
A library that two worlds use cannot decide where its words go: the kernel has a serial log, a user program a debug console, a host test perhaps nothing. Logging from inside the library tied it to the kernel's logger and to everything that logger needs. Zircon's `elfldltl` has the same two users (the kernel's loaders and user-space ones) and solves it by taking a `Diagnostics` object in every call that can fail: the library reports, the caller decides what a report becomes.

Reports are not formatted inside elfload. The text is fixed, and the symbol name and the number that complete it travel beside it, so elfload needs no formatting library at all; the caller formats. The one report that is not an error, where an import was bound, is a Note, so the kernel keeps writing it at info as before.

The user chose this over making logz4 work in ring 3 (an allocator, libc++ support, compiler-rt and a linker section for every user program), and over copying the binder into the launcher without its logging.

**Change**
```cpp
struct Report {
    Severity severity = Severity::Error;
    std::string_view what;
    std::string_view name = {};
    ustl::Option<u64> value = {};
};
struct Diagnostics {
    auto (*report)(void *context, Report const &report) -> void = nullptr;
    void *context = nullptr;
};
// before: log::error("elfload: '{}' is imported but the exporter does not define it", spelling);
diagnostics({.what = "imported, but the exporter does not define it", .name = spelling});
```

**Reference**
Zircon `elfldltl::Diagnostics` and its `FormatError()` (`src/lib/elfldltl/include/lib/elfldltl/diagnostics.h:241`), which every elfldltl entry point takes first.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped).
- The kernel's log is unchanged where it matters: the dbg image writes the same 280 `elfload: bound '<name>' to <address>` lines as before.
- Not exercised by a boot: the error reports, which no image's programs trigger. Their text is the old one with the formatting moved to `log_elfload()`.

**Deliberately not done**
- Making elfload build for the host (its machine is still the compiler's): `docs/repairs/261002/elfload.md` item 1.
- A logz4 for user programs.

**How it was found**
Linking the K23 launcher into the shell.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a library used by both the kernel and a user program not log through the kernel's logger?
- What does Zircon's elfldltl do instead, and what does the caller gain by it?
- Why are the reports not formatted inside elfload?

## Title: The binder reports the two refusals the Diagnostics change left silent (2026-10-02)

**Files**
- `system/lib/elfload/src/binder.cpp` — `Binder::bind()` reports a relocation entry it cannot read, and a relocation whose symbol or name it cannot read, before refusing with InvArgs.

**Symptom**
A program refused for either reason came back as a bare InvArgs, with nothing in the caller's diagnostics. The K23 launcher's header promises that elfload's refusals carry their reason, so a caller saw a silent refusal where every other one says why.

**Cause**
The change that moved elfload's reports into a caller's `Diagnostics` (earlier today) converted every refusal that already logged a line. These two never logged one, so a sweep for log calls did not find them. The rule the change set up is that every refusal says why, and a sweep for `return Status::` would have found them.

**Change**

```cpp
if (entry.is_none()) {
    diagnostics({.what = "a relocation entry cannot be read",
                 .value = ustl::some(static_cast<u64>(index))});
    return Status::InvArgs;
}
...
if (symbol.is_none() || name.is_none()) {
    diagnostics({.what = "a relocation names a symbol that cannot be read",
                 .value = ustl::some(static_cast<u64>(symbol_index))});
    return Status::InvArgs;
}
```

**Reference**
Zircon `elfldltl::RelocateSymbolic()` (`src/lib/elfldltl/include/lib/elfldltl/link.h:175`) reports `"relocation entry symbol table index out of bounds"` through its `Diagnostics` before it fails (`:223`), the counterpart of the second report here.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS, rel PASS.
- A script listed every `return` of a non-Ok status in `binder.cpp` and `program.cpp` and whether a `diagnostics(` call precedes it: these two were the only ones without. The two in `store()` are reported by its caller as "a relocation could not be written".
- Neither path runs on the tree's programs.

**Deliberately not done**
None.

**How it was found**
The review of the K23 launcher: its header said elfload's refusals all go to the diagnostics, and checking that claim found these two.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did converting log lines into reports leave these two silent?
- What sweep finds every refusal, and not only every log line?
- Why does a caller want the reason through the diagnostics when the status is already returned?
