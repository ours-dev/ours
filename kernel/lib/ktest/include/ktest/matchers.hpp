/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// EXPECT_THAT(value, matcher): the value-shaped half of the assertion
// family. A matcher is a lightweight callable that answers two questions
// about a value -- does it match, and what did it expect, as text -- and
// nothing else: it carries its expected value in fixed storage, by value,
// so one lives on the stack and none allocates.
//
// The core set is the one the tests use: the six relational factories, the
// two pointer checks, the Status check, and the two combinators over two to
// four children. A custom matcher is any type with a const `matches(actual)
// -> bool` and a const `describe(Buffer&) -> void` -- the factories below
// are written exactly that way, and EXPECT_THAT accepts them beside these.
#ifndef KTEST_MATCHERS_HPP
#define KTEST_MATCHERS_HPP 1

#include <ktest/assertions.hpp>
#include <ktest/format.hpp>

#include <format>
#include <functional>
#include <source_location>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <ours/status.hpp>

namespace ktest::internal {

/// Rendering of one value in a matcher's message. ours::Status has no
/// std::formatter, so it renders through to_string -- the same text an
/// EXPECT_OK failure names -- and everything else through append_value.
template <typename Value>
auto append_matcher_value(Buffer &buffer, Value const &value) -> void {
    if constexpr (std::is_same_v<std::remove_cvref_t<Value>, ::ours::Status>) {
        append(buffer, ::ours::to_string(value));
    } else {
        append_value(buffer, value);
    }
}

/// Records one failed EXPECT_THAT or ASSERT_THAT: the value expression,
/// what the matcher expected, and the value it saw.
template <typename Actual, typename Matcher>
auto record_matcher(std::source_location const location, std::string_view const expression,
                    Actual const &actual, Matcher const &matcher) -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, expression);
    append(buffer, ": expected ");
    matcher.describe(buffer);
    append(buffer, ", actual ");
    append_matcher_value(buffer, actual);
    record_failure(location, finish(buffer));
}

/// As above, with the author's `{}`-formatted message appended.
template <typename Actual, typename Matcher, typename... Args>
auto record_matcher(std::source_location const location, std::string_view const expression,
                    Actual const &actual, Matcher const &matcher,
                    std::format_string<Args...> message, Args &&...args) -> void {
    char storage[MESSAGE_ROOM];
    auto buffer = make_buffer(storage, sizeof(storage));
    append(buffer, expression);
    append(buffer, ": expected ");
    matcher.describe(buffer);
    append(buffer, ", actual ");
    append_matcher_value(buffer, actual);
    append(buffer, " -- ");
    append_format(buffer, message, std::forward<Args>(args)...);
    record_failure(location, finish(buffer));
}

/// The relational matchers' one body: the comparison is a compile-time
/// functor and the phrase names it in the description, so the six factories
/// keep one implementation each -- the operator-as-parameter shape the
/// comparison macros use.
template <typename Expected, typename Compare>
class RelationalMatcher {
  public:
    constexpr RelationalMatcher(Expected const &expected, std::string_view const phrase,
                                Compare const &compare)
        : m_expected(expected), m_phrase(phrase), m_compare(compare) {}

    template <typename Actual>
    [[nodiscard]]
    constexpr auto matches(Actual const &actual) const -> bool {
        return m_compare(actual, m_expected);
    }

    constexpr auto describe(Buffer &buffer) const -> void {
        append(buffer, m_phrase);
        append(buffer, " ");
        append_matcher_value(buffer, m_expected);
    }

  private:
    Expected m_expected;
    std::string_view m_phrase;
    Compare m_compare;
};

/// Matches a null pointer. A value that is not a pointer is an authoring
/// bug the compiler reports at the EXPECT_THAT that wrote it, not silently
/// compared against nullptr.
class IsNullMatcher {
  public:
    template <typename Actual>
    [[nodiscard]]
    constexpr auto matches(Actual const &actual) const -> bool {
        static_assert(std::is_pointer_v<Actual> || std::is_null_pointer_v<Actual>,
                      "IsNull() matches a pointer");
        return actual == nullptr;
    }

    constexpr auto describe(Buffer &buffer) const -> void {
        append(buffer, "a null pointer");
    }
};

/// Matches a pointer that is not null. NotNull(x) is EXPECT_NONNULL's check
/// in matcher form, composable where a bare check is not.
class NotNullMatcher {
  public:
    template <typename Actual>
    [[nodiscard]]
    constexpr auto matches(Actual const &actual) const -> bool {
        static_assert(std::is_pointer_v<Actual> || std::is_null_pointer_v<Actual>,
                      "NotNull() matches a pointer");
        return actual != nullptr;
    }

