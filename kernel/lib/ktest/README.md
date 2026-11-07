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

`filter.hpp`, `format.hpp`, `summary.hpp`, `fixture.hpp`, `instantiate.hpp` and
`environment.hpp` include no kernel header, so the host tests exercise them
without a booted kernel — the fixture and instantiation tests there drive the
real macro expansions; `ktest.hpp`, `assertions.hpp` and `run_state.hpp` are
the kernel-facing half and are compiled only for the kernel toolchain.

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

A suite whose tests share state declares a fixture instead, and each test takes
`KTEST_F`:

```cpp
class MyFixture : public ktest::Fixture {
  public:
    usize ready{};

    void SetUp() override {
        ready = 3;
    }

    void TearDown() override {
        // undo SetUp; runs even after a failed check
    }
};

KTEST_F(MyFixture, ReadsWhatSetUpWrote) {
    EXPECT_EQ(this->ready, 3u);
}
```

The body is a member of a class derived from the fixture, so it reads the
fixture's members through `this`; the fixture must be default constructible and
its members the bodies touch are `protected:` or public, as in GoogleTest. The
object is built in the macro's own static storage — a fresh one per test, SetUp
before the body, TearDown after it however the body ended. `KTEST_SUITE_SETUP(MyFixture)`
and `KTEST_SUITE_TEARDOWN(MyFixture)` run once around all of the suite's tests,
and `ktest::Environment` stages registered through `ktest::add_environment` run
once around the whole run; both print nothing unless they fail. The details and
the failure semantics are in `fixture.hpp` and `environment.hpp`.

`EXPECT_EQ` compares its operands with the language's `==`, so two C string
literals compare as pointers, not characters; compare `std::string_view`s when
the content is what the check is about. `EXPECT_OK` is the spelling for a
`Status`, and prints the status through its `to_string`.

One body run over many values takes `KTEST_VALUE_PARAM` and `KTEST_P`, and one
body over many types takes `KTEST_TYPE_LIST` and `KTEST_T`. Each instantiation
is its own test named `Suite.Name/0`, `Suite.Name/1`, ... — the runner lists,
reports, times and filters them exactly like plain tests, and a filter token
names one instantiation by its suffix:

```cpp
KTEST_VALUE_PARAM(MySuite, int, MyValues, 1, 42, 99);

KTEST_P(MySuite, ReadsItsValue) {
    EXPECT_GT(GetParam(), 0);
}

KTEST_TYPE_LIST(MyTypes, (ByteBox, IntBox));

KTEST_T(MyTypes, SizeIsKnown) {
    EXPECT_GE(sizeof(T), alignof(T));
}
```

The body is a member of a class derived from the suite's param base (a
`ktest::Fixture` for `KTEST_P`), so `GetParam()` is the current table value;
`KTEST_F_P(MyFixture, Name)` composes it with a fixture, and the body sees the
fixture's `SetUp`-built members beside `GetParam()`. The value table and the
type list are constexpr declarations in the test file — nothing is allocated,
and the records are parts of the image. The details and the naming rules are
in `instantiate.hpp`.

## Running

On the dbg image's kernel shell:

```text
ktest                 run every test
ktest all             same
ktest list            list the registry (also `ktest ?`)
ktest <filter>        run the tests whose full name matches the filter
ktest <selection> [name=value ...]   ... with options for the tests that read them
```

A run may carry options after its selection: `name=value` tokens, or a bare
`name` for a presence-only flag. A test reads them through `ktest::option`,
`ktest::option_usize` and `ktest::has_option`, always with a fallback of its
own, so `ktest all` and a bare `ktest <filter>` still run the ordinary
configuration:

```text
ktest Interrupt.* rounds=256
ktest TimerShare.* big
```

`ktest list` takes no options, and a test that does not ask for an option never
sees it. The value points into the console line and is only valid while the
test runs.

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
prebuilt/ninja/ninja -C out/bringup.x86 host/obj/kernel/lib/ktest/test/ktest-test
```

The kernel-side source set is built only by a kernel that links tests; the
dbg image's `vmours.with-tests` is that kernel.
