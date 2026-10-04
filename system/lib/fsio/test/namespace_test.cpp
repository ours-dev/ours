/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The namespace table (<fsio/namespace.hpp>): fdio's bind rules (absolute
// paths, no overlap in either direction), unbind, and the lookup open() and
// connect() make, each answering what fdio's namespace answers.
#include <fsio/namespace.hpp>
#include <fsio/path.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {
using fsio::Namespace;
using fsio::PathBuffer;
using ours::Status;
using ours::Uandle;

constexpr auto BOOT = Uandle{0x1003};
constexpr auto SVC = Uandle{0x2003};
constexpr auto OTHER = Uandle{0x3003};

// A namespace a user program keeps as a global must be constant-initialised:
// nothing runs a user program's dynamic initialisers.
constinit Namespace global_namespace;

/// What resolving |path|, cleaned, comes to: "<handle> <rest>", or the
/// status it was refused with.
auto resolved(Namespace const &ns, std::string_view const path) -> std::string {
    PathBuffer clean;
    if (auto const status = fsio::clean_path(path, clean); status != Status::Ok) {
        return ours::to_string(status);
    }
    auto const result = ns.resolve(clean);
    if (result.is_err()) {
        return ours::to_string(result.unwrap_err());
    }
    return std::to_string(static_cast<u32>(result->directory)) + " " + std::string{result->rest};
}

auto at(Uandle const directory, std::string_view const rest) -> std::string {
    return std::to_string(static_cast<u32>(directory)) + " " + std::string{rest};
}

/// A component's namespace: /boot and /svc.
auto component() -> Namespace {
    Namespace ns;
    EXPECT_EQ(ns.bind("/boot", BOOT), Status::Ok);
    EXPECT_EQ(ns.bind("/svc", SVC), Status::Ok);
    return ns;
}

TEST(Namespace, StartsEmpty) {
    EXPECT_EQ(global_namespace.size(), 0U);
    EXPECT_EQ(resolved(global_namespace, "/boot"), "NotFound");
}

// The rest of the path is what Open carries: "." for the mount point
// itself, and never a leading or trailing '/'.
TEST(Namespace, ResolvesTheLongestBoundPrefix) {
    auto const ns = component();
    EXPECT_EQ(ns.size(), 2U);
    EXPECT_EQ(resolved(ns, "/boot"), at(BOOT, "."));
    EXPECT_EQ(resolved(ns, "/boot/bin/x"), at(BOOT, "bin/x"));
    EXPECT_EQ(resolved(ns, "/svc/fuchsia.process.Launcher"), at(SVC, "fuchsia.process.Launcher"));
}

// What the lookup sees is the cleaned path, so a ".." cannot climb out of
// the mount point on the server's side.
TEST(Namespace, ResolvesTheCleanedPath) {
    auto const ns = component();
    EXPECT_EQ(resolved(ns, "//boot/./bin//x/"), at(BOOT, "bin/x"));
    EXPECT_EQ(resolved(ns, "/boot/bin/.."), at(BOOT, "."));
    EXPECT_EQ(resolved(ns, "/boot/../svc/x"), at(SVC, "x"));
    EXPECT_EQ(resolved(ns, "/boot/../../../etc"), "NotFound");
}

// A prefix counts only up to a name boundary: /bootfs is not below /boot.
TEST(Namespace, PrefixesEndAtANameBoundary) {
    auto ns = component();
    EXPECT_EQ(resolved(ns, "/bootfs"), "NotFound");
    EXPECT_EQ(resolved(ns, "/boo"), "NotFound");
    ASSERT_EQ(ns.bind("/bootfs", OTHER), Status::Ok);
    EXPECT_EQ(resolved(ns, "/bootfs/x"), at(OTHER, "x"));
    EXPECT_EQ(resolved(ns, "/boot/x"), at(BOOT, "x"));
}

// fdio_ns_open3() refuses a relative path with NotFound: there is no working
// directory to start from. An empty path cleans to ".", relative too, and a
// buffer nothing was cleaned into holds no path at all.
TEST(Namespace, ResolveRefusesARelativePath) {
    auto const ns = component();
    EXPECT_EQ(resolved(ns, "boot/bin"), "NotFound");
    EXPECT_EQ(resolved(ns, ""), "NotFound");
    EXPECT_EQ(resolved(ns, "../boot"), "NotFound");
    EXPECT_TRUE(ns.resolve(PathBuffer{}).is_err());
}