    constexpr auto describe(Buffer &buffer) const -> void {
        append(buffer, "a non-null pointer");
    }
};

/// Matches ours::Status::Ok -- EXPECT_OK's check in matcher form. The
/// failure names the actual status through to_string, the text a Status
/// renders as; the type has no std::formatter of its own.
class OkMatcher {
  public:
    template <typename Actual>
    [[nodiscard]]
    constexpr auto matches(Actual const &actual) const -> bool {
        return actual == ::ours::Status::Ok;
    }

    constexpr auto describe(Buffer &buffer) const -> void {
        append(buffer, "Status::Ok");
    }
};

/// Matches when every child does. The children are evaluated in order and
/// the first failure ends the check; the description names each child, so a
/// failure shows the whole expectation beside the value that broke one of
/// them. Two to four children: a combinator is for pairing, and an
/// expectation longer than that reads better as two EXPECT_THATs.
template <typename... Matchers>
class AllOfMatcher {
  public:
    static_assert(sizeof...(Matchers) >= 2 && sizeof...(Matchers) <= 4,
                  "AllOf combines two to four matchers");

    explicit constexpr AllOfMatcher(Matchers const &...matchers) : m_matchers(matchers...) {}

    template <typename Actual>
    [[nodiscard]]
    constexpr auto matches(Actual const &actual) const -> bool {
        return matches_all(actual, std::index_sequence_for<Matchers...>{});
    }

    constexpr auto describe(Buffer &buffer) const -> void {
        append(buffer, "all of: ");
        describe_children(buffer, std::index_sequence_for<Matchers...>{});
    }

  private:
    template <typename Actual, usize... Indices>
    [[nodiscard]]
    constexpr auto matches_all(Actual const &actual, std::index_sequence<Indices...>) const
        -> bool {
        return (std::get<Indices>(m_matchers).matches(actual) && ...);
    }

    template <usize... Indices>
    constexpr auto describe_children(Buffer &buffer, std::index_sequence<Indices...>) const
        -> void {
        auto first = true;
        auto const one = [&](auto const &matcher) {
            if (!first) {
                append(buffer, ", ");
            }
            first = false;
            matcher.describe(buffer);
        };
        (one(std::get<Indices>(m_matchers)), ...);
    }

    std::tuple<Matchers...> m_matchers;
};

/// Matches when any child does. The children are evaluated in order and the
/// first match ends the check; the description names each child. Two to
/// four children, as AllOf's are.
template <typename... Matchers>
class AnyOfMatcher {
  public:
    static_assert(sizeof...(Matchers) >= 2 && sizeof...(Matchers) <= 4,
                  "AnyOf combines two to four matchers");

    explicit constexpr AnyOfMatcher(Matchers const &...matchers) : m_matchers(matchers...) {}

    template <typename Actual>
    [[nodiscard]]
    constexpr auto matches(Actual const &actual) const -> bool {
        return matches_any(actual, std::index_sequence_for<Matchers...>{});
    }

    constexpr auto describe(Buffer &buffer) const -> void {
        append(buffer, "any of: ");
        describe_children(buffer, std::index_sequence_for<Matchers...>{});
    }

  private:
    template <typename Actual, usize... Indices>
    [[nodiscard]]
    constexpr auto matches_any(Actual const &actual, std::index_sequence<Indices...>) const
        -> bool {
        return (std::get<Indices>(m_matchers).matches(actual) || ...);
    }

    template <usize... Indices>
    constexpr auto describe_children(Buffer &buffer, std::index_sequence<Indices...>) const
        -> void {
        auto first = true;
        auto const one = [&](auto const &matcher) {
            if (!first) {
                append(buffer, ", ");
            }
            first = false;
            matcher.describe(buffer);
        };
        (one(std::get<Indices>(m_matchers)), ...);
    }

    std::tuple<Matchers...> m_matchers;
};

} // namespace ktest::internal

