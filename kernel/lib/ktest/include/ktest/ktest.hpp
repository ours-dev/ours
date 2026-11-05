/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// One kernel test. `KTEST(Suite, Name)` defines the body, the adapter that
// matches the runtime's invoke signature, and the record that puts the test in
// the image; nothing runs at boot and nothing has to be registered by hand.
#ifndef KTEST_KTEST_HPP
#define KTEST_KTEST_HPP 1

#include <ktest/assertions.hpp>
#include <ktest/options.hpp>

#include <source_location>

#include <gktl/types.hpp>
#include <hsien/test.hpp>

/// Defines one test named `Suite.Name`. The braced body that follows is a
/// plain `void()` function; the suite is a naming prefix and carries no state
/// of its own, which is what lets a test file declare several independent
/// tests without a fixture class.
///
/// The record lands in `.hsien.tests`, the section the runtime's section
/// reader already bounds, so no registration runs and no dynamic
/// initialization is needed. The adapter binds the runtime's Context to the
/// state the assertion macros read; it is one function call around the body.
#define KTEST_JOIN(left, right) left##right

#define KTEST_DEFINE(suite_token, name_token, line_number)                                         \
    static void KTEST_JOIN(ktest_body_, line_number)();                                            \
    namespace {                                                                                    \
    void KTEST_JOIN(ktest_invoke_, line_number)(::hsien::Context & context) {                      \
        ::ktest::internal::BindScope const scope(context);                                         \
        KTEST_JOIN(ktest_body_, line_number)();                                                    \
    }                                                                                              \
    LINK_SECTION(".hsien.tests")                                                                   \
    FORCE_USED static ::hsien::TestRecord const KTEST_JOIN(ktest_record_, line_number) = {         \
        .name = #suite_token "." #name_token,                                                      \
        .invoke = KTEST_JOIN(ktest_invoke_, line_number),                                          \
        .location = ::std::source_location::current(),                                             \
    };                                                                                             \
    }                                                                                              \
    static void KTEST_JOIN(ktest_body_, line_number)()

#define KTEST(suite, name) KTEST_DEFINE(suite, name, __LINE__)

#endif // #ifndef KTEST_KTEST_HPP
