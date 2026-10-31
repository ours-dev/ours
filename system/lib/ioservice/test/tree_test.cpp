/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ioservice::Tree (<ioservice/tree.hpp>): how names and paths are judged, children kept
// in name order, and a path walked down one segment at a time.
#include <ioservice/tree.hpp>

#include <array>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
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

// --- Subtree nodes -------------------------------------------------------------------

/// The tree the Subtree tests walk:
///
///   /                     root
///     export/             a directory
///       data              a Subtree node, DATA_VALUE
///       docs/             a directory
///         readme          a file
///         mnt             a Subtree node, MNT_VALUE
///       stop              a service
///     other               a Subtree node, OTHER_VALUE
class IoserviceSubtree: public testing::Test {
  protected:
    /// The status |result| was refused with, or Ok when it was not.
    /// unwrap_err() would trap on a call that succeeded, which ends the whole
    /// run in place of failing the one test that expected a refusal.
    template <typename T>
    static auto refusal(ustl::Result<T, Status> const &result) -> Status {
        if (result.is_ok()) {
            return Status::Ok;
        }
        return result.unwrap_err();
    }

    /// The node |result| holds; a test failure, and the root's id, when it
    /// holds none.
    template <typename T>
    static auto node_of(ustl::Result<T, Status> const &result) -> NodeId {
        EXPECT_TRUE(result.is_ok());
        if (result.is_err()) {
            return NodeId{};
        }
        if constexpr (std::is_same_v<T, NodeId>) {
            return result.unwrap();
        } else {
            return result.unwrap().node;
        }
    }

    static constexpr auto DATA_VALUE = u64{0};
    static constexpr auto MNT_VALUE = u64{0x1'0000'0002};
    static constexpr auto OTHER_VALUE = u64{9};

    IoserviceSubtree() {
        m_root = m_tree.add_root().unwrap();
        m_export = m_tree.add_directory(m_root, "export").unwrap();
        m_data = m_tree.add_subtree(m_export, "data", DATA_VALUE).unwrap();
        m_docs = m_tree.add_directory(m_export, "docs").unwrap();
        m_readme = m_tree.add_file(m_docs, "readme", file("hi")).unwrap();
        m_mnt = m_tree.add_subtree(m_docs, "mnt", MNT_VALUE).unwrap();
        m_stop = m_tree.add_service(m_export, "stop", 3).unwrap();
        m_other = m_tree.add_subtree(m_root, "other", OTHER_VALUE).unwrap();
    }

    std::array<ioservice::Node, 12> m_nodes{};
    Tree m_tree{m_nodes};
    NodeId m_root{};
    NodeId m_export{};
    NodeId m_data{};
    NodeId m_docs{};
    NodeId m_readme{};
    NodeId m_mnt{};
    NodeId m_stop{};
    NodeId m_other{};
};

TEST_F(IoserviceSubtree, ASubtreeNodeIsAKindOfItsOwnThatHoldsAValue) {
    EXPECT_EQ(held(m_tree.kind(m_data)), NodeKind::Subtree);
    EXPECT_EQ(m_tree.name(m_data), "data");
    // The value is what the program chose, zero and a wide one included.
    EXPECT_EQ(held(m_tree.subtree(m_data)), DATA_VALUE);
    EXPECT_EQ(held(m_tree.subtree(m_mnt)), MNT_VALUE);
    EXPECT_EQ(held(m_tree.subtree(m_other)), OTHER_VALUE);

    // It is not a service, a file or a directory, and they are not it.
    EXPECT_TRUE(m_tree.service(m_data).is_none());
    EXPECT_TRUE(m_tree.file(m_data).is_none());
    EXPECT_TRUE(m_tree.first_child(m_data).is_none());
    EXPECT_TRUE(m_tree.subtree(m_stop).is_none());
    EXPECT_TRUE(m_tree.subtree(m_readme).is_none());
    EXPECT_TRUE(m_tree.subtree(m_export).is_none());
    EXPECT_TRUE(m_tree.subtree(NodeId{11}).is_none());
    EXPECT_EQ(held(m_tree.service(m_stop)), 3u);
}