namespace ktest {

/// Matches when the value equals |expected|. The expected value travels by
/// value, decayed, so a matcher is a small stack object and `Eq(read(i))`
/// cannot dangle; as with EXPECT_EQ, two C strings compare as pointers, and
/// std::string_view is the spelling when the content is the point.
template <typename Expected>
[[nodiscard]]
constexpr auto Eq(Expected &&expected)
    -> internal::RelationalMatcher<std::decay_t<Expected>, std::equal_to<>> {
    return {std::forward<Expected>(expected), "equal to", std::equal_to<>{}};
}

/// Matches when the value does not equal |expected|.
template <typename Expected>
[[nodiscard]]
constexpr auto Ne(Expected &&expected)
    -> internal::RelationalMatcher<std::decay_t<Expected>, std::not_equal_to<>> {
    return {std::forward<Expected>(expected), "not equal to", std::not_equal_to<>{}};
}

/// Matches when the value is less than |expected|.
template <typename Expected>
[[nodiscard]]
constexpr auto Lt(Expected &&expected)
    -> internal::RelationalMatcher<std::decay_t<Expected>, std::less<>> {
    return {std::forward<Expected>(expected), "less than", std::less<>{}};
}

/// Matches when the value is less than or equal to |expected|.
template <typename Expected>
[[nodiscard]]
constexpr auto Le(Expected &&expected)
    -> internal::RelationalMatcher<std::decay_t<Expected>, std::less_equal<>> {
    return {std::forward<Expected>(expected), "less than or equal to", std::less_equal<>{}};
}

/// Matches when the value is greater than |expected|.
template <typename Expected>
[[nodiscard]]
constexpr auto Gt(Expected &&expected)
    -> internal::RelationalMatcher<std::decay_t<Expected>, std::greater<>> {
    return {std::forward<Expected>(expected), "greater than", std::greater<>{}};
}

/// Matches when the value is greater than or equal to |expected|.
template <typename Expected>
[[nodiscard]]
constexpr auto Ge(Expected &&expected)
    -> internal::RelationalMatcher<std::decay_t<Expected>, std::greater_equal<>> {
    return {std::forward<Expected>(expected), "greater than or equal to", std::greater_equal<>{}};
}

/// Matches a null pointer.
[[nodiscard]]
constexpr auto IsNull() -> internal::IsNullMatcher {
    return {};
}

/// Matches a pointer that is not null.
[[nodiscard]]
constexpr auto NotNull() -> internal::NotNullMatcher {
    return {};
}

/// Matches ours::Status::Ok.
[[nodiscard]]
constexpr auto Ok() -> internal::OkMatcher {
    return {};
}

/// Matches when every child matches: `AllOf(Ge(1), Le(9))`.
template <typename M0, typename M1, typename... Rest>
[[nodiscard]]
constexpr auto AllOf(M0 &&first, M1 &&second, Rest &&...rest)
    -> internal::AllOfMatcher<std::decay_t<M0>, std::decay_t<M1>, std::decay_t<Rest>...> {
    return internal::AllOfMatcher<std::decay_t<M0>, std::decay_t<M1>, std::decay_t<Rest>...>{
        std::forward<M0>(first), std::forward<M1>(second), std::forward<Rest>(rest)...};
}

/// Matches when any child matches: `AnyOf(Eq(1), Eq(9))`.
template <typename M0, typename M1, typename... Rest>
[[nodiscard]]
constexpr auto AnyOf(M0 &&first, M1 &&second, Rest &&...rest)
    -> internal::AnyOfMatcher<std::decay_t<M0>, std::decay_t<M1>, std::decay_t<Rest>...> {
    return internal::AnyOfMatcher<std::decay_t<M0>, std::decay_t<M1>, std::decay_t<Rest>...>{
        std::forward<M0>(first), std::forward<M1>(second), std::forward<Rest>(rest)...};
}

} // namespace ktest

/// EXPECT_THAT(value, matcher[, "message {}", args...]) records a failed
/// check when the matcher does not match and lets the test continue;
/// ASSERT_THAT records it and returns from the current function. Both bind
/// the value and the matcher once each -- a matcher argument with side
/// effects is evaluated once -- and the failure carries what the matcher
/// expected beside the value it saw, as EXPECT_EQ's does.
#define KTEST_EXPECT_THAT(value, matcher, ...)                                                     \
    do {                                                                                           \
        auto const &ktest_that_value = (value);                                                    \
        auto const &ktest_that_matcher = (matcher);                                                \
        if (!ktest_that_matcher.matches(ktest_that_value)) {                                       \
            ::ktest::internal::record_matcher(::std::source_location::current(), #value,           \
                                              ktest_that_value,                                    \
                                              ktest_that_matcher __VA_OPT__(, ) __VA_ARGS__);      \
        }                                                                                          \
    } while (false)

#define KTEST_ASSERT_THAT(value, matcher, ...)                                                     \
    do {                                                                                           \
        auto const &ktest_that_value = (value);                                                    \
        auto const &ktest_that_matcher = (matcher);                                                \
        if (!ktest_that_matcher.matches(ktest_that_value)) {                                       \
            ::ktest::internal::record_matcher(::std::source_location::current(), #value,           \
                                              ktest_that_value,                                    \
                                              ktest_that_matcher __VA_OPT__(, ) __VA_ARGS__);      \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define EXPECT_THAT(...) KTEST_EXPECT_THAT(__VA_ARGS__)
#define ASSERT_THAT(...) KTEST_ASSERT_THAT(__VA_ARGS__)

#endif // #ifndef KTEST_MATCHERS_HPP