// A path that is above mount points but is none names one of the nodes
// fdio makes to hold them, and the root is always one: it has nothing to
// connect to, which is NotSupported, not NotFound.
TEST(Namespace, ResolveAboveAMountPointIsNotSupported) {
    auto ns = component();
    EXPECT_EQ(resolved(ns, "/"), "NotSupported");
    EXPECT_EQ(resolved(Namespace{}, "/"), "NotSupported");
    ASSERT_EQ(ns.bind("/data/cache", OTHER), Status::Ok);
    EXPECT_EQ(resolved(ns, "/data"), "NotSupported");
    EXPECT_EQ(resolved(ns, "/data/other"), "NotFound");
    EXPECT_EQ(resolved(ns, "/data/cache/x"), at(OTHER, "x"));
}

TEST(Namespace, ResolveChecksEveryNameLength) {
    auto const ns = component();
    auto const longest = std::string(fuchsia_io::MAX_NAME_LENGTH, 'n');
    EXPECT_EQ(resolved(ns, "/boot/" + longest), at(BOOT, longest));
    EXPECT_EQ(resolved(ns, "/boot/" + longest + "n"), "BadPath");
    EXPECT_EQ(resolved(ns, "/" + longest + "n"), "BadPath");
}

// "/" bound takes every absolute path.
TEST(Namespace, ARootBindingTakesEverything) {
    Namespace ns;
    ASSERT_EQ(ns.bind("/", OTHER), Status::Ok);
    EXPECT_EQ(resolved(ns, "/"), at(OTHER, "."));
    EXPECT_EQ(resolved(ns, "/x/y"), at(OTHER, "x/y"));
}

TEST(Namespace, BindCleansThePath) {
    Namespace ns;
    ASSERT_EQ(ns.bind("//boot/./", BOOT), Status::Ok);
    EXPECT_EQ(resolved(ns, "/boot/x"), at(BOOT, "x"));
    // The same place, spelt differently.
    EXPECT_EQ(ns.bind("/svc/../boot", OTHER), Status::AlreadyExists);
}

// Refusals before the table is looked at, in fdio_ns_bind()'s order, and
// each leaves the table as it was.
TEST(Namespace, BindRefusesBadArguments) {
    auto ns = component();
    EXPECT_EQ(ns.bind(std::string(fsio::MAX_PATH_LENGTH + 1, '/'), OTHER), Status::BadPath);
    EXPECT_EQ(ns.bind(std::string(fsio::MAX_PATH_LENGTH + 1, '/'), Uandle::Invalid),
              Status::BadPath);
    EXPECT_EQ(ns.bind("/data", Uandle::Invalid), Status::BadHandle);
    EXPECT_EQ(ns.bind("data", Uandle::Invalid), Status::BadHandle);
    EXPECT_EQ(ns.bind("data", OTHER), Status::InvArgs);
    EXPECT_EQ(ns.bind("", OTHER), Status::InvArgs);
    EXPECT_EQ(ns.bind("./data", OTHER), Status::InvArgs);

    auto const longest = std::string(fuchsia_io::MAX_NAME_LENGTH, 'n');
    EXPECT_EQ(ns.bind("/data/" + longest + "n", OTHER), Status::BadPath);
    EXPECT_EQ(ns.bind("/data/" + longest, OTHER), Status::Ok);
    EXPECT_EQ(ns.size(), 3U);
}

TEST(Namespace, BindRefusesTheSamePathTwice) {
    auto ns = component();
    EXPECT_EQ(ns.bind("/boot", OTHER), Status::AlreadyExists);
    EXPECT_EQ(resolved(ns, "/boot"), at(BOOT, "."));
}

