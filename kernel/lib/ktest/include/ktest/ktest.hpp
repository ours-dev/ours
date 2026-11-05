/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// One kernel test. `KTEST(Suite, Name)` defines the body, the adapter that
// matches the runtime's invoke signature, and the record that puts the test in
// the image; nothing runs at boot and nothing has to be registered by hand.
#ifndef KTEST_KTEST_HPP
#define KTEST_KTEST_HPP 1

#include <ktest/assertions.hpp>

#include <source_location>

#include <gktl/types.hpp>
#include <hsien/test.hpp>

/// Defines one test named `Suite.Name`. The braced body that follows is a
/// plain `void()` function; the suite is a naming prefix and carries no state
/// of its own, which is what lets a test file declare several independent
/// tests without a fixture class.
///
/// The record lands in `.hsien.tests`, the section the runtime's section
/// reader already bounds, so no registration runs and no dynamic initialization
/// is needed. The adapter binds the runtime's Context to the state the
/// assertion macros read; it is one function call around the body.
#define KTEST_INTERNAL_JOIN_(left, right) left##right

#define KTEST_INTERNAL_DEFINE_(suite, name, line)                                                  \
    namespace {                                                                                    \
    void KTEST_INTERNAL_JOIN_(ktest_body_, line)();                                                \
    void KTEST_INTERNAL_JOIN_(ktest_invoke_, line)(::hsien::Context & context) {                   \
        ::ktest::internal::BindScope const scope(context);                                         \
        KTEST_INTERNAL_JOIN_(ktest_body_, line)();                                                 \
    }                                                                                              \
    LINK_SECTION(".hsien.tests")                                                                   \
    FORCE_USED static ::hsien::TestRecord const KTEST_INTERNAL_JOIN_(ktest_record_, line) = {      \
        .name = #suite "." #name,                                                                  \
        .invoke = KTEST_INTERNAL_JOIN_(ktest_invoke_, line),                                       \
        .location = ::std::source_location::current(),                                             \
    };                                                                                             \
    void KTEST_INTERNAL_JOIN_(ktest_body_, line)()                                                 \
    }

#define KTEST(suite, name) KTEST_INTERNAL_DEFINE_(suite, name, __LINE__)

#endif // #ifndef KTEST_KTEST_HPP
