/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The assertion half of the kernel test interface: EXPECT_* records a failed
// check and lets the test continue, ASSERT_* records it and returns from the
// current function, and KTEST_SKIP marks a test this machine cannot exercise.
//
// The macros exist for the same reason the kernel's ASSERT and DEBUG_ASSERT
// do: an assertion is a declarative annotation that needs the call site's
// file, line and expression text, which no function call can capture by
// itself. Everything that can be a function is one.
#ifndef KTEST_ASSERTIONS_HPP
#define KTEST_ASSERTIONS_HPP 1

#include <ktest/format.hpp>
#include <ktest/run_state.hpp>

#include <format>
#include <source_location>
#include <string_view>
#include <utility>

#include <ours/status.hpp>

namespace ktest::internal {

/// Records one failed check. Every assertion path ends here; it owns the
/// run-state lookup, the wrong-thread guard, and the call into the runtime
/// that counts failures and reports them.
inline auto record_failure(std::source_location const location, std::string_view const message)
    -> void {
    auto *const state = current_state();
    if (state == nullptr) {
        framework_error("an assertion ran with no test running");
        return;
    }
    // Ask before touching |state|: it lives on the runner thread's stack, and
    // a worker thread that reads here must be told off, not dereferenced.
    if (!on_test_thread()) {
        framework_error("an assertion ran on a thread that is not running the test");
        return;
    }
    if (state->context == nullptr) {
        framework_error("an assertion ran before the test context was bound");
        return;
    }
    ++state->checks;
    state->context->check(false, location, message.data());
}

/// Records a failed comparison: the expression's text, then both values.
template <typename Expected, typename Actual>
auto record_comparison(std::source_location const location, std::string_view const expression,
                       Expected const &expected, Actual const &actual) -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, expression);
    append(buffer, ": expected ");
    append_value(buffer, expected);
    append(buffer, ", actual ");
    append_value(buffer, actual);
    record_failure(location, finish(buffer));
}

/// As above, with the author's `{}`-formatted message appended.
template <typename Expected, typename Actual, typename... Args>
auto record_comparison(std::source_location const location, std::string_view const expression,
                       Expected const &expected, Actual const &actual,
                       std::format_string<Args...> message, Args &&...args) -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, expression);
    append(buffer, ": expected ");
    append_value(buffer, expected);
    append(buffer, ", actual ");
    append_value(buffer, actual);
    append(buffer, " -- ");
    append_format(buffer, message, std::forward<Args>(args)...);
    record_failure(location, finish(buffer));
}

/// Records a failed condition check, with the prose its macro supplied.
inline auto record_message(std::source_location const location, std::string_view const text)
    -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, text);
    record_failure(location, finish(buffer));
}

/// As above, with the author's `{}`-formatted message appended.
template <typename... Args>
auto record_message(std::source_location const location, std::string_view const text,
                    std::format_string<Args...> message, Args &&...args) -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, text);
    append(buffer, ": ");
    append_format(buffer, message, std::forward<Args>(args)...);
    record_failure(location, finish(buffer));
}

/// Records a failed EXPECT_OK or ASSERT_OK.
inline auto record_status(std::source_location const location, std::string_view const expression,
                          ::ours::Status const status) -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, expression);
    append(buffer, ": expected Status::Ok, actual ");
    append(buffer, ::ours::to_string(status));
    record_failure(location, finish(buffer));
}

/// As above, with the author's `{}`-formatted message appended.
template <typename... Args>
auto record_status(std::source_location const location, std::string_view const expression,
                   ::ours::Status const status, std::format_string<Args...> message, Args &&...args)
    -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, expression);
    append(buffer, ": expected Status::Ok, actual ");
    append(buffer, ::ours::to_string(status));
    append(buffer, " -- ");
    append_format(buffer, message, std::forward<Args>(args)...);
    record_failure(location, finish(buffer));
}

} // namespace ktest::internal

/// Records one failed check and returns the test name and location it
/// belongs to as the runtime sees them. EXPECT_* and ASSERT_* are the two
/// public spellings; both name the check, the values where there are two, and
/// take an optional trailing `{}` format string and arguments.
#define KTEST_RECORD_MESSAGE(...)                                                                  \
    ::ktest::internal::record_message(::std::source_location::current(), __VA_ARGS__)

