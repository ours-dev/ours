/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// route() against the manifests component_manifest() built from the targets
/// in fixtures/, through two trees: realm.cm as the root, and top.cm as the
/// root with realm.cm and user.cm below it and leaf.cm below user.cm. The
/// instance tree is the test's own, resolves a child only when asked, and
/// holds a fixed number of instances, as the root's will (D27).
///
/// Each expectation compares a one-line description of what route() answered,
/// so that a failure prints the whole answer: the provider or built-in, the
/// name, the subdir and the rights of a Route, or the status, the instance,
/// the verb and the name of a RouteError.
///
/// P3 of the component-framework plan reserves route() for the user to write
/// by hand against these tests. The AI wrote it for the MVP on the user's
/// instruction of 2026-10-04; the exercise stays open.
#include <component/route.hpp>

#include <array>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstring>

#include <fixtures/manifests.hpp>
#include <gtest/gtest.h>

namespace {
using component::Builtin;
using component::Decl;
using component::Kind;
using component::RefKind;
using component::Rights;
using component::Subdir;
using component::Verb;
using ours::Status;
namespace format = component::format;

/// The root's built-ins on dev: the design's "built-ins" object.
constexpr std::array<Builtin, 3> BUILTINS{
    Builtin{.kind = Kind::Directory, .name = "boot", .rights = Rights::RxStarDir},
    Builtin{.kind = Kind::Protocol, .name = "ours.process.Launcher", .rights = ustl::NONE},
    Builtin{.kind = Kind::Protocol, .name = "ours.resource.Debug", .rights = ustl::NONE},
};

constexpr std::string_view URL_PREFIX = "fuchsia-boot:///#meta/";

/// A manifest with no records at all, which decl() hands out, after failing
/// the test, for an instance route() should have resolved first.
auto empty_decl() -> Decl const & {
    static auto const BYTES = [] {
        auto header = format::Header{};
        header.magic = format::MAGIC;
        header.version = format::VERSION;
        header.total_size = sizeof(format::Header);
        auto out = std::array<u8, sizeof(format::Header)>{};
        std::memcpy(out.data(), &header, sizeof(header));
        return out;
    }();
    static auto const DECL = Decl::decode(BYTES).unwrap();
    return DECL;
}

class Tree;

/// One instance of the test's tree, and route()'s Instance: its parent, its
/// index in the parent, its decl once resolved, and children created at
/// resolve, Unresolved. Breaking the concept's preconditions fails the test
/// instead of crashing it, so that the next test still runs.
class Node final {
  public:
    Node(Tree &tree, Node *const parent, u32 const index, std::string name, std::string url)
        : m_tree(&tree)
        , m_parent(parent)
        , m_index(index)
        , m_name(std::move(name))
        , m_url(std::move(url)) {}

    Node(Node const &) = delete;
    auto operator=(Node const &) -> Node & = delete;

    [[nodiscard]]
    auto parent() const -> Node * {
        return m_parent;
    }

    [[nodiscard]]
    auto index() const -> u32 {
        return m_index;
    }

    [[nodiscard]]
    auto decl() const -> Decl const & {
        if (m_decl.is_none()) {
            ADD_FAILURE() << "decl() of " << moniker() << ", which is not resolved";
            return empty_decl();
        }
        return m_decl.value();
    }

    [[nodiscard]]
    auto resolve() -> Status;

    [[nodiscard]]
    auto child(u32 index) -> Node &;

    /// The instance's path from the root: "." for the root, "realm/provider"
    /// below it.
    [[nodiscard]]
    auto moniker() const -> std::string {
        if (m_parent == nullptr) {
            return ".";
        }
        if (m_parent->m_parent == nullptr) {
            return m_name;
        }
        return m_parent->moniker() + "/" + m_name;
    }

    [[nodiscard]]
    auto is_resolved() const -> bool {
        return m_decl.is_some();
    }

    /// The child named |name|, or nullptr. Resolves nothing.
    [[nodiscard]]
    auto find(std::string_view const name) const -> Node * {
        for (auto const &child: m_children) {
            if (child->m_name == name) {
                return child.get();
            }
        }
        return nullptr;
    }

