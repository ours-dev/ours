/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ioservice::Tree (<ioservice/tree.hpp>): how names and paths are judged, children kept
// in name order, and a path walked down one segment at a time.
#include <ioservice/tree.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <ours_io/ours_io.hpp>

namespace {
using ioservice::NodeId;
using ioservice::NodeKind;
using ioservice::Tree;
using ours::Status;

constexpr auto VMO = ours::Uandle{0x2003};

auto file(std::string_view const contents = {}) -> ioservice::VmoFile {
    return ioservice::VmoFile{
        .vmo = VMO,
        .bytes = {reinterpret_cast<u8 const *>(contents.data()), contents.size()},
        .executable = false,
    };
}

/// What |option| holds; a test failure, and T{}, when it holds nothing.
template <typename T>
auto held(ustl::Option<T> const &option) -> T {
    EXPECT_TRUE(option.is_some());
    if (option.is_none()) {
        return T{};
    }
    return *option;
}

/// The children of |directory|, in the order the tree keeps them.
auto children(Tree const &tree, NodeId const directory) -> std::vector<std::string_view> {
    auto names = std::vector<std::string_view>{};
    for (auto child = tree.first_child(directory); child.is_some();
         child = tree.next_sibling(*child)) {
        names.push_back(tree.name(*child));
    }
    return names;
}

TEST(IoserviceName, Valid) {
    for (auto const name: {"a", "bin", "...", ".hidden", "a b", "\xff"}) {
        EXPECT_TRUE(ioservice::is_valid_name(name)) << name;
    }
    EXPECT_TRUE(ioservice::is_valid_name(std::string(ours_io::MAX_NAME_LENGTH, 'n')));
}

TEST(IoserviceName, Invalid) {
    for (auto const name: {"", ".", "..", "a/b", "/"}) {
        EXPECT_FALSE(ioservice::is_valid_name(name)) << name;
    }
    EXPECT_FALSE(ioservice::is_valid_name(std::string_view{"a\0b", 3}));
    EXPECT_FALSE(ioservice::is_valid_name(std::string(ours_io::MAX_NAME_LENGTH + 1, 'n')));
}

TEST(IoservicePath, Valid) {
    for (auto const path: {".", "a", "bin/x", "a/b/c", "a/.../b"}) {
        EXPECT_TRUE(ioservice::is_valid_path(path)) << path;
    }
    // MAX_PATH_LENGTH bytes, every segment a valid name.
    auto longest = std::string{};
    // Room is left for one more byte after the last '/'.
    while (longest.size() + 3 <= ours_io::MAX_PATH_LENGTH) {
        longest += "a/";
    }
    longest += std::string(ours_io::MAX_PATH_LENGTH - longest.size(), 'a');
    ASSERT_EQ(longest.size(), ours_io::MAX_PATH_LENGTH);
    EXPECT_TRUE(ioservice::is_valid_path(longest));
    EXPECT_FALSE(ioservice::is_valid_path(longest + "a"));
}

TEST(IoservicePath, Invalid) {
    // An ours.io path: not empty, no leading or trailing '/', and either "."
    // or valid names. Design D21 holds clients to this canonical form, so a
    // path has one spelling.
    for (auto const path: {"", "/", "/a", "a/", "a//b", "..", "./a", "a/.", "a/../b", "a/./b"}) {
        EXPECT_FALSE(ioservice::is_valid_path(path)) << path;
    }
    EXPECT_FALSE(ioservice::is_valid_path(std::string_view{"a/\0", 3}));
}

TEST(IoserviceTree, ChildrenAreKeptInNameOrder) {
    auto nodes = std::array<ioservice::Node, 8>{};
    auto tree = Tree(nodes);
    auto const root = tree.add_root().unwrap();
    // Added out of order; compared as unsigned bytes, so "Z" < "a" and the
    // high byte sorts last.
    for (auto const name: {"meta", "bin", "\xc3\xa9", "Zeta", "config"}) {
        ASSERT_TRUE(tree.add_directory(root, name).is_ok()) << name;
    }
    EXPECT_EQ(children(tree, root),
              (std::vector<std::string_view>{"Zeta", "bin", "config", "meta", "\xc3\xa9"}));
}

TEST(IoserviceTree, KindsAndWhatTheyHold) {
    auto nodes = std::array<ioservice::Node, 8>{};
    auto tree = Tree(nodes);
    auto const root = tree.add_root().unwrap();
    auto const dir = tree.add_directory(root, "dir").unwrap();
    auto const readme = tree.add_file(root, "readme", file("hello")).unwrap();
    auto const echo = tree.add_service(root, "echo", 42).unwrap();

    EXPECT_EQ(held(tree.kind(root)), NodeKind::Directory);
    EXPECT_EQ(held(tree.kind(dir)), NodeKind::Directory);
    EXPECT_EQ(held(tree.kind(readme)), NodeKind::File);
    EXPECT_EQ(held(tree.kind(echo)), NodeKind::Service);
    EXPECT_TRUE(tree.kind(NodeId{7}).is_none());

    EXPECT_EQ(tree.name(root), "");
    EXPECT_EQ(tree.name(readme), "readme");
    ASSERT_TRUE(tree.file(readme).is_some());
    EXPECT_EQ(tree.file(readme)->vmo, VMO);
    EXPECT_EQ(tree.file(readme)->bytes.size(), 5u);
    EXPECT_TRUE(tree.file(dir).is_none());
    EXPECT_EQ(held(tree.service(echo)), 42u);
    EXPECT_TRUE(tree.service(readme).is_none());
    EXPECT_TRUE(tree.first_child(readme).is_none());
    EXPECT_TRUE(tree.first_child(dir).is_none());
}

TEST(IoserviceTree, AddRefusals) {
    auto nodes = std::array<ioservice::Node, 4>{};
    auto tree = Tree(nodes);
    auto const root = tree.add_root().unwrap();
    auto const readme = tree.add_file(root, "readme", file()).unwrap();

    EXPECT_EQ(tree.add_directory(root, "readme").unwrap_err(), Status::AlreadyExists);
    EXPECT_EQ(tree.add_service(root, "readme", 1).unwrap_err(), Status::AlreadyExists);
    EXPECT_EQ(tree.add_directory(readme, "below").unwrap_err(), Status::NotDir);
    EXPECT_EQ(tree.add_directory(NodeId{3}, "x").unwrap_err(), Status::InvArgs);
    for (auto const name: {"", ".", "..", "a/b"}) {
        EXPECT_EQ(tree.add_directory(root, name).unwrap_err(), Status::InvArgs) << name;
    }

    // A refusal takes no storage: two more nodes still fit, then none.
    EXPECT_TRUE(tree.add_directory(root, "a").is_ok());
    EXPECT_TRUE(tree.add_root().is_ok());
    EXPECT_EQ(tree.add_directory(root, "b").unwrap_err(), Status::NoResource);
    EXPECT_EQ(tree.add_root().unwrap_err(), Status::NoResource);
    EXPECT_EQ(children(tree, root), (std::vector<std::string_view>{"a", "readme"}));
}

TEST(IoserviceTree, AvailableCountsTheRoomLeft) {
    auto nodes = std::array<ioservice::Node, 4>{};
    auto tree = Tree(nodes);
    EXPECT_EQ(tree.available(), 4u);

    auto const root = tree.add_root().unwrap();
    EXPECT_EQ(tree.available(), 3u);
    ASSERT_TRUE(tree.add_directory(root, "a").is_ok());
    ASSERT_TRUE(tree.add_file(root, "b", file()).is_ok());
    EXPECT_EQ(tree.available(), 1u);

    // A refused add takes nothing.
    EXPECT_EQ(tree.add_directory(root, "a").unwrap_err(), Status::AlreadyExists);
    EXPECT_EQ(tree.add_directory(root, "").unwrap_err(), Status::InvArgs);
    EXPECT_EQ(tree.available(), 1u);

    // The last node goes, and then an add is refused for room, which is what
    // available() == 0 says beforehand.
    ASSERT_TRUE(tree.add_service(root, "c", 1).is_ok());
    EXPECT_EQ(tree.available(), 0u);
    EXPECT_EQ(tree.add_directory(root, "d").unwrap_err(), Status::NoResource);
    EXPECT_EQ(tree.available(), 0u);
}

TEST(IoserviceTree, Lookup) {
    auto nodes = std::array<ioservice::Node, 8>{};
    auto tree = Tree(nodes);
    auto const root = tree.add_root().unwrap();
    auto const bin = tree.add_directory(root, "bin").unwrap();
    auto const x = tree.add_file(bin, "x", file()).unwrap();
    auto const echo = tree.add_service(root, "echo", 1).unwrap();

    EXPECT_EQ(tree.lookup(root, ".").unwrap(), root);
    EXPECT_EQ(tree.lookup(root, "bin").unwrap(), bin);
    EXPECT_EQ(tree.lookup(root, "bin/x").unwrap(), x);
    EXPECT_EQ(tree.lookup(bin, "x").unwrap(), x);
    EXPECT_EQ(tree.lookup(root, "echo").unwrap(), echo);

    EXPECT_EQ(tree.lookup(root, "nope").unwrap_err(), Status::NotFound);
    EXPECT_EQ(tree.lookup(root, "bin/nope").unwrap_err(), Status::NotFound);
    EXPECT_EQ(tree.lookup(root, "bin/x/y").unwrap_err(), Status::NotDir);
    EXPECT_EQ(tree.lookup(root, "echo/y").unwrap_err(), Status::NotDir);
    EXPECT_EQ(tree.lookup(x, ".").unwrap_err(), Status::NotDir);
    EXPECT_EQ(tree.lookup(NodeId{6}, ".").unwrap_err(), Status::InvArgs);
    // The path is judged before anything is looked at.
    EXPECT_EQ(tree.lookup(NodeId{6}, "/bin").unwrap_err(), Status::BadPath);
    // No ".." and no leading '/': a walk from |bin| stays under it.
    EXPECT_EQ(tree.lookup(bin, "../echo").unwrap_err(), Status::BadPath);
    EXPECT_EQ(tree.lookup(bin, "/echo").unwrap_err(), Status::BadPath);
    EXPECT_EQ(tree.lookup(bin, "echo").unwrap_err(), Status::NotFound);
}

TEST(IoserviceTree, NodesBelongToTheirTree) {
    auto nodes = std::array<ioservice::Node, 4>{};
    auto tree = Tree(nodes);
    auto const first = tree.add_root().unwrap();
    auto const second = tree.add_root().unwrap();
    ASSERT_TRUE(tree.add_directory(first, "a").is_ok());
    // Two roots, each with its own children: one tree per component's /svc.
    EXPECT_EQ(tree.lookup(second, "a").unwrap_err(), Status::NotFound);
    EXPECT_TRUE(tree.first_child(second).is_none());
}

// --- printable_path ---------------------------------------------------------------

TEST(IoservicePrintablePath, APrintablePathComesBackAsItWas) {
    auto buffer = std::array<char, 32>{};
    EXPECT_EQ(ioservice::printable_path("bin/shell", buffer), "bin/shell");
    EXPECT_EQ(ioservice::printable_path("", buffer), "");
    // The view is of the buffer, not of the path.
    EXPECT_EQ(ioservice::printable_path("a", buffer).data(), buffer.data());
}

TEST(IoservicePrintablePath, EveryByteOutsidePrintableAsciiBecomesAQuestionMark) {
    auto buffer = std::array<char, 32>{};
    EXPECT_EQ(ioservice::printable_path("a\nb\rc\x1b[2J", buffer), "a?b?c?[2J");
    EXPECT_EQ(ioservice::printable_path(std::string_view{"a\0b", 3}, buffer), "a?b");
    EXPECT_EQ(ioservice::printable_path("a\x7f\x80\xffz", buffer), "a???z");
    // The ends of the printable range stay.
    EXPECT_EQ(ioservice::printable_path(" ~", buffer), " ~");
}

TEST(IoservicePrintablePath, ALongPathIsCutToTheBufferAndEndsInEllipsis) {
    auto buffer = std::array<char, 8>{};
    // Exactly the room: nothing is cut.
    EXPECT_EQ(ioservice::printable_path("12345678", buffer), "12345678");
    // One byte over: the last three of the room become "...".
    EXPECT_EQ(ioservice::printable_path("123456789", buffer), "12345...");
    // The cut is applied after the bytes are made printable.
    EXPECT_EQ(ioservice::printable_path("12\n45678901", buffer), "12?45...");
}

TEST(IoservicePrintablePath, ABufferTooSmallForTheEllipsisIsJustCut) {
    auto buffer = std::array<char, 2>{};
    EXPECT_EQ(ioservice::printable_path("abcdef", buffer), "ab");
    auto none = std::array<char, 0>{};
    EXPECT_EQ(ioservice::printable_path("abcdef", none), "");
}
} // namespace