// Below a mount point is shadowing: part of that directory would be hidden.
TEST(Namespace, BindRefusesShadowing) {
    auto ns = component();
    EXPECT_EQ(ns.bind("/boot/bin", OTHER), Status::NotSupported);
    EXPECT_EQ(ns.bind("/svc/a/b", OTHER), Status::NotSupported);
    EXPECT_EQ(resolved(ns, "/boot/bin/x"), at(BOOT, "bin/x"));

    Namespace rooted;
    ASSERT_EQ(rooted.bind("/", OTHER), Status::Ok);
    EXPECT_EQ(rooted.bind("/boot", BOOT), Status::NotSupported);
    EXPECT_EQ(rooted.bind("/", BOOT), Status::AlreadyExists);
}

// Above a mount point: fdio finds the node it made to hold it, so the path
// exists already; the root is an overlay, refused as such.
TEST(Namespace, BindRefusesAPathAboveAMountPoint) {
    Namespace ns;
    ASSERT_EQ(ns.bind("/data/cache", OTHER), Status::Ok);
    EXPECT_EQ(ns.bind("/data", BOOT), Status::AlreadyExists);
    EXPECT_EQ(ns.bind("/", BOOT), Status::NotSupported);
    EXPECT_EQ(ns.size(), 1U);
}

TEST(Namespace, BindStopsAtMaxBindings) {
    Namespace ns;
    for (auto index = usize{0}; index < fsio::MAX_BINDINGS; ++index) {
        ASSERT_EQ(ns.bind("/" + std::to_string(index), Uandle{static_cast<u32>(0x100 + index)}),
                  Status::Ok);
    }
    EXPECT_EQ(ns.bind("/one-too-many", OTHER), Status::NoResource);
    EXPECT_EQ(ns.size(), fsio::MAX_BINDINGS);
    EXPECT_EQ(resolved(ns, "/one-too-many"), "NotFound");
    EXPECT_EQ(resolved(ns, "/15/x"), at(Uandle{0x10F}, "x"));
}

// An absolute path of exactly |size| bytes (at least 2), |c| repeated, cut
// into names short enough to bind: a first name of 1 to 101 bytes, then
// names of 100, each after its '/'.
auto path_of_size(usize const size, char const c) -> std::string {
    constexpr auto BLOCK = usize{101};
    auto blocks = (size - 1) / BLOCK;
    auto first = (size - 1) % BLOCK;
    if (first == 0) {
        blocks -= 1;
        first = BLOCK;
    }
    auto path = "/" + std::string(first, c);
    for (auto index = usize{0}; index < blocks; ++index) {
        path += "/" + std::string(BLOCK - 1, c);
    }
    return path;
}

// The paths share BOUND_PATHS_CAPACITY bytes: filling it exactly works, and
// one byte more is refused.
TEST(Namespace, BindStopsAtThePathCapacity) {
    Namespace ns;
    auto const first = path_of_size(500, 'a');
    auto const second = path_of_size(300, 'b');
    ASSERT_EQ(first.size(), 500U);
    ASSERT_EQ(path_of_size(203, 'x').size(), 203U);
    ASSERT_EQ(ns.bind(first, BOOT), Status::Ok);
    ASSERT_EQ(ns.bind(second, SVC), Status::Ok);
    auto const rest = fsio::BOUND_PATHS_CAPACITY - first.size() - second.size();

    EXPECT_EQ(ns.bind(path_of_size(rest + 1, 'c'), OTHER), Status::NoResource);
    EXPECT_EQ(ns.size(), 2U);
    EXPECT_EQ(ns.bind(path_of_size(rest, 'c'), OTHER), Status::Ok);
    EXPECT_EQ(ns.bind("/e", OTHER), Status::NoResource);
    EXPECT_EQ(resolved(ns, second + "/x"), at(SVC, "x"));
    EXPECT_EQ(resolved(ns, path_of_size(rest, 'c') + "/x"), at(OTHER, "x"));
}

TEST(Namespace, UnbindHandsTheChannelBack) {
    auto ns = component();
    auto const boot = ns.unbind("/boot");
    ASSERT_TRUE(boot.is_ok());
    EXPECT_EQ(boot.unwrap(), BOOT);
    EXPECT_EQ(ns.size(), 1U);
    EXPECT_EQ(resolved(ns, "/boot/x"), "NotFound");
    EXPECT_EQ(resolved(ns, "/svc/x"), at(SVC, "x"));
    EXPECT_EQ(ns.bind("/boot", OTHER), Status::Ok);
    EXPECT_EQ(resolved(ns, "/boot/x"), at(OTHER, "x"));
}