#define KTEST_EXPECT_TRUE(condition, ...)                                                          \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            KTEST_RECORD_MESSAGE(#condition " is false" __VA_OPT__(, ) __VA_ARGS__);               \
        }                                                                                          \
    } while (false)

#define KTEST_ASSERT_TRUE(condition, ...)                                                          \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            KTEST_RECORD_MESSAGE(#condition " is false" __VA_OPT__(, ) __VA_ARGS__);               \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define KTEST_EXPECT_FALSE(condition, ...)                                                         \
    do {                                                                                           \
        if (condition) {                                                                           \
            KTEST_RECORD_MESSAGE(#condition " is true" __VA_OPT__(, ) __VA_ARGS__);                \
        }                                                                                          \
    } while (false)

#define KTEST_ASSERT_FALSE(condition, ...)                                                         \
    do {                                                                                           \
        if (condition) {                                                                           \
            KTEST_RECORD_MESSAGE(#condition " is true" __VA_OPT__(, ) __VA_ARGS__);                \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define KTEST_EXPECT_NULL(pointer, ...)                                                            \
    do {                                                                                           \
        if ((pointer) != nullptr) {                                                                \
            KTEST_RECORD_MESSAGE(#pointer " is not null" __VA_OPT__(, ) __VA_ARGS__);              \
        }                                                                                          \
    } while (false)

#define KTEST_ASSERT_NULL(pointer, ...)                                                            \
    do {                                                                                           \
        if ((pointer) != nullptr) {                                                                \
            KTEST_RECORD_MESSAGE(#pointer " is not null" __VA_OPT__(, ) __VA_ARGS__);              \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define KTEST_EXPECT_NONNULL(pointer, ...)                                                         \
    do {                                                                                           \
        if ((pointer) == nullptr) {                                                                \
            KTEST_RECORD_MESSAGE(#pointer " is null" __VA_OPT__(, ) __VA_ARGS__);                  \
        }                                                                                          \
    } while (false)

#define KTEST_ASSERT_NONNULL(pointer, ...)                                                         \
    do {                                                                                           \
        if ((pointer) == nullptr) {                                                                \
            KTEST_RECORD_MESSAGE(#pointer " is null" __VA_OPT__(, ) __VA_ARGS__);                  \
            return;                                                                                \
        }                                                                                          \
    } while (false)

/// One comparison body for both families. The operator is a parameter so the
/// six relational checks keep one implementation each, and the symbol names
/// the operator in the message.
// The comparison macros use the language's own operator on the two operands,
// with both bound as `auto const &`. Two C string literals therefore compare
// as pointers; compare std::string_view instead when the content is what the
// check is about.
#define KTEST_COMPARE(operator_token, symbol, expected, actual, ...)                               \
    do {                                                                                           \
        auto const &ktest_expected = (expected);                                                   \
        auto const &ktest_actual = (actual);                                                       \
        if (!(ktest_expected operator_token ktest_actual)) {                                       \
            ::ktest::internal::record_comparison(::std::source_location::current(),                \
                                                 #expected " " symbol " " #actual, ktest_expected, \
                                                 ktest_actual __VA_OPT__(, ) __VA_ARGS__);         \
        }                                                                                          \
    } while (false)

#define KTEST_ASSERT_COMPARE(operator_token, symbol, expected, actual, ...)                        \
    do {                                                                                           \
        auto const &ktest_expected = (expected);                                                   \
        auto const &ktest_actual = (actual);                                                       \
        if (!(ktest_expected operator_token ktest_actual)) {                                       \
            ::ktest::internal::record_comparison(::std::source_location::current(),                \
                                                 #expected " " symbol " " #actual, ktest_expected, \
                                                 ktest_actual __VA_OPT__(, ) __VA_ARGS__);         \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define KTEST_EXPECT_OK(expression, ...)                                                           \
    do {                                                                                           \
        auto const ktest_status = (expression);                                                    \
        if (ktest_status != ::ours::Status::Ok) {                                                  \
            ::ktest::internal::record_status(::std::source_location::current(), #expression,       \
                                             ktest_status __VA_OPT__(, ) __VA_ARGS__);             \
        }                                                                                          \
    } while (false)

#define KTEST_ASSERT_OK(expression, ...)                                                           \
    do {                                                                                           \
        auto const ktest_status = (expression);                                                    \
        if (ktest_status != ::ours::Status::Ok) {                                                  \
            ::ktest::internal::record_status(::std::source_location::current(), #expression,       \
                                             ktest_status __VA_OPT__(, ) __VA_ARGS__);             \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define EXPECT_TRUE(condition, ...) KTEST_EXPECT_TRUE(condition __VA_OPT__(, ) __VA_ARGS__)
#define EXPECT_FALSE(condition, ...) KTEST_EXPECT_FALSE(condition __VA_OPT__(, ) __VA_ARGS__)
#define EXPECT_NULL(pointer, ...) KTEST_EXPECT_NULL(pointer __VA_OPT__(, ) __VA_ARGS__)
#define EXPECT_NONNULL(pointer, ...) KTEST_EXPECT_NONNULL(pointer __VA_OPT__(, ) __VA_ARGS__)
#define EXPECT_OK(expression, ...) KTEST_EXPECT_OK(expression __VA_OPT__(, ) __VA_ARGS__)
#define EXPECT_EQ(...) KTEST_COMPARE(==, "==", __VA_ARGS__)
#define EXPECT_NE(...) KTEST_COMPARE(!=, "!=", __VA_ARGS__)
#define EXPECT_LT(...) KTEST_COMPARE(<, "<", __VA_ARGS__)
#define EXPECT_LE(...) KTEST_COMPARE(<=, "<=", __VA_ARGS__)
#define EXPECT_GT(...) KTEST_COMPARE(>, ">", __VA_ARGS__)
#define EXPECT_GE(...) KTEST_COMPARE(>=, ">=", __VA_ARGS__)

#define ASSERT_TRUE(condition, ...) KTEST_ASSERT_TRUE(condition __VA_OPT__(, ) __VA_ARGS__)
#define ASSERT_FALSE(condition, ...) KTEST_ASSERT_FALSE(condition __VA_OPT__(, ) __VA_ARGS__)
#define ASSERT_NULL(pointer, ...) KTEST_ASSERT_NULL(pointer __VA_OPT__(, ) __VA_ARGS__)
#define ASSERT_NONNULL(pointer, ...) KTEST_ASSERT_NONNULL(pointer __VA_OPT__(, ) __VA_ARGS__)
#define ASSERT_OK(expression, ...) KTEST_ASSERT_OK(expression __VA_OPT__(, ) __VA_ARGS__)
#define ASSERT_EQ(...) KTEST_ASSERT_COMPARE(==, "==", __VA_ARGS__)
#define ASSERT_NE(...) KTEST_ASSERT_COMPARE(!=, "!=", __VA_ARGS__)
#define ASSERT_LT(...) KTEST_ASSERT_COMPARE(<, "<", __VA_ARGS__)
#define ASSERT_LE(...) KTEST_ASSERT_COMPARE(<=, "<=", __VA_ARGS__)
#define ASSERT_GT(...) KTEST_ASSERT_COMPARE(>, ">", __VA_ARGS__)
#define ASSERT_GE(...) KTEST_ASSERT_COMPARE(>=, ">=", __VA_ARGS__)

/// Records a missed opportunity unconditionally. ADD_FAILURE continues;
/// FAIL returns from the test, and is the only way to end one early without a
/// failed check of its own.
#define ADD_FAILURE(...)                                                                           \
    do {                                                                                           \
        KTEST_RECORD_MESSAGE("ADD_FAILURE" __VA_OPT__(, ) __VA_ARGS__);                            \
    } while (false)

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        KTEST_RECORD_MESSAGE("FAIL" __VA_OPT__(, ) __VA_ARGS__);                                   \
        return;                                                                                    \
    } while (false)

/// Ends the current test as skipped. The reason is copied into the run state,
/// so a temporary string is fine; a longer one is truncated.
#define KTEST_SKIP(reason)                                                                         \
    do {                                                                                           \
        ::ktest::internal::record_skip(reason);                                                    \
        return;                                                                                    \
    } while (false)

#endif // #ifndef KTEST_ASSERTIONS_HPP