  private:
    Tree *m_tree;
    Node *m_parent;
    u32 m_index;
    std::string m_name;
    std::string m_url;
    ustl::Option<Decl> m_decl;
    std::vector<std::unique_ptr<Node>> m_children;
};

static_assert(component::Instance<Node>);

/// The fixtures' manifests by URL, the resolver over them, and the tree
/// rooted at one of them.
class Tree final {
  public:
    /// The most instances the tree holds, the root included; the stand-in for
    /// the root's fixed pool (D27). Larger than the root's 8 components, so
    /// that a manifest which names itself shows a few levels before the pool
    /// is full, and still more than top.cm's tree of 7 needs.
    static constexpr usize POOL = 16;

    /// The tree rooted at |root|, "realm.cm" or "top.cm", with the root
    /// resolved, as the root resolves `meta/root.cm` before anything else.
    explicit Tree(std::string_view const root) {
        constexpr std::array<std::pair<std::string_view, std::span<u8 const>>, 7> MANIFESTS{{
            {"realm.cm", fixtures::REALM},
            {"provider.cm", fixtures::PROVIDER},
            {"consumer.cm", fixtures::CONSUMER},
            {"hollow.cm", fixtures::HOLLOW},
            {"top.cm", fixtures::TOP},
            {"user.cm", fixtures::USER},
            {"leaf.cm", fixtures::LEAF},
        }};
        for (auto const &[name, bytes]: MANIFESTS) {
            m_bytes.emplace(url_of(name), std::vector<u8>(bytes.begin(), bytes.end()));
        }
        m_root = std::make_unique<Node>(*this, nullptr, 0, "", url_of(root));
        m_instances = 1;
        EXPECT_EQ(m_root->resolve(), Status::Ok) << "resolving " << root;
    }

    Tree(Tree const &) = delete;
    auto operator=(Tree const &) -> Tree & = delete;

    /// The instance at |moniker|, "." or "realm/provider", without resolving
    /// it; its ancestors are resolved on the way. nullptr when there is none.
    [[nodiscard]]
    auto peek(std::string_view const moniker) -> Node * {
        auto *node = m_root.get();
        if (moniker == ".") {
            return node;
        }
        auto rest = moniker;
        while (node != nullptr) {
            auto const slash = rest.find('/');
            auto const name = rest.substr(0, slash);
            if (node->resolve() != Status::Ok) {
                return nullptr;
            }
            node = node->find(name);
            if (slash == std::string_view::npos) {
                return node;
            }
            rest.remove_prefix(slash + 1);
        }
        return nullptr;
    }

    /// The instance at |moniker|, resolved, as the root has resolved a
    /// component before it routes the component's uses.
    [[nodiscard]]
    auto resolved(std::string_view const moniker) -> Node * {
        auto *const node = peek(moniker);
        if (node == nullptr || node->resolve() != Status::Ok) {
            return nullptr;
        }
        return node;
    }

    /// The bytes resolve() reads for |manifest|, "provider.cm", to break on
    /// purpose before that manifest is resolved.
    [[nodiscard]]
    auto bytes(std::string_view const manifest) -> std::vector<u8> & {
        return m_bytes.find(url_of(manifest))->second;
    }

    /// Makes resolving |manifest| fail, as a manifest missing from /boot would.
    auto refuse(std::string_view const manifest) -> void {
        m_refused.insert(url_of(manifest));
    }

    /// Takes room for |count| more instances, or answers false when the pool
    /// cannot hold them, taking none.
    [[nodiscard]]
    auto admit(usize const count) -> bool {
        if (count > POOL - m_instances) {
            return false;
        }
        m_instances += count;
        return true;
    }

    [[nodiscard]]
    auto instances() const -> usize {
        return m_instances;
    }

    /// The resolver: the bytes at |url|, or NotFound.
    [[nodiscard]]
    auto load(std::string_view const url) const -> ustl::Result<std::span<u8 const>, Status> {
        auto const found = m_bytes.find(url);
        if (found == m_bytes.end() || m_refused.contains(url)) {
            return ustl::err(Status::NotFound);
        }
        return ustl::ok(std::span<u8 const>(found->second));
    }