TEST_F(IoserviceSubtree, ASubtreeNodeIsListedWithItsSiblingsInNameOrder) {
    EXPECT_EQ(children(m_tree, m_export), (std::vector<std::string_view>{"data", "docs", "stop"}));
    EXPECT_EQ(children(m_tree, m_root), (std::vector<std::string_view>{"export", "other"}));
}

TEST_F(IoserviceSubtree, NothingIsAddedBelowASubtreeNode) {
    EXPECT_EQ(m_tree.add_directory(m_data, "x").unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.add_file(m_data, "x", file()).unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.add_service(m_data, "x", 1).unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.add_subtree(m_data, "x", 1).unwrap_err(), Status::NotDir);
}

TEST_F(IoserviceSubtree, AddSubtreeIsRefusedAsTheOtherAddersAre) {
    EXPECT_EQ(m_tree.add_subtree(m_export, "data", 1).unwrap_err(), Status::AlreadyExists);
    EXPECT_EQ(m_tree.add_subtree(m_export, "docs", 1).unwrap_err(), Status::AlreadyExists);
    EXPECT_EQ(m_tree.add_subtree(m_export, "stop", 1).unwrap_err(), Status::AlreadyExists);
    EXPECT_EQ(m_tree.add_subtree(m_readme, "x", 1).unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.add_subtree(NodeId{11}, "x", 1).unwrap_err(), Status::InvArgs);
    for (auto const name: {"", ".", "..", "a/b"}) {
        EXPECT_EQ(m_tree.add_subtree(m_export, name, 1).unwrap_err(), Status::InvArgs) << name;
    }

    // Refusals take no storage: the four nodes left are all there.
    EXPECT_EQ(m_tree.available(), 4u);
    for (auto const name: {"a", "b", "c", "d"}) {
        EXPECT_TRUE(m_tree.add_subtree(m_export, name, 1).is_ok()) << name;
    }
    EXPECT_EQ(m_tree.add_subtree(m_export, "e", 1).unwrap_err(), Status::NoResource);
}

TEST_F(IoserviceSubtree, ResolveStopsAtASubtreeNodeAndReturnsTheRestOfThePath) {
    // The path ends on the node: nothing is left, and "." says so, so that
    // the program can hand the rest on as the path of an Open.
    auto const itself = m_tree.resolve(m_root, "export/data");
    ASSERT_TRUE(itself.is_ok());
    EXPECT_EQ(itself->node, m_data);
    EXPECT_EQ(itself->rest, ".");

    // One segment after it, and several.
    auto const one = m_tree.resolve(m_root, "export/data/x");
    ASSERT_TRUE(one.is_ok());
    EXPECT_EQ(one->node, m_data);
    EXPECT_EQ(one->rest, "x");
    auto const several = m_tree.resolve(m_root, "export/data/sub/dir/file.txt");
    ASSERT_TRUE(several.is_ok());
    EXPECT_EQ(several->node, m_data);
    EXPECT_EQ(several->rest, "sub/dir/file.txt");

    // From a directory further down, and the node at the first segment.
    auto const below = m_tree.resolve(m_export, "docs/mnt/a/b");
    ASSERT_TRUE(below.is_ok());
    EXPECT_EQ(below->node, m_mnt);
    EXPECT_EQ(below->rest, "a/b");
    auto const first = m_tree.resolve(m_root, "other/q");
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first->node, m_other);
    EXPECT_EQ(first->rest, "q");
    auto const from_docs = m_tree.resolve(m_docs, "mnt");
    ASSERT_TRUE(from_docs.is_ok());
    EXPECT_EQ(from_docs->node, m_mnt);
    EXPECT_EQ(from_docs->rest, ".");
}

TEST_F(IoserviceSubtree, TheRestIsAViewOfThePathItCameFrom) {
    // Not a copy and not joined from segments: the tail of the same bytes, so
    // that resolving costs no storage.
    auto const path = std::string_view{"export/data/sub/dir"};
    auto const resolved = m_tree.resolve(m_root, path);
    ASSERT_TRUE(resolved.is_ok());
    EXPECT_EQ(resolved->rest.data(), path.data() + std::string_view{"export/data/"}.size());
    EXPECT_EQ(resolved->rest.size(), std::string_view{"sub/dir"}.size());
}

