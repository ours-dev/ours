/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Path cleaning (<fsio/path.hpp>): fdio's own CleanPath cases
// (sdk/lib/fdio/tests/fdio_cleanpath.cc), each with the output fdio expects,
// then the length limit and the cases fdio's list leaves out.
#include <fsio/path.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {
using fsio::clean_path;
using fsio::PathBuffer;
using ours::Status;

/// |path| cleaned, or a note that cleaning refused it.
auto cleaned(std::string_view const path) -> std::string {
    PathBuffer out;
    if (clean_path(path, out) != Status::Ok) {
        return "<refused>";
    }
    return std::string{out.view()};
}

TEST(CleanPath, Basic) {
    EXPECT_EQ(cleaned("/foo"), "/foo");
    EXPECT_EQ(cleaned("/foo/bar/baz"), "/foo/bar/baz");
    EXPECT_EQ(cleaned("/foo/bar/baz/"), "/foo/bar/baz");
}

TEST(CleanPath, DotDot) {
    EXPECT_EQ(cleaned("/foo/bar/../baz"), "/foo/baz");
    EXPECT_EQ(cleaned("/foo/bar/../baz/.."), "/foo");
    EXPECT_EQ(cleaned("/foo/bar/../baz/../"), "/foo");
    EXPECT_EQ(cleaned("../../.."), "../../..");
    EXPECT_EQ(cleaned("/../../.."), "/");
    EXPECT_EQ(cleaned("/./././../foo"), "/foo");
}

TEST(CleanPath, Dot) {
    EXPECT_EQ(cleaned("/."), "/");
    EXPECT_EQ(cleaned("/./././."), "/");
    EXPECT_EQ(cleaned("/././././"), "/");
    EXPECT_EQ(cleaned("/foobar/././."), "/foobar");
    EXPECT_EQ(cleaned("/foobar/./../././././///."), "/");
    EXPECT_EQ(cleaned("."), ".");
    EXPECT_EQ(cleaned("./."), ".");
    EXPECT_EQ(cleaned("./././../foo"), "../foo");
}

TEST(CleanPath, Minimal) {
    EXPECT_EQ(cleaned(""), ".");
    EXPECT_EQ(cleaned("/"), "/");
    EXPECT_EQ(cleaned("//"), "/");
    EXPECT_EQ(cleaned("///"), "/");
    EXPECT_EQ(cleaned("a"), "a");
    EXPECT_EQ(cleaned("a/"), "a");
    EXPECT_EQ(cleaned("a/."), "a");
    EXPECT_EQ(cleaned("a/.."), ".");
    EXPECT_EQ(cleaned("a/../."), ".");
    EXPECT_EQ(cleaned("/a/../."), "/");
    EXPECT_EQ(cleaned(".."), "..");
    EXPECT_EQ(cleaned("..."), "...");
}

// Names that only start like "." or ".." are names, kept whole.
TEST(CleanPath, NamesThatStartWithDots) {
    EXPECT_EQ(cleaned("/boot/.hidden"), "/boot/.hidden");
    EXPECT_EQ(cleaned("/boot/..x/y"), "/boot/..x/y");
    EXPECT_EQ(cleaned("/boot/x../y"), "/boot/x../y");
    EXPECT_EQ(cleaned("/a/.../.."), "/a");
}

// A ".." never climbs above where cleaning can see: the root of a rooted
// path, or the ".." names a relative path starts with.
TEST(CleanPath, DotDotStopsAtTheRootOrAtLeadingDotDots) {
    EXPECT_EQ(cleaned("/boot/../../svc"), "/svc");
    EXPECT_EQ(cleaned("a/b/../../.."), "..");
    EXPECT_EQ(cleaned("../a/../.."), "../..");
    EXPECT_EQ(cleaned("../../a/b/.."), "../../a");
    EXPECT_EQ(cleaned("a/../../b"), "../b");
}

TEST(CleanPath, TheShapesAComponentPathTakes) {
    EXPECT_EQ(cleaned("/boot//bin/./x/../y/"), "/boot/bin/y");
    EXPECT_EQ(cleaned("/svc/fuchsia.process.Launcher"), "/svc/fuchsia.process.Launcher");
    EXPECT_EQ(cleaned("//svc//"), "/svc");
}

// The limit is on the path given, as fdio's is: MAX_PATH_LENGTH bytes clean,
// one more is refused, and the buffer is then empty.
TEST(CleanPath, LengthLimit) {
    auto longest = std::string(fsio::MAX_PATH_LENGTH, 'a');
    longest[0] = '/';
    EXPECT_EQ(cleaned(longest), longest);

    // Its cleaned form is shorter, but the path given is too long.
    auto const too_long = longest + "/";
    PathBuffer out;
    ASSERT_EQ(clean_path("/boot", out), Status::Ok);
    EXPECT_EQ(clean_path(too_long, out), Status::BadPath);
    EXPECT_TRUE(out.view().empty());
}

// Cleaning never makes a path longer than it was, so a buffer of
// MAX_PATH_LENGTH holds every result: the worst cases keep every byte.
TEST(CleanPath, NeverLonger) {
    auto const dotdots = [] {
        auto path = std::string{".."};
        while (path.size() + 3 <= fsio::MAX_PATH_LENGTH) {
            path += "/..";
        }
        return path;
    }();
    EXPECT_EQ(cleaned(dotdots), dotdots);

    auto const names = [] {
        auto path = std::string{};
        while (path.size() + 2 <= fsio::MAX_PATH_LENGTH) {
            path += "/a";
        }
        return path;
    }();
    EXPECT_EQ(cleaned(names), names);
}

// A buffer holds one path: cleaning into it again replaces what was there.
TEST(CleanPath, ReusesTheBuffer) {
    PathBuffer out;
    ASSERT_EQ(clean_path("/a/very/long/path/indeed", out), Status::Ok);
    ASSERT_EQ(clean_path("/b", out), Status::Ok);
    EXPECT_EQ(out.view(), "/b");
}

// constexpr-constructible, so that a PathBuffer can be a constinit global.
TEST(CleanPath, AnUnwrittenBufferIsEmpty) {
    constexpr PathBuffer UNWRITTEN;
    EXPECT_TRUE(UNWRITTEN.view().empty());
}
} // namespace