    /// Where child() leads for an index it has no child at: an instance whose
    /// manifest cannot be resolved.
    [[nodiscard]]
    auto nowhere() -> Node & {
        if (m_nowhere == nullptr) {
            m_nowhere = std::make_unique<Node>(*this, m_root.get(), 0, "<nowhere>", "");
        }
        return *m_nowhere;
    }

  private:
    [[nodiscard]]
    static auto url_of(std::string_view const manifest) -> std::string {
        return std::string(URL_PREFIX) + std::string(manifest);
    }

    std::map<std::string, std::vector<u8>, std::less<>> m_bytes;
    std::set<std::string, std::less<>> m_refused;
    std::unique_ptr<Node> m_root;
    /// Not in the pool: it stands for a broken precondition, not an instance.
    std::unique_ptr<Node> m_nowhere;
    usize m_instances = 0;
};

auto Node::resolve() -> Status {
    if (m_decl.is_some()) {
        return Status::Ok;
    }
    auto loaded = m_tree->load(m_url);
    if (loaded.is_err()) {
        return loaded.unwrap_err();
    }
    auto decoded = Decl::decode(loaded.unwrap());
    if (decoded.is_err()) {
        return decoded.unwrap_err();
    }
    // The children are created here, Unresolved, so they need room in the
    // pool now; without it the instance stays Unresolved, as a request past
    // one of the root's pools answers NoResource (D27).
    if (!m_tree->admit(decoded.value().children().size())) {
        return Status::NoResource;
    }
    m_decl = decoded.unwrap();
    auto const children = m_decl.value().children();
    for (u32 index = 0; index < children.size(); ++index) {
        auto const child = children[index];
        m_children.push_back(std::make_unique<Node>(*m_tree, this, index, std::string(child.name),
                                                    std::string(child.url)));
    }
    return Status::Ok;
}

auto Node::child(u32 const index) -> Node & {
    if (m_decl.is_none()) {
        ADD_FAILURE() << "child(" << index << ") of " << moniker() << ", which is not resolved";
        return m_tree->nowhere();
    }
    if (index >= m_children.size()) {
        ADD_FAILURE() << "child(" << index << ") of " << moniker() << ", which has "
                      << m_children.size() << " children";
        return m_tree->nowhere();
    }
    return *m_children[index];
}

using RouteResult = ustl::Result<component::Route<Node>, component::RouteError<Node>>;

auto rights_text(ustl::Option<Rights> const &rights) -> std::string {
    if (rights.is_none()) {
        return "none";
    }
    switch (rights.value()) {
        case Rights::RStarDir:
            return "r*";
        case Rights::RxStarDir:
            return "rx*";
        default:
            return std::format("{:#x}", static_cast<u64>(rights.value()));
    }
}

auto verb_text(Verb const verb) -> std::string {
    switch (verb) {
        case Verb::Offer:
            return "Offer";
        case Verb::Expose:
            return "Expose";
        case Verb::Declare:
            return "Declare";
        case Verb::Use:
            return "Use";
    }
    return std::format("Verb({})", static_cast<u32>(verb));
}

/// What route() answered, in one line:
///   built-in boot, subdir "config/ours", rights r*
///   realm/provider serves ours.example.Echo, subdir "", rights none
///   NotFound at hollow: Declare ours.example.Hollow
///   AccessDenied at the built-ins: Declare boot
auto describe(RouteResult const &result) -> std::string {
    if (result.is_ok()) {
        auto const &route = result.value();
        auto source = std::string("built-in ") + std::string(route.name);
        if (route.provider != nullptr) {
            source = route.provider->moniker() + " serves " + std::string(route.name);
        }
        return std::format("{}, subdir \"{}\", rights {}", source, route.subdir.view(),
                           rights_text(route.rights));
    }
    auto const &error = result.error();
    auto where = std::string("the built-ins");
    if (error.at != nullptr) {
        where = error.at->moniker();
    }
    return std::format("{} at {}: {} {}", ours::to_string(error.status), where,
                       verb_text(error.verb), error.name);
}

/// route() for |target|'s use named |source_name|, described.
auto route_use(Tree &tree, std::string_view const target, std::string_view const source_name)
    -> std::string {
    auto *const node = tree.resolved(target);
    if (node == nullptr) {
        ADD_FAILURE() << "no instance " << target;
        return "no instance";
    }
    for (auto const use: node->decl().uses()) {
        if (use.source_name == source_name) {
            return describe(component::route(*node, use, BUILTINS));
        }
    }
    ADD_FAILURE() << target << " has no use of " << source_name;
    return "no use";
}

/// Where the tables of |BYTES| start, from the header's counts alone.
struct Tables final {
    format::Header header;
    usize children;
    usize capabilities;
    usize uses;
    usize offers;
    usize exposes;
};

auto tables_of(std::vector<u8> const &bytes) -> Tables {
    auto tables = Tables{};
    std::memcpy(&tables.header, bytes.data(), sizeof(format::Header));
    auto const &header = tables.header;
    tables.children = sizeof(format::Header) + header.program_count * sizeof(format::Program) +
                      header.arg_count * sizeof(format::Arg);
    tables.capabilities = tables.children + header.child_count * sizeof(format::Child);
    tables.uses = tables.capabilities + header.capability_count * sizeof(format::Capability);
    tables.offers = tables.uses + header.use_count * sizeof(format::Use);
    tables.exposes = tables.offers + header.offer_count * sizeof(format::Offer);
    return tables;
}

template <typename T>
auto patch(std::vector<u8> &bytes, usize const offset, T const &value) -> void {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

/// Adds |text| to the string table, which ends the file, and answers where.
auto append_string(std::vector<u8> &bytes, std::string_view const text) -> format::String {
    auto header = tables_of(bytes).header;
    auto const string =
        format::String{.offset = header.string_table_size, .size = static_cast<u32>(text.size())};
    bytes.insert(bytes.end(), text.begin(), text.end());
    header.string_table_size += string.size;
    header.total_size += string.size;
    patch(bytes, 0, header);
    return string;
}

/// The index of the first record of |records| that |matches| accepts.
template <typename Records, typename Matches>
auto index_of(Records const &records, Matches const &matches) -> u32 {
    for (u32 index = 0; index < records.size(); ++index) {
        if (matches(records[index])) {
            return index;
        }
    }
    ADD_FAILURE() << "no such record";
    return 0;
}

// --- Subdir ------------------------------------------------------------------

TEST(Subdir, StartsEmpty) {
    auto const subdir = Subdir{};
    EXPECT_TRUE(subdir.is_empty());
    EXPECT_EQ(subdir.view(), "");
}

TEST(Subdir, PrependsWithOneSlash) {
    auto subdir = Subdir{};
    EXPECT_TRUE(subdir.prepend("ours"));
    EXPECT_EQ(subdir.view(), "ours");
    EXPECT_TRUE(subdir.prepend("config"));
    EXPECT_EQ(subdir.view(), "config/ours");
    EXPECT_TRUE(subdir.prepend("etc/x"));
    EXPECT_EQ(subdir.view(), "etc/x/config/ours");
    EXPECT_FALSE(subdir.is_empty());
}

TEST(Subdir, AnEmptyHeadChangesNothing) {
    auto subdir = Subdir{};
    EXPECT_TRUE(subdir.prepend(""));
    EXPECT_TRUE(subdir.is_empty());
    EXPECT_TRUE(subdir.prepend("ours"));
    EXPECT_TRUE(subdir.prepend(""));
    EXPECT_EQ(subdir.view(), "ours");
}

TEST(Subdir, FillsToTheLimitAndNoFurther) {
    auto subdir = Subdir{};
    auto const full = std::string(component::MAX_PATH_LENGTH, 'a');
    EXPECT_TRUE(subdir.prepend(full));
    EXPECT_EQ(subdir.view(), full);
    EXPECT_FALSE(subdir.prepend("b"));
    EXPECT_EQ(subdir.view(), full);

    auto too_long = Subdir{};
    EXPECT_FALSE(too_long.prepend(std::string(component::MAX_PATH_LENGTH + 1, 'a')));
    EXPECT_TRUE(too_long.is_empty());
}

TEST(Subdir, CountsTheJoiningSlash) {
    // 1022 + '/' + 1 is exactly the limit; 1023 + '/' + 1 is one past it.
    auto fits = Subdir{};
    EXPECT_TRUE(fits.prepend(std::string(component::MAX_PATH_LENGTH - 2, 'a')));
    EXPECT_TRUE(fits.prepend("b"));
    EXPECT_EQ(fits.view().size(), component::MAX_PATH_LENGTH);
    EXPECT_EQ(fits.view().substr(0, 2), "b/");

    auto full = Subdir{};
    auto const tail = std::string(component::MAX_PATH_LENGTH - 1, 'a');
    EXPECT_TRUE(full.prepend(tail));
    EXPECT_FALSE(full.prepend("b"));
    EXPECT_EQ(full.view(), tail);
}

// --- Routes in realm.cm's tree -------------------------------------------------
//
// realm (".", no program)
//   provider  program; declares and exposes ours.example.Echo
//   consumer  program; uses boot, config, echo, Launcher and Hollow
//   hollow    no program; declares and exposes ours.example.Hollow

TEST(Route, ABuiltinProtocol) {
    auto tree = Tree("realm.cm");
    EXPECT_EQ(route_use(tree, "consumer", "ours.process.Launcher"),
              "built-in ours.process.Launcher, subdir \"\", rights none");
    // realm offers the Launcher to each child in a record of its own.
    EXPECT_EQ(route_use(tree, "provider", "ours.process.Launcher"),
              "built-in ours.process.Launcher, subdir \"\", rights none");
}

TEST(Route, ABuiltinDirectoryCarriesTheUsesRights) {
    auto tree = Tree("realm.cm");
    EXPECT_EQ(route_use(tree, "consumer", "boot"), "built-in boot, subdir \"\", rights rx*");
}

/// consumer's `config` (r*, subdir "ours") is realm's `boot` as `config` (r*,
/// subdir "config"): the source keeps its name, the offer's subdir goes in
/// front, and the rights are the use's r*, not the built-in's rx*.
TEST(Route, AnOffersSubdirGoesInFrontOfTheUses) {
    auto tree = Tree("realm.cm");
    EXPECT_EQ(route_use(tree, "consumer", "config"),
              "built-in boot, subdir \"config/ours\", rights r*");
}

/// consumer's `echo` is realm's offer from #provider, renamed with `as`, which
/// provider exposes from self. provider is resolved by the walk, not before.
TEST(Route, ASiblingThroughItsExpose) {
    auto tree = Tree("realm.cm");
    auto const *const provider = tree.peek("provider");
    ASSERT_NE(provider, nullptr);
    EXPECT_FALSE(provider->is_resolved());

    EXPECT_EQ(route_use(tree, "consumer", "echo"),
              "provider serves ours.example.Echo, subdir \"\", rights none");
    EXPECT_TRUE(provider->is_resolved());
}

TEST(Route, AProviderWithoutAProgramIsNotFound) {
    auto tree = Tree("realm.cm");
    EXPECT_EQ(route_use(tree, "consumer", "ours.example.Hollow"),
              "NotFound at hollow: Declare ours.example.Hollow");
}

/// provider's expose of Echo, changed to come from its parent: the compiler
/// never writes one, and following it would turn the walk back up.
TEST(Route, AnExposeFromTheParentIsNotFollowed) {
    auto tree = Tree("realm.cm");
    auto &bytes = tree.bytes("provider.cm");
    patch(bytes, tables_of(bytes).exposes + offsetof(format::Expose, source),
          format::Ref{.kind = RefKind::Parent, .child = 0});
    ASSERT_TRUE(Decl::decode(bytes).is_ok());

    EXPECT_EQ(route_use(tree, "consumer", "echo"),
              "NotFound at provider: Expose ours.example.Echo");
}

/// provider's expose of Echo, changed to a directory: the walk looks for a
/// protocol.
TEST(Route, ExposesMatchTheKind) {
    auto tree = Tree("realm.cm");
    auto &bytes = tree.bytes("provider.cm");
    patch(bytes, tables_of(bytes).exposes + offsetof(format::Expose, kind), Kind::Directory);
    ASSERT_TRUE(Decl::decode(bytes).is_ok());

    EXPECT_EQ(route_use(tree, "consumer", "echo"),
              "NotFound at provider: Expose ours.example.Echo");
}

/// provider's declaration of Echo, changed to a directory.
TEST(Route, DeclarationsMatchTheKind) {
    auto tree = Tree("realm.cm");
    auto &bytes = tree.bytes("provider.cm");
    patch(bytes, tables_of(bytes).capabilities + offsetof(format::Capability, kind),
          Kind::Directory);
    ASSERT_TRUE(Decl::decode(bytes).is_ok());

    EXPECT_EQ(route_use(tree, "consumer", "echo"),
              "NotFound at provider: Declare ours.example.Echo");
}

/// consumer's use of `echo`, changed to come from self: the walk starts from
/// the use's own source, and consumer declares no `echo`.
TEST(Route, TheWalkStartsFromTheUsesSource) {
    auto tree = Tree("realm.cm");
    auto &bytes = tree.bytes("consumer.cm");
    auto const echo = index_of(Decl::decode(bytes).unwrap().uses(),
                               [](component::Use const &use) { return use.source_name == "echo"; });
    patch(bytes, tables_of(bytes).uses + echo * sizeof(format::Use) + offsetof(format::Use, source),
          format::Ref{.kind = RefKind::Self, .child = 0});
    ASSERT_TRUE(Decl::decode(bytes).is_ok());

    EXPECT_EQ(route_use(tree, "consumer", "echo"), "NotFound at consumer: Declare echo");
}

// --- Routes in top.cm's tree ---------------------------------------------------
//
// top (".", no program; declares ours.example.TopSelf)
//   realm     realm.cm, as above, so its children are realm/provider and so on
//   user      program; declares ours.example.UserFirst, then UserEcho
//     leaf    program; uses config, wide, echo and ours.example.UserEcho

/// Each hop's subdir goes in front, so the source's side comes first: top's
/// "etc", then user's "config", then leaf's own "ours".
TEST(Route, SubdirsJoinFromTheSourceSide) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "boot"), "built-in boot, subdir \"etc\", rights r*");
    EXPECT_EQ(route_use(tree, "user/leaf", "config"),
              "built-in boot, subdir \"etc/config/ours\", rights r*");
}

