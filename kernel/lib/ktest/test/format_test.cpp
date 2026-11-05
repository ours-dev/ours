/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ktest/format.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

struct Unprintable {};

TEST(KtestFormat, RendersValues) {
    char storage[64];
    auto buffer = ktest::internal::make_buffer(storage, sizeof(storage));
    ktest::internal::append_value(buffer, 42);
    ktest::internal::append(buffer, " ");
    ktest::internal::append_value(buffer, std::string_view{"ok"});
    EXPECT_EQ(ktest::internal::finish(buffer), "42 ok");
}

TEST(KtestFormat, FormatsAuthorMessages) {
    char storage[64];
    auto buffer = ktest::internal::make_buffer(storage, sizeof(storage));
    ktest::internal::append_format(buffer, "{} + {} = {}", 1, 2, 3);
    EXPECT_EQ(ktest::internal::finish(buffer), "1 + 2 = 3");
}

TEST(KtestFormat, MarksUnprintableTypes) {
    char storage[64];
    auto buffer = ktest::internal::make_buffer(storage, sizeof(storage));
    ktest::internal::append_value(buffer, Unprintable{});
    EXPECT_EQ(ktest::internal::finish(buffer), "<unprintable>");
}

TEST(KtestFormat, CutsLongValuesShort) {
    char storage[32];
    auto buffer = ktest::internal::make_buffer(storage, sizeof(storage));
    ktest::internal::append_value(buffer, std::string(100, 'x'));
    auto const rendered = ktest::internal::finish(buffer);
    EXPECT_LE(rendered.size(), static_cast<usize>(sizeof(storage) - 1));
    EXPECT_TRUE(rendered.ends_with("..."));
}

TEST(KtestFormat, KeepsTheMessageNulTerminated) {
    char storage[16];
    auto buffer = ktest::internal::make_buffer(storage, sizeof(storage));
    ktest::internal::append(buffer, "short");
    auto const rendered = ktest::internal::finish(buffer);
    EXPECT_EQ(rendered.data()[rendered.size()], '\0');
}

} // namespace
