# ktest

The kernel's test interface: `KTEST(Suite, Name)` declares a test, the
`EXPECT_*`/`ASSERT_*` family checks it, and the `ktest` console command runs a
selection of them on demand. It is a layer over the hsien runtime
(`//system/lib/hsien`), which owns the test records, their linker section, the
per-test context and the failure reporting; this library adds the declaration,
the assertions, the selection grammar and the command.

## Layout

    include/ktest/   public headers, reached as <ktest/...>
    src/             the runner and the `ktest` command
    test/            host-side tests, built with googletest

`filter.hpp`, `format.hpp` and `summary.hpp` include no kernel header, so the
host tests exercise them without a booted kernel; `ktest.hpp`, `assertions.hpp`
and `run_state.hpp` are the kernel-facing half and are compiled only for the
kernel toolchain.

## Writing a test

```cpp
#include <ktest/ktest.hpp>

KTEST(Semaphore, WakesEachWaiter) {
    auto semaphore = MakeSemaphore(0);
    ASSERT_OK(semaphore.Post());
    EXPECT_EQ(semaphore.Count(), 1u, "after post, waits={}", 3);
}
```

A test is a `void()` body; the suite is a naming prefix with no state of its
own. `EXPECT_*` records a failed check and continues, `ASSERT_*` records it and
returns from the current function, `ADD_FAILURE`/`FAIL` record unconditionally,
and `KTEST_SKIP("reason")` marks a test this machine cannot exercise. Assertions
run on the test thread only; a worker thread publishes its result for the test
thread to assert on.

## Running

On the dbg image's kernel shell:

```text
ktest                 run every test
ktest all             same
ktest list            list the registry (also `ktest ?`)
ktest <filter>        run the tests whose full name matches the filter
```

The filter is a `:`-separated list of `*`/`?` globs over `Suite.Name`; a token
beginning with `-` excludes. Each run ends with a `SUMMARY` line
(`ran=`, `passed=`, `failed=`, `skipped=`), which is what the host script
parses; `scripts/kshell_run.py` knows the grammar and turns it into an exit
status.

## Building and testing

The library is `//kernel/lib/ktest`; its headers alone are
`//kernel/lib/ktest:headers`. The host tests are `//kernel/lib/ktest:tests`,
built and run by the host toolchain, not in the kernel:

```text
prebuilt/ninja/ninja -C out/bringup.x86 host/obj/kernel/lib/ktest/ktest-test
```

The kernel-side source set is built only by a kernel that links tests; the
dbg image's `vmours.with-tests` is that kernel.