/// leaf's `echo` is user's `ours.example.RealmEcho`, which top offers from
/// #realm, which realm exposes from #provider as provider's
/// `ours.example.Echo`: two levels up, two down, and renamed twice.
TEST(Route, TwoLevelsUpAndTwoDown) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user/leaf", "echo"),
              "realm/provider serves ours.example.Echo, subdir \"\", rights none");
    EXPECT_EQ(route_use(tree, "user", "ours.example.RealmEcho"),
              "realm/provider serves ours.example.Echo, subdir \"\", rights none");
}

/// user declares ours.example.UserFirst ahead of UserEcho, so the route
/// ends at the declaration with the name the walk carries, not at the first
/// protocol user declares.
TEST(Route, AnOfferFromSelf) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user/leaf", "ours.example.UserEcho"),
              "user serves ours.example.UserEcho, subdir \"\", rights none");
}

/// user asks rx* of `narrow`, which top offers as r*.
TEST(Route, AnOfferNarrowerThanTheUseIsAccessDenied) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "narrow"), "AccessDenied at .: Offer narrow");
}

/// user asks rw* of `writable`, which top offers stating no rights, so the
/// built-in's rx* is the next rights the walk meets.
TEST(Route, ABuiltinNarrowerThanTheUseIsAccessDenied) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "writable"), "AccessDenied at the built-ins: Declare boot");
}