// Unbinding one in the middle closes its gap: the others keep their paths,
// and the bytes it took are free again.
TEST(Namespace, UnbindInTheMiddleKeepsTheOthers) {
    Namespace ns;
    ASSERT_EQ(ns.bind("/a", BOOT), Status::Ok);
    ASSERT_EQ(ns.bind("/bb", SVC), Status::Ok);
    ASSERT_EQ(ns.bind("/ccc", OTHER), Status::Ok);
    auto const removed = ns.unbind("/bb/");
    ASSERT_TRUE(removed.is_ok());
    EXPECT_EQ(removed.unwrap(), SVC);
    EXPECT_EQ(resolved(ns, "/a/x"), at(BOOT, "x"));
    EXPECT_EQ(resolved(ns, "/bb/x"), "NotFound");
    EXPECT_EQ(resolved(ns, "/ccc/x"), at(OTHER, "x"));
    EXPECT_EQ(ns.bind("/dddd", SVC), Status::Ok);
    EXPECT_EQ(resolved(ns, "/dddd/x"), at(SVC, "x"));
    EXPECT_EQ(resolved(ns, "/ccc"), at(OTHER, "."));

    // What was freed can be bound again, up to the capacity.
    Namespace full;
    auto const big = "/" + std::string(fuchsia_io::MAX_NAME_LENGTH, 'x');
    ASSERT_EQ(full.bind(big, BOOT), Status::Ok);
    ASSERT_EQ(full.bind("/" + std::string(fuchsia_io::MAX_NAME_LENGTH, 'y'), SVC), Status::Ok);
    ASSERT_EQ(full.bind("/" + std::string(fuchsia_io::MAX_NAME_LENGTH, 'z'), OTHER), Status::Ok);
    auto const last = "/" + std::string(fuchsia_io::MAX_NAME_LENGTH - 1, 'w');
    ASSERT_EQ(full.bind(last, Uandle{0x4003}), Status::Ok);
    EXPECT_EQ(full.bind("/v", Uandle{0x5003}), Status::NoResource);
    ASSERT_TRUE(full.unbind(big).is_ok());
    EXPECT_EQ(full.bind("/v", Uandle{0x5003}), Status::Ok);
    EXPECT_EQ(resolved(full, last + "/x"), at(Uandle{0x4003}, "x"));
}

TEST(Namespace, UnbindRefusals) {
    auto ns = component();
    auto const status = [&ns](std::string_view const path) {
        auto const result = ns.unbind(path);
        if (result.is_ok()) {
            return Status::Ok;
        }
        return result.unwrap_err();
    };
    EXPECT_EQ(status(std::string(fsio::MAX_PATH_LENGTH + 1, '/')), Status::BadPath);
    EXPECT_EQ(status("boot"), Status::InvArgs);
    EXPECT_EQ(status("/" + std::string(fuchsia_io::MAX_NAME_LENGTH + 1, 'n')), Status::BadPath);
    // Nothing there, below a mount point, above one, and the root.
    EXPECT_EQ(status("/data"), Status::NotFound);
    EXPECT_EQ(status("/boot/bin"), Status::NotFound);
    EXPECT_EQ(status("/"), Status::NotFound);
    ASSERT_EQ(ns.bind("/data/cache", OTHER), Status::Ok);
    EXPECT_EQ(status("/data"), Status::NotFound);
    EXPECT_EQ(ns.size(), 3U);
}

// With "/" bound, fdio's walk starts at a mount point and refuses to look
// into it: BadPath for anything but "/" itself, which unbinds.
TEST(Namespace, UnbindUnderARootBinding) {
    Namespace ns;
    ASSERT_EQ(ns.bind("/", OTHER), Status::Ok);
    auto const below = ns.unbind("/x");
    ASSERT_TRUE(below.is_err());
    EXPECT_EQ(below.unwrap_err(), Status::BadPath);
    auto const root = ns.unbind("/");
    ASSERT_TRUE(root.is_ok());
    EXPECT_EQ(root.unwrap(), OTHER);
    EXPECT_EQ(ns.size(), 0U);
    EXPECT_EQ(ns.bind("/boot", BOOT), Status::Ok);
}
} // namespace
