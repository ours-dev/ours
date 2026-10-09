# profdata

The raw profile of a program's own source-based coverage counters, written without a compiler runtime.

## What it is for

A program built with `-fprofile-instr-generate -fcoverage-mapping` counts in memory the compiler laid out. The compiler's own runtime writes that memory to a file when the program exits; a kernel has no such runtime and no exit. `profdata::RawProfile` does the part that is not I/O: it sizes the profile, writes everything in it that does not change while the program runs, and copies the counters in at the moment the caller picks. The bytes are then in the format `llvm-profdata` reads, and where they go next is the caller's business.

## Using it

```cpp
profdata::RawProfile const profile{build_id};         // sizes only; reads no counter
if (profile.size_bytes() == 0) { return; }             // nothing was instrumented

auto const dump = allocate_aligned(profile.size_bytes(), profdata::RawProfile::ALIGNMENT);
auto const regions = profile.write_fixed(dump);        // header, build ID, fixed data
profile.copy_live(regions);                            // the counters, now
send(dump);
```

`copy_live()` may be called again on the same `regions` for a later snapshot. It reads each counter with a relaxed atomic load, so it can run while other CPUs execute instrumented code; what it copies is each counter at some moment during the call, which is enough for counters that only grow.

A program that links no compiler runtime also needs `//system/lib/profdata:runtime`, which defines the one symbol the instrumented code refers to. A program that links the compiler's runtime must not.

## Layout

    include/profdata/   public header, reached as <profdata/profdata.hpp>
    src/profdata.cpp    RawProfile
    src/runtime.cpp     the runtime marker symbol (the :runtime target)
    test/               host-side tests, built with googletest

## Building and testing

The library is `//system/lib/profdata`; its header alone is `//system/lib/profdata:headers`. The tests are `//system/lib/profdata/test:profdata`, listed in `//:default` through `//system/lib/profdata:tests`. They link a small example that is instrumented in every build variant, so they run in the plain host build and in the coverage variant alike.

To look at what the library wrote with the upstream tools, run the test with `PROFDATA_TEST_DUMP=<file>` set; it writes the profile there, and `llvm-profdata merge`, `llvm-profdata show` and `llvm-cov show` can then be pointed at it and the test binary.

## Supported object formats

ELF, for the kernel, and Mach-O, for the host tests on a Mac. The compiler's section layout and the linker's names for section bounds differ between them; anything else is a compile error.