/// leaf asks r* of `wide`; user offers it rx*, from its own `thin`, which top
/// offers as r*. r* would do for leaf, but user offered more than it had.
TEST(Route, RightsAreHeldAgainstTheHopBeforeNotTheUse) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user/leaf", "wide"), "AccessDenied at .: Offer thin");
}

TEST(Route, NotFoundInTheParentsOffers) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "ours.example.Missing"),
              "NotFound at .: Offer ours.example.Missing");
}

TEST(Route, NotFoundInAChildsExposes) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "ours.example.Unexposed"),
              "NotFound at realm: Expose ours.example.Unexposed");
}

TEST(Route, NotFoundAmongTheBuiltins) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "ours.example.NotBuiltin"),
              "NotFound at the built-ins: Declare ours.example.NotBuiltin");
}

/// top offers the protocol `boot` from its parent; the built-in `boot` is a
/// directory.
TEST(Route, BuiltinsMatchTheKind) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "ours.example.BootProtocol"),
              "NotFound at the built-ins: Declare boot");
}

/// top offers user a directory named ours.example.Kind, and user uses a
/// protocol by that name.
TEST(Route, OffersMatchTheKind) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "ours.example.Kind"),
              "NotFound at .: Offer ours.example.Kind");
}

/// top offers ours.example.ForRealm to realm, not to user.
TEST(Route, OffersMatchTheTargetChild) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "ours.example.ForRealm"),
              "NotFound at .: Offer ours.example.ForRealm");
}

