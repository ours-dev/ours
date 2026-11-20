/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// One kernel test. `KTEST(Suite, Name)` defines the body, the adapter that
// matches the runtime's invoke signature, and the record that puts the test in
// the image; nothing runs at boot and nothing has to be registered by hand.
#ifndef KTEST_KTEST_HPP
#define KTEST_KTEST_HPP 1

#include <ktest/assertions.hpp>
#include <ktest/environment.hpp>
#include <ktest/fixture.hpp>
#include <ktest/instantiate.hpp>
#include <ktest/matchers.hpp>
#include <ktest/options.hpp>

#include <source_location>

#include <gktl/types.hpp>
#include <hsien/test.hpp>

/// Defines one test named `Suite.Name`. The braced body that follows is a
/// plain `void()` function; the suite is a naming prefix and carries no state
/// of its own, which is what lets a test file declare several independent
/// tests without a fixture class. A suite whose tests share state takes a
/// fixture instead: `KTEST_F` and the suite hooks live in fixture.hpp, which
/// this header includes.
///
/// The record lands in `.hsien.tests`, the section the runtime's section
/// reader already bounds, so no registration runs and no dynamic
/// initialization is needed. The adapter binds the runtime's Context to the
/// state the assertion macros read; it is one function call around the body.
#define KTEST_JOIN(left, right) left##right

#define KTEST_DEFINE(suite_token, name_token, line_number)                                         \
    class suite_token##_##name_token##_Test {                                                      \
      public:                                                                                      \
        static auto invoke(::hsien::Context &context) -> void {                                    \
            ::ktest::internal::BindScope const scope(context);                                     \
            body();                                                                                \
        }                                                                                          \
        static void body();                                                                        \
        LINK_SECTION(".hsien.tests")                                                               \
        FORCE_USED static inline ::hsien::TestRecord const record = {                              \
            .name = #suite_token "." #name_token,                                                  \
            .invoke = &invoke,                                                                     \
            .location = ::std::source_location::current(),                                         \
        };                                                                                         \
    };                                                                                             \
    void suite_token##_##name_token##_Test::body()

#define KTEST(suite, name) KTEST_DEFINE(suite, name, __LINE__)

/// Defines one death test named `Suite.Name`: a body expected to panic the
/// kernel, which is the verdict the host judges. The record is the same
/// `hsien::TestRecord` a `KTEST` declares, but it lands in a second section,
/// `.ktest.death`, that the live registry never walks — so `ktest all` and a
/// filter run never run one, and a normal run's SUMMARY does not count them.
/// `ktest death <name>` runs the ones its filter selects, and the first panic
/// ends the machine, which is why the host drives one death test per fresh
/// boot and reads the list off `ktest list`'s `ktest: DEATH` lines.
///
/// The expansion is the `KTEST` one: a static body, an adapter that binds the
/// runtime's Context, and a record. Assertions inside are pointless but
/// harmless — the body's whole report is the panic — and a body that returns
/// without panicking is reported FAILED by the runner, since a death test
/// that leaves the machine alive is what failed.
#define KTEST_DEATH(suite, name) KTEST_DEATH_DEFINE(suite, name, __LINE__)

#define KTEST_DEATH_DEFINE(suite_token, name_token, line_number)                                   \
    class suite_token##_##name_token##_DeathTest {                                                 \
      public:                                                                                      \
        static auto invoke(::hsien::Context &context) -> void {                                    \
            ::ktest::internal::BindScope const scope(context);                                     \
            body();                                                                                \
        }                                                                                          \
        static void body();                                                                        \
        KTEST_RECORD_SECTION(".ktest.death")                                                       \
        FORCE_USED static inline ::hsien::TestRecord const record = {                              \
            .name = #suite_token "." #name_token,                                                  \
            .invoke = &invoke,                                                                     \
            .location = ::std::source_location::current(),                                         \
        };                                                                                         \
    };                                                                                             \
    void suite_token##_##name_token##_DeathTest::body()

#endif // #ifndef KTEST_KTEST_HPP
