# acpi-route changes

## The edge test maps by the host's page size, not a hard-coded 4 KiB (2026-11-03)

**Files**
- `system/service/acpi-route/test/acpi_route_test.cpp` — `accepted_at_the_edge()` asks the host its page size with `sysconf(_SC_PAGESIZE)` instead of mapping and protecting 4 KiB halves.

**Symptom**
`acpi-route-test` aborted at `acpi_route_test.cpp:283`: `require(mprotect(page + PAGE, PAGE, PROT_NONE) == 0)` failed, on an arm64 Mac (16 KiB pages). The other 153 host tests passed.

**Cause**
The test maps two pages and protects the second, so that a read past the buffer it places at the end of the first page faults instead of reading whatever follows. The page size was hard-coded at 4096, which is the x86 Mac's page size and not the arm64 Mac's: there a page is 16 KiB, so `mmap(nullptr, 2 * 4096, ...)` rounds up to one 16 KiB page, and `page + 4096` is not a page boundary at all. `mprotect` refuses a sub-page range with EINVAL before the test ever reaches the read it means to observe, so the check died of its own setup rather than of what it tests. Found when the branch was booted on the arm64 Mac: the suite had only ever run on a 4 KiB host.

**Change**
The size comes from the host at run time, and the test refuses to run if the host will not say it:

```cpp
// before
constexpr auto PAGE = usize{4096};
auto *const mapping =
    mmap(nullptr, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

// after
auto const host_page = sysconf(_SC_PAGESIZE);
require(host_page > 0);
auto const page_size = static_cast<usize>(host_page);
auto *const mapping = mmap(nullptr, 2 * page_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

The guard is unchanged in shape: the bytes end at the last byte of the first *real* page and the next real page is unmapped, so a read past the end faults whatever the page size is.

**Reference**
None, this project's own design — a host test's portable use of `mmap`/`mprotect`. The mistake itself is the classic one of assuming the page size, which POSIX makes `sysconf(_SC_PAGESIZE)` for exactly this reason.

**Verification**
`boot_check.py`: dbg 45 passed and 4 skipped, rel 42 passed and 8 skipped with 6 of 6 required lines, host tests 154 passed and 0 failed (the suite the failure belonged to). `clang-tidy` on the file reports nothing new on the changed lines.

**Deliberately not done**
- A compile-time page-size constant: the host toolchain builds for whichever Mac it runs on, and the page size is a property of the machine, not of the build.
- Auditing other host tests for the same assumption: none of the others mmaps two pages by hand (`grep -rn "mmap" system/*/test kernel/*/test` finds only this one), so there is nothing to fix elsewhere.

**How it was found**
`boot_check.py` reported `acpi-route-test` exit -6 at line 283; a standalone `mmap`+`mprotect` program of the same shape succeeded five times in a row, so the environment was not the cause; under lldb, at the failing `require`, a fresh `mmap` in the test process and `mprotect` of its second page also failed with errno 22, while `mprotect` of the first page succeeded — which is what "the mapping is one page, not two" looks like, and on an arm64 Mac that is what `mmap(nullptr, 8192, ...)` gives.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did `mprotect` answer EINVAL rather than the read past the end faulting?
- What does `mmap` do to a size that is not a multiple of the page size, and how did that turn a two-page mapping into one page?
- Why must the page size come from `sysconf` at run time rather than a constant in the source?