/// top offers its own ours.example.TopSelf, and has no program to serve it.
TEST(Route, AnOfferFromSelfWithoutAProgramIsNotFound) {
    auto tree = Tree("top.cm");
    EXPECT_EQ(route_use(tree, "user", "ours.example.TopSelf"),
              "NotFound at .: Declare ours.example.TopSelf");
}

/// realm's manifest missing, then present but corrupt: resolve() answers
/// NotFound for the one and InvArgs for the other, and route() answers
/// NotFound for both.
TEST(Route, AChildThatCannotResolveIsNotFound) {
    auto missing = Tree("top.cm");
    missing.refuse("realm.cm");
    EXPECT_EQ(route_use(missing, "user", "ours.example.RealmEcho"),
              "NotFound at realm: Expose ours.example.RealmEcho");

    auto corrupt = Tree("top.cm");
    auto &bytes = corrupt.bytes("realm.cm");
    patch(bytes, offsetof(format::Header, magic), u32{0});
    ASSERT_EQ(corrupt.peek("realm")->resolve(), Status::InvArgs);
    EXPECT_EQ(route_use(corrupt, "user", "ours.example.RealmEcho"),
              "NotFound at realm: Expose ours.example.RealmEcho");
}

/// realm's expose of ours.example.RealmEcho, changed to target realm
/// itself rather than its parent.
TEST(Route, ExposesMatchTheParentTarget) {
    auto tree = Tree("top.cm");
    auto &bytes = tree.bytes("realm.cm");
    patch(bytes, tables_of(bytes).exposes + offsetof(format::Expose, target),
          format::Ref{.kind = RefKind::Self, .child = 0});
    ASSERT_TRUE(Decl::decode(bytes).is_ok());

    EXPECT_EQ(route_use(tree, "user", "ours.example.RealmEcho"),
              "NotFound at realm: Expose ours.example.RealmEcho");
}