TEST_F(IoserviceSubtree, NoSegmentAfterASubtreeNodeIsLookedAt) {
    // Names a tree of this size has no node for: they are the program's.
    for (auto const path: {"export/data/nope", "export/data/a/b/c", "export/data/x/y/z/w"}) {
        auto const resolved = m_tree.resolve(m_root, path);
        ASSERT_TRUE(resolved.is_ok()) << path;
        EXPECT_EQ(resolved->node, m_data) << path;
    }
    // The same names under a directory are not there.
    EXPECT_EQ(m_tree.resolve(m_root, "export/docs/nope").unwrap_err(), Status::NotFound);
}

TEST_F(IoserviceSubtree, ResolveGivesEachSubtreeNodeItsOwnRest) {
    // Two Subtree nodes side by side: each walk stops at its own.
    EXPECT_EQ(node_of(m_tree.resolve(m_root, "export/data/a")), m_data);
    EXPECT_EQ(node_of(m_tree.resolve(m_root, "other/b")), m_other);
    EXPECT_EQ(node_of(m_tree.resolve(m_root, "export/docs/mnt/c")), m_mnt);
    EXPECT_EQ(held(m_tree.subtree(node_of(m_tree.resolve(m_root, "other/b")))), OTHER_VALUE);
}

TEST_F(IoserviceSubtree, AWalkThatMeetsNoSubtreeNodeEndsWhereLookupDoesWithNothingLeft) {
    for (auto const [directory, path, node]: {
             std::tuple{m_root, ".", m_root},
             std::tuple{m_root, "export", m_export},
             std::tuple{m_root, "export/docs", m_docs},
             std::tuple{m_root, "export/docs/readme", m_readme},
             std::tuple{m_root, "export/stop", m_stop},
             std::tuple{m_docs, "readme", m_readme},
         }) {
        auto const resolved = m_tree.resolve(directory, path);
        ASSERT_TRUE(resolved.is_ok()) << path;
        EXPECT_EQ(resolved->node, node) << path;
        // Empty is "no Subtree node", where "." is "ended on one".
        EXPECT_TRUE(resolved->rest.empty()) << path;
        EXPECT_EQ(node_of(m_tree.lookup(directory, path)), node) << path;
    }
}

TEST_F(IoserviceSubtree, ResolveRefusesAsLookupDoesUpToTheNode) {
    EXPECT_EQ(m_tree.resolve(m_root, "nope").unwrap_err(), Status::NotFound);
    EXPECT_EQ(m_tree.resolve(m_root, "export/nope/x").unwrap_err(), Status::NotFound);
    EXPECT_EQ(m_tree.resolve(m_root, "export/docs/readme/x").unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.resolve(m_root, "export/stop/x").unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.resolve(m_readme, ".").unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.resolve(NodeId{11}, ".").unwrap_err(), Status::InvArgs);
    // The path is judged whole first, so a bad segment after the node is
    // refused though the walk would have stopped before it.
    for (auto const path: {"/export/data", "export/data/", "export/data//x", "export/data/../x",
                           "export/data/./x", ""}) {
        EXPECT_EQ(m_tree.resolve(m_root, path).unwrap_err(), Status::BadPath) << path;
    }
    // A Subtree node is no place to start a walk from.
    EXPECT_EQ(m_tree.resolve(m_data, ".").unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_tree.resolve(m_data, "x").unwrap_err(), Status::NotDir);
}

TEST_F(IoserviceSubtree, LookupWalksToASubtreeNodeButNotThroughIt) {
    EXPECT_EQ(node_of(m_tree.lookup(m_root, "export/data")), m_data);
    EXPECT_EQ(node_of(m_tree.lookup(m_export, "docs/mnt")), m_mnt);
    EXPECT_EQ(node_of(m_tree.lookup(m_root, "other")), m_other);
    // Below it is the program's, and lookup() answers for this tree's nodes
    // only: the node is not a directory.
    EXPECT_EQ(refusal(m_tree.lookup(m_root, "export/data/x")), Status::NotDir);
    EXPECT_EQ(refusal(m_tree.lookup(m_root, "export/data/x/y")), Status::NotDir);
    EXPECT_EQ(refusal(m_tree.lookup(m_root, "other/x")), Status::NotDir);
    EXPECT_EQ(refusal(m_tree.lookup(m_data, ".")), Status::NotDir);
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