/// realm's offer of `config` to consumer, its subdir lengthened so that with
/// consumer's "ours" and the '/' between them it is one byte past
/// MAX_PATH_LENGTH, and then exactly at it.
TEST(Route, ASubdirTooLongIsNotFound) {
    auto const lengthen = [](Tree &tree, usize const size) {
        auto &bytes = tree.bytes("realm.cm");
        auto const config =
            index_of(Decl::decode(bytes).unwrap().offers(),
                     [](component::Offer const &offer) { return offer.target_name == "config"; });
        auto const subdir = append_string(bytes, std::string(size, 'a'));
        patch(bytes,
              tables_of(bytes).offers + config * sizeof(format::Offer) +
                  offsetof(format::Offer, subdir),
              subdir);
        ASSERT_TRUE(Decl::decode(bytes).is_ok());
    };
    constexpr auto OURS = std::string_view("/ours").size();

    auto past = Tree("top.cm");
    lengthen(past, component::MAX_PATH_LENGTH - OURS + 1);
    EXPECT_EQ(route_use(past, "realm/consumer", "config"), "NotFound at realm: Offer config");

    auto at = Tree("top.cm");
    lengthen(at, component::MAX_PATH_LENGTH - OURS);
    auto const expected = std::format("built-in boot, subdir \"{}/ours\", rights r*",
                                      std::string(component::MAX_PATH_LENGTH - OURS, 'a'));
    EXPECT_EQ(route_use(at, "realm/consumer", "config"), expected);
}

/// consumer's own use of `config`, its subdir lengthened to one byte past
/// MAX_PATH_LENGTH, which the compiler refuses and the decoder does not: the
/// use is the hop at fault, so the error names consumer and the verb Use. At
/// exactly the limit the use fits, and realm's "config" in front of it is
/// what overflows.
TEST(Route, AUsesOwnSubdirTooLongIsNotFound) {
    auto const lengthen = [](Tree &tree, usize const size) {
        auto &bytes = tree.bytes("consumer.cm");
        auto const config =
            index_of(Decl::decode(bytes).unwrap().uses(),
                     [](component::Use const &use) { return use.source_name == "config"; });
        auto const subdir = append_string(bytes, std::string(size, 'a'));
        patch(bytes,
              tables_of(bytes).uses + config * sizeof(format::Use) + offsetof(format::Use, subdir),
              subdir);
        ASSERT_TRUE(Decl::decode(bytes).is_ok());
    };

    auto past = Tree("top.cm");
    lengthen(past, component::MAX_PATH_LENGTH + 1);
    EXPECT_EQ(route_use(past, "realm/consumer", "config"),
              "NotFound at realm/consumer: Use config");

    auto at = Tree("top.cm");
    lengthen(at, component::MAX_PATH_LENGTH);
    EXPECT_EQ(route_use(at, "realm/consumer", "config"), "NotFound at realm: Offer config");
}

/// realm.cm changed to name itself as its child `provider`, and to expose
/// ours.example.RealmEcho from that child under the same name: a tree with
/// no bottom, which the decoder accepts. Each step down resolves realm.cm one
/// level deeper, until resolve() finds no room in the pool, and the walk ends
/// there. This is what bounds route() on such a manifest, and why the root's
/// resolve() must fail once its pool is full (D27).
TEST(Route, AManifestThatNamesItselfEndsWhenThePoolIsFull) {
    auto tree = Tree("top.cm");
    auto &bytes = tree.bytes("realm.cm");
    auto const decl = Decl::decode(bytes).unwrap();
    auto const provider = index_of(
        decl.children(), [](component::Child const &child) { return child.name == "provider"; });
    auto const echo = index_of(decl.exposes(), [](component::Expose const &expose) {
        return expose.target_name == "ours.example.RealmEcho";
    });

    // The expose's source_name becomes its target_name, so that each level
    // asks the next for the same name.
    auto const expose_at = tables_of(bytes).exposes + echo * sizeof(format::Expose);
    auto expose = format::Expose{};
    std::memcpy(&expose, bytes.data() + expose_at, sizeof(expose));
    expose.source_name = expose.target_name;
    patch(bytes, expose_at, expose);
    // decl views |BYTES|, which append_string() may move: it is not read
    // after this.
    auto const url = append_string(bytes, std::string(URL_PREFIX) + "realm.cm");
    patch(bytes,
          tables_of(bytes).children + provider * sizeof(format::Child) +
              offsetof(format::Child, url),
          url);
    ASSERT_TRUE(Decl::decode(bytes).is_ok());

    // top, realm and user, then leaf when user resolves: 4. Each realm.cm
    // that resolves adds 3 children: 7, 10, 13, 16. The fifth realm.cm, at
    // depth 5, finds no room for its children.
    EXPECT_EQ(route_use(tree, "user", "ours.example.RealmEcho"),
              "NotFound at realm/provider/provider/provider/provider: Expose "
              "ours.example.RealmEcho");
    EXPECT_EQ(tree.instances(), Tree::POOL);
}
} // namespace
