/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// route() against the manifests component_manifest() built from the targets
/// in fixtures/, through three trees: realm.cm as the root, top.cm as the
/// root with realm.cm and user.cm below it and leaf.cm below user.cm, and
/// mismatch.cm, whose routes end at the wrong protocol on purpose. The
/// instance tree is the test's own, resolves a child only when asked, and
/// holds a fixed number of instances, as the root's will (D27).
///
/// Each expectation compares a one-line description of what route() answered,
/// so that a failure prints the whole answer: the provider or built-in, the
/// name, the declared path of a directory, the subdir and the rights of a
/// Route, or the status, the instance, the verb and the name of a RouteError,
/// and for a BadType the two types.
///
/// No fixture declares a directory, so the tests of a directory route end at
/// a provider with the bytes of the protocol route of leaf's `echo` patched by
/// hand: each record on it is made a directory's and states what the test
/// says (see Chain). The compiler will write such manifests in a later step;
/// the patched bytes are what it will write.
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
using component::RecordFlags;
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

    /// The tree rooted at |root|, "realm.cm", "top.cm" or "mismatch.cm", with
    /// the root resolved, as the root resolves `meta/root.cm` before anything else.
    /// |prepare|, when given, runs first, on the tree with every manifest's bytes
    /// loaded and nothing resolved: the one chance to patch the root's own.
    explicit Tree(std::string_view const root,
                  std::function<void(Tree &)> const &prepare = nullptr) {
        constexpr std::array<std::pair<std::string_view, std::span<u8 const>>, 8> MANIFESTS{{
            {"realm.cm", fixtures::REALM},
            {"provider.cm", fixtures::PROVIDER},
            {"consumer.cm", fixtures::CONSUMER},
            {"hollow.cm", fixtures::HOLLOW},
            {"top.cm", fixtures::TOP},
            {"user.cm", fixtures::USER},
            {"leaf.cm", fixtures::LEAF},
            {"mismatch.cm", fixtures::MISMATCH},
        }};
        for (auto const &[name, bytes]: MANIFESTS) {
            m_bytes.emplace(url_of(name), std::vector<u8>(bytes.begin(), bytes.end()));
        }
        if (prepare) {
            prepare(*this);
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

/// The rights a manifest spells r*, rx*, rw* and w*, by their ours.io values.
constexpr auto R_STAR = Rights::RStarDir;
constexpr auto RX_STAR = Rights::RxStarDir;
constexpr auto W_STAR = Rights::Connect | Rights::Enumerate | Rights::Traverse |
                        Rights::WriteBytes | Rights::ModifyDirectory | Rights::UpdateAttributes;
constexpr auto RW_STAR = R_STAR | W_STAR;

auto rights_text(ustl::Option<Rights> const &rights) -> std::string {
    if (rights.is_none()) {
        return "none";
    }
    switch (rights.value()) {
        case R_STAR:
            return "r*";
        case RX_STAR:
            return "rx*";
        case W_STAR:
            return "w*";
        case RW_STAR:
            return "rw*";
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
///   realm/provider serves ours.example.Echo, path "/export/data", subdir "a", rights r*
///   NotFound at hollow: Declare ours.example.Hollow
///   AccessDenied at the built-ins: Declare boot
///   BadType at provider: Declare ours.example.Echo (wants x, got ours.example.Echo)
/// The types are named only when the error has them, so every other
/// expectation also pins that want and got are empty; the same goes for the
/// path of a Route, so every protocol and built-in expectation pins that it
/// has none.
auto describe(RouteResult const &result) -> std::string {
    if (result.is_ok()) {
        auto const &route = result.value();
        auto source = std::string("built-in ") + std::string(route.name);
        if (route.provider != nullptr) {
            source = route.provider->moniker() + " serves " + std::string(route.name);
        }
        auto path = std::string();
        if (!route.path.empty()) {
            path = std::format(", path \"{}\"", route.path);
        }
        return std::format("{}{}, subdir \"{}\", rights {}", source, path, route.subdir.view(),
                           rights_text(route.rights));
    }
    auto const &error = result.error();
    auto where = std::string("the built-ins");
    if (error.at != nullptr) {
        where = error.at->moniker();
    }
    auto text = std::format("{} at {}: {} {}", ours::to_string(error.status), where,
                            verb_text(error.verb), error.name);
    if (!error.want.empty() || !error.got.empty()) {
        text += std::format(" (wants {}, got {})", error.want, error.got);
    }
    return text;
}

/// route() for |target|'s use named |source_name|, described, over |builtins|.
auto route_use(Tree &tree, std::string_view const target, std::string_view const source_name,
               std::span<Builtin const> const builtins = BUILTINS) -> std::string {
    auto *const node = tree.resolved(target);
    if (node == nullptr) {
        ADD_FAILURE() << "no instance " << target;
        return "no instance";
    }
    for (auto const use: node->decl().uses()) {
        if (use.source_name == source_name) {
            return describe(component::route(*node, use, builtins));
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

/// Makes |manifest|'s use named |source_name| state |type| as its protocol.
/// |manifest| must not be resolved yet.
auto set_use_type(Tree &tree, std::string_view const manifest, std::string_view const source_name,
                  std::string_view const type) -> void {
    auto &bytes = tree.bytes(manifest);
    auto const use =
        index_of(Decl::decode(bytes).unwrap().uses(), [source_name](component::Use const &use) {
            return use.source_name == source_name;
        });
    auto const string = append_string(bytes, type);
    patch(bytes, tables_of(bytes).uses + use * sizeof(format::Use) + offsetof(format::Use, type),
          string);
    ASSERT_TRUE(Decl::decode(bytes).is_ok());
}

/// Makes |manifest|'s capability named |name| state |type|. |manifest| must not
/// be resolved yet.
auto set_capability_type(Tree &tree, std::string_view const manifest, std::string_view const name,
                         std::string_view const type) -> void {
    auto &bytes = tree.bytes(manifest);
    auto const capability = index_of(
        Decl::decode(bytes).unwrap().capabilities(),
        [name](component::Capability const &capability) { return capability.name == name; });
    auto const string = append_string(bytes, type);
    patch(bytes,
          tables_of(bytes).capabilities + capability * sizeof(format::Capability) +
              offsetof(format::Capability, type),
          string);
    ASSERT_TRUE(Decl::decode(bytes).is_ok());
}

/// BUILTINS with the Launcher given |type| and |rights|.
auto builtins_with_launcher(std::string_view const type, ustl::Option<Rights> const rights)
    -> std::array<Builtin, BUILTINS.size()> {
    auto builtins = BUILTINS;
    for (auto &builtin: builtins) {
        if (builtin.name == "ours.process.Launcher") {
            builtin.type = type;
            builtin.rights = rights;
        }
    }
    return builtins;
}

// --- Directory routes: patched records ------------------------------------------

/// What one hop of a directory route states: its rights, none for a hop that
/// states none, and its subdir.
struct Hop final {
    ustl::Option<Rights> rights = ustl::NONE;
    std::string_view subdir = {};
};

/// Makes the record at |record| of the table of |Raw| (a Use, an Offer or an
/// Expose, which share their field names) a directory's, stating |hop|.
template <typename Raw>
auto make_directory_hop(std::vector<u8> &bytes, usize const record, Hop const &hop) -> void {
    patch(bytes, record + offsetof(Raw, kind), Kind::Directory);
    if (hop.rights.is_some()) {
        patch(bytes, record + offsetof(Raw, flags), RecordFlags::RightsStated);
        patch(bytes, record + offsetof(Raw, rights), hop.rights.value());
    }
    if (!hop.subdir.empty()) {
        patch(bytes, record + offsetof(Raw, subdir), append_string(bytes, hop.subdir));
    }
}

/// |manifest|'s use named |source_name| as a directory use stating |hop|.
auto directory_use(Tree &tree, std::string_view const manifest, std::string_view const source_name,
                   Hop const &hop) -> void {
    auto &bytes = tree.bytes(manifest);
    auto const index =
        index_of(Decl::decode(bytes).unwrap().uses(), [source_name](component::Use const &use) {
            return use.source_name == source_name;
        });
    make_directory_hop<format::Use>(bytes, tables_of(bytes).uses + index * sizeof(format::Use),
                                    hop);
}

/// |manifest|'s offer that targets |target_name|, as a directory offer.
auto directory_offer(Tree &tree, std::string_view const manifest,
                     std::string_view const target_name, Hop const &hop) -> void {
    auto &bytes = tree.bytes(manifest);
    auto const index = index_of(
        Decl::decode(bytes).unwrap().offers(),
        [target_name](component::Offer const &offer) { return offer.target_name == target_name; });
    make_directory_hop<format::Offer>(bytes,
                                      tables_of(bytes).offers + index * sizeof(format::Offer), hop);
}

/// |manifest|'s expose that targets |target_name|, as a directory expose.
auto directory_expose(Tree &tree, std::string_view const manifest,
                      std::string_view const target_name, Hop const &hop) -> void {
    auto &bytes = tree.bytes(manifest);
    auto const index = index_of(Decl::decode(bytes).unwrap().exposes(),
                                [target_name](component::Expose const &expose) {
                                    return expose.target_name == target_name;
                                });
    make_directory_hop<format::Expose>(
        bytes, tables_of(bytes).exposes + index * sizeof(format::Expose), hop);
}

/// |manifest|'s capability named |name| as a directory declared at |path|,
/// stating |rights|.
auto directory_capability(Tree &tree, std::string_view const manifest, std::string_view const name,
                          ustl::Option<Rights> const &rights, std::string_view const path) -> void {
    auto &bytes = tree.bytes(manifest);
    auto const index = index_of(
        Decl::decode(bytes).unwrap().capabilities(),
        [name](component::Capability const &capability) { return capability.name == name; });
    auto const record = tables_of(bytes).capabilities + index * sizeof(format::Capability);
    patch(bytes, record + offsetof(format::Capability, kind), Kind::Directory);
    if (rights.is_some()) {
        patch(bytes, record + offsetof(format::Capability, flags), RecordFlags::RightsStated);
        patch(bytes, record + offsetof(format::Capability, rights), rights.value());
    }
    patch(bytes, record + offsetof(format::Capability, path), append_string(bytes, path));
}

/// What each record on one route states. The route is leaf's `echo` in top.cm's
/// tree, six records long, from the use to the declaration:
///
///   use              leaf.cm,     use `echo`                          (Use, at user/leaf)
///   user_offer       user.cm,     offer to #leaf as `echo`            (Offer, at user)
///   top_offer        top.cm,      offer to #user of RealmEcho         (Offer, at the root)
///   realm_expose     realm.cm,    expose of RealmEcho from #provider  (Expose, at realm)
///   provider_expose  provider.cm, expose of Echo from self            (Expose, at realm/provider)
///   declared, path   provider.cm, capability Echo                     (Declare, at realm/provider)
///
/// A hop that states nothing narrows nothing, and the default Chain asks r*
/// of a provider that allows r*, at /export/data: the route that works.
struct Chain final {
    Hop use = {.rights = R_STAR};
    Hop user_offer = {};
    Hop top_offer = {};
    Hop realm_expose = {};
    Hop provider_expose = {};
    ustl::Option<Rights> declared = R_STAR;
    std::string_view path = "/export/data";
};

/// The records of |chain| made directory records, in |tree|, which must not
/// have resolved any of those manifests yet.
auto make_directory_chain(Tree &tree, Chain const &chain) -> void {
    directory_use(tree, "leaf.cm", "echo", chain.use);
    directory_offer(tree, "user.cm", "echo", chain.user_offer);
    directory_offer(tree, "top.cm", "ours.example.RealmEcho", chain.top_offer);
    directory_expose(tree, "realm.cm", "ours.example.RealmEcho", chain.realm_expose);
    directory_expose(tree, "provider.cm", "ours.example.Echo", chain.provider_expose);
    directory_capability(tree, "provider.cm", "ours.example.Echo", chain.declared, chain.path);
}

/// top.cm's tree with leaf's `echo` made a directory route as |chain| says.
auto directory_tree(Chain const &chain) -> Tree {
    return Tree("top.cm", [&chain](Tree &tree) { make_directory_chain(tree, chain); });
}

/// The route of leaf's `echo` through |chain|, described.
auto route_chain(Chain const &chain) -> std::string {
    auto tree = directory_tree(chain);
    return route_use(tree, "user/leaf", "echo");
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

// --- Protocol types ------------------------------------------------------------
//
// A protocol use states the protocol it speaks, a capability the one it
// provides, and a route that ends at another is BadType. The routes above all
// end at the type their use asks for, so every one of them also shows that a
// match is no error. mismatch.cm's tree is for the routes that do not:
//
// mismatch (".", no program)
//   provider  program; declares and exposes ours.example.Echo (the type of
//             the same name)
//   consumer  program; uses ours.process.Launcher with no type, and `echo`
//             with the type ours.example.Echo
// and offers provider's Echo to consumer as ours.process.Launcher, and the
// built-in Launcher to consumer as `echo`.

/// A rename with the type stated on the use is no mismatch, however far the
/// name has travelled: consumer's `echo` is provider's ours.example.Echo, and
/// leaf's `echo` and user's ours.example.RealmEcho are renamed twice more.
TEST(RouteTypes, ARenamedUseWithItsTypeStatedRoutes) {
    auto realm = Tree("realm.cm");
    EXPECT_EQ(route_use(realm, "consumer", "echo"),
              "provider serves ours.example.Echo, subdir \"\", rights none");

    auto top = Tree("top.cm");
    EXPECT_EQ(route_use(top, "user/leaf", "echo"),
              "realm/provider serves ours.example.Echo, subdir \"\", rights none");
    EXPECT_EQ(route_use(top, "user", "ours.example.RealmEcho"),
              "realm/provider serves ours.example.Echo, subdir \"\", rights none");
}

/// A use and a capability that state no type are both their names, so they
/// agree without saying so; and a use that states its own name says what it
/// said before. A use that states another type does not reach the capability,
/// and `got` is the type the decoder filled in for the declaration: its name.
TEST(RouteTypes, TheDefaultTypeIsTheName) {
    constexpr auto ROUTED = "user serves ours.example.UserEcho, subdir \"\", rights none";

    auto none = Tree("top.cm");
    EXPECT_EQ(route_use(none, "user/leaf", "ours.example.UserEcho"), ROUTED);

    auto same = Tree("top.cm");
    set_use_type(same, "leaf.cm", "ours.example.UserEcho", "ours.example.UserEcho");
    EXPECT_EQ(route_use(same, "user/leaf", "ours.example.UserEcho"), ROUTED);

    auto other = Tree("top.cm");
    set_use_type(other, "leaf.cm", "ours.example.UserEcho", "ours.example.Other");
    EXPECT_EQ(route_use(other, "user/leaf", "ours.example.UserEcho"),
              "BadType at user: Declare ours.example.UserEcho "
              "(wants ours.example.Other, got ours.example.UserEcho)");
}

/// mismatch offers provider's ours.example.Echo as ours.process.Launcher, and
/// consumer's use of that name states no type, so it wants the type of its
/// name. The error is at the provider, names the capability as provider
/// declared it, not as it was offered, and has both types.
TEST(RouteTypes, AProviderOfAnotherTypeIsBadType) {
    auto tree = Tree("mismatch.cm");
    EXPECT_EQ(route_use(tree, "consumer", "ours.process.Launcher"),
              "BadType at provider: Declare ours.example.Echo "
              "(wants ours.process.Launcher, got ours.example.Echo)");
}

/// The same answer, field by field: a mismatch is BadType, in the declaration
/// the walk reached, with the types as views of the manifests' bytes.
TEST(RouteTypes, TheErrorNamesTheDeclarationAndBothTypes) {
    auto tree = Tree("mismatch.cm");
    auto *const consumer = tree.resolved("consumer");
    ASSERT_NE(consumer, nullptr);
    for (auto const use: consumer->decl().uses()) {
        if (use.source_name != "ours.process.Launcher") {
            continue;
        }
        auto const result = component::route(*consumer, use, BUILTINS);
        ASSERT_TRUE(result.is_err());
        auto const &error = result.error();
        EXPECT_EQ(error.status, Status::BadType);
        EXPECT_EQ(error.at, tree.peek("provider"));
        EXPECT_EQ(error.verb, Verb::Declare);
        EXPECT_EQ(error.name, "ours.example.Echo");
        EXPECT_EQ(error.want, "ours.process.Launcher");
        EXPECT_EQ(error.got, "ours.example.Echo");
        return;
    }
    ADD_FAILURE() << "consumer has no use of ours.process.Launcher";
}

/// mismatch offers the built-in Launcher as `echo`, and consumer's use of
/// `echo` states ours.example.Echo. The Launcher states no type, so the route
/// reaches its name. At the built-ins the error has no instance.
TEST(RouteTypes, ABuiltinWithoutATypeIsComparedByItsName) {
    auto tree = Tree("mismatch.cm");
    EXPECT_EQ(route_use(tree, "consumer", "echo"),
              "BadType at the built-ins: Declare ours.process.Launcher "
              "(wants ours.example.Echo, got ours.process.Launcher)");

    // The same built-in, for the use that means it: the name is the type.
    auto realm = Tree("realm.cm");
    EXPECT_EQ(route_use(realm, "consumer", "ours.process.Launcher"),
              "built-in ours.process.Launcher, subdir \"\", rights none");
}

/// A built-in that states a type is compared by it, not by its name: the
/// Launcher stated as ours.example.Echo is what mismatch's `echo` means, and
/// no longer what consumer's use of ours.process.Launcher means.
TEST(RouteTypes, ABuiltinThatStatesATypeIsComparedByIt) {
    auto const echo = builtins_with_launcher("ours.example.Echo", ustl::NONE);
    auto mismatch = Tree("mismatch.cm");
    EXPECT_EQ(route_use(mismatch, "consumer", "echo", echo),
              "built-in ours.process.Launcher, subdir \"\", rights none");

    auto realm = Tree("realm.cm");
    EXPECT_EQ(route_use(realm, "consumer", "ours.process.Launcher", echo),
              "BadType at the built-ins: Declare ours.process.Launcher "
              "(wants ours.process.Launcher, got ours.example.Echo)");

    // Stating the name is stating nothing.
    auto const itself = builtins_with_launcher("ours.process.Launcher", ustl::NONE);
    EXPECT_EQ(route_use(realm, "consumer", "ours.process.Launcher", itself),
              "built-in ours.process.Launcher, subdir \"\", rights none");
}

/// A directory is held to its rights at every hop and has no type: consumer's
/// `boot` reaches the built-in `boot` whatever type that states.
TEST(RouteTypes, ADirectoryUseIsNeverCompared) {
    auto builtins = BUILTINS;
    for (auto &builtin: builtins) {
        if (builtin.kind == Kind::Directory) {
            builtin.type = "ours.example.Other";
        }
    }
    auto tree = Tree("realm.cm");
    EXPECT_EQ(route_use(tree, "consumer", "boot", builtins),
              "built-in boot, subdir \"\", rights rx*");
}

/// The same at a provider. consumer's `echo`, with every record on its route
/// made a directory's and its use stating another type, ends at provider's
/// declaration: a directory use is not compared there either.
TEST(RouteTypes, ADirectoryUseIsNeverComparedAtAProviderEither) {
    auto tree = Tree("top.cm");
    set_use_type(tree, "consumer.cm", "echo", "ours.example.Other");

    auto &consumer = tree.bytes("consumer.cm");
    auto const use = index_of(Decl::decode(consumer).unwrap().uses(),
                              [](component::Use const &use) { return use.source_name == "echo"; });
    patch(consumer,
          tables_of(consumer).uses + use * sizeof(format::Use) + offsetof(format::Use, kind),
          Kind::Directory);

    auto &realm = tree.bytes("realm.cm");
    auto const offer =
        index_of(Decl::decode(realm).unwrap().offers(),
                 [](component::Offer const &offer) { return offer.target_name == "echo"; });
    patch(realm,
          tables_of(realm).offers + offer * sizeof(format::Offer) + offsetof(format::Offer, kind),
          Kind::Directory);

    // provider.cm has one expose and one capability.
    auto &provider = tree.bytes("provider.cm");
    patch(provider, tables_of(provider).exposes + offsetof(format::Expose, kind), Kind::Directory);
    patch(provider, tables_of(provider).capabilities + offsetof(format::Capability, kind),
          Kind::Directory);

    EXPECT_EQ(route_use(tree, "realm/consumer", "echo"),
              "realm/provider serves ours.example.Echo, subdir \"\", rights none");
}

/// A route that fails for another reason keeps that status, even when the
/// types would not have agreed either.
TEST(RouteTypes, ARouteThatFailsForAnotherReasonKeepsItsStatus) {
    // hollow declares ours.example.Hollow as another type, and has no program
    // to serve it: NotFound, as it is without the type.
    auto hollow = Tree("realm.cm");
    set_capability_type(hollow, "hollow.cm", "ours.example.Hollow", "ours.example.Other");
    EXPECT_EQ(route_use(hollow, "consumer", "ours.example.Hollow"),
              "NotFound at hollow: Declare ours.example.Hollow");

    // The Launcher holds less than the use asks, and states another type:
    // the hop's rights are applied first, so AccessDenied. consumer's use is
    // made to state rx*, which a protocol use never does.
    auto denied = Tree("realm.cm");
    auto &bytes = denied.bytes("consumer.cm");
    auto const launcher =
        index_of(Decl::decode(bytes).unwrap().uses(), [](component::Use const &use) {
            return use.source_name == "ours.process.Launcher";
        });
    auto const at = tables_of(bytes).uses + launcher * sizeof(format::Use);
    patch(bytes, at + offsetof(format::Use, flags), RecordFlags::RightsStated);
    patch(bytes, at + offsetof(format::Use, rights), Rights::RxStarDir);
    ASSERT_TRUE(Decl::decode(bytes).is_ok());

    auto const narrow = builtins_with_launcher("ours.example.Other", Rights::RStarDir);
    EXPECT_EQ(route_use(denied, "consumer", "ours.process.Launcher", narrow),
              "AccessDenied at the built-ins: Declare ours.process.Launcher");
}

// --- Directory routes ------------------------------------------------------------
//
// A directory use that ends at a provider: the declaration states a path and
// rights, an expose may state rights and a subdir, and both are hops. The
// records are patched by hand (Chain); the route's other hops, in top.cm's
// tree, are those of the protocol routes above.

TEST(RouteDirectories, ADirectoryUseEndsAtTheProviderWithTheDeclaredPath) {
    EXPECT_EQ(route_chain({}),
              "realm/provider serves ours.example.Echo, path \"/export/data\", subdir \"\", "
              "rights r*");
}

/// The answer field by field: the declared path is a view of the provider's
/// manifest bytes and apart from the subdir, which the hops joined; the rights
/// are the use's, not the declaration's rw*.
TEST(RouteDirectories, TheRouteCarriesThePathApartFromTheSubdirAndTheUsesRights) {
    auto tree = directory_tree({
        .use = {.rights = R_STAR, .subdir = "docs"},
        .realm_expose = {.subdir = "shared"},
        .declared = RW_STAR,
    });
    auto *const leaf = tree.resolved("user/leaf");
    ASSERT_NE(leaf, nullptr);
    auto const use = leaf->decl().uses()[index_of(
        leaf->decl().uses(), [](component::Use const &use) { return use.source_name == "echo"; })];
    auto const result = component::route(*leaf, use, BUILTINS);
    ASSERT_TRUE(result.is_ok()) << describe(result);
    auto const &route = result.value();
    EXPECT_EQ(route.provider, tree.peek("realm/provider"));
    EXPECT_EQ(route.name, "ours.example.Echo");
    EXPECT_EQ(route.path, "/export/data");
    EXPECT_EQ(route.subdir.view(), "shared/docs");
    ASSERT_TRUE(route.rights.is_some());
    EXPECT_EQ(route.rights.value(), R_STAR);
}

/// A protocol route and a built-in route have no declared path: the Route of a
/// protocol provider and of a built-in directory leave it empty. (Every
/// protocol and built-in expectation above also pins it, since describe() names
/// a path only when there is one.)
TEST(RouteDirectories, AProtocolAndABuiltinHaveNoPath) {
    auto tree = Tree("top.cm");
    auto *const user = tree.resolved("user");
    auto *const leaf = tree.resolved("user/leaf");
    ASSERT_NE(user, nullptr);
    ASSERT_NE(leaf, nullptr);
    auto const find = [](component::Decl const &decl, std::string_view const name) {
        return decl.uses()[index_of(
            decl.uses(), [name](component::Use const &use) { return use.source_name == name; })];
    };

    auto const protocol = component::route(*leaf, find(leaf->decl(), "echo"), BUILTINS);
    ASSERT_TRUE(protocol.is_ok()) << describe(protocol);
    EXPECT_NE(protocol.value().provider, nullptr);
    EXPECT_TRUE(protocol.value().path.empty());

    auto const builtin = component::route(*user, find(user->decl(), "boot"), BUILTINS);
    ASSERT_TRUE(builtin.is_ok()) << describe(builtin);
    EXPECT_EQ(builtin.value().provider, nullptr);
    EXPECT_TRUE(builtin.value().path.empty());
}

/// A protocol capability that states a path and rights, which the compiler
/// never writes and the decoder lets through: the route still ends there as a
/// protocol, with no path, and the rights are not a hop.
TEST(RouteDirectories, ADeclarationOfAProtocolIsNoHopAndStatesNoPath) {
    auto tree = Tree("top.cm", [](Tree &tree) {
        auto &leaf = tree.bytes("leaf.cm");
        auto const echo =
            index_of(Decl::decode(leaf).unwrap().uses(),
                     [](component::Use const &use) { return use.source_name == "echo"; });
        auto const use = tables_of(leaf).uses + echo * sizeof(format::Use);
        patch(leaf, use + offsetof(format::Use, flags), RecordFlags::RightsStated);
        patch(leaf, use + offsetof(format::Use, rights), RX_STAR);

        auto &provider = tree.bytes("provider.cm");
        auto const capability = tables_of(provider).capabilities;
        patch(provider, capability + offsetof(format::Capability, flags),
              RecordFlags::RightsStated);
        patch(provider, capability + offsetof(format::Capability, rights), R_STAR);
        patch(provider, capability + offsetof(format::Capability, path),
              append_string(provider, "/export/data"));
        ASSERT_TRUE(Decl::decode(provider).is_ok());
    });
    // r* is less than the rx* the use asks, and a directory declaration would
    // be AccessDenied for it.
    EXPECT_EQ(route_use(tree, "user/leaf", "echo"),
              "realm/provider serves ours.example.Echo, subdir \"\", rights rx*");
}

// Rights at the declaration.

/// user offers rx*, and the provider allows r*: the offer asks for more than
/// the declaration has, so the declaration is the hop at fault.
TEST(RouteDirectories, ADeclarationNarrowerThanTheOfferIsAccessDeniedAtTheProvider) {
    EXPECT_EQ(route_chain({.user_offer = {.rights = RX_STAR}, .declared = R_STAR}),
              "AccessDenied at realm/provider: Declare ours.example.Echo");
}

TEST(RouteDirectories, ADeclarationAsWideAsTheOfferOrWiderIsRouted) {
    constexpr auto ROUTED =
        "realm/provider serves ours.example.Echo, path \"/export/data\", subdir \"\", rights r*";
    EXPECT_EQ(route_chain({.user_offer = {.rights = R_STAR}, .declared = R_STAR}), ROUTED);
    EXPECT_EQ(route_chain({.user_offer = {.rights = R_STAR}, .declared = RW_STAR}), ROUTED);
    EXPECT_EQ(route_chain({.user_offer = {.rights = RX_STAR}, .declared = RX_STAR}), ROUTED);
}

/// The rights contain the hop before, not merely differ from it: w* allows
/// writes where r* allows reads, and holds neither the other's.
TEST(RouteDirectories, ADeclarationThatAllowsOtherRightsIsAccessDenied) {
    EXPECT_EQ(route_chain({.declared = W_STAR}),
              "AccessDenied at realm/provider: Declare ours.example.Echo");
}

/// Rights are held against the hop before the declaration, not against the use:
/// the use asks r*, which the declaration's r* meets, but user offered rx* and
/// the exposes between state none.
TEST(RouteDirectories, TheDeclarationIsHeldAgainstTheLastHopThatStatedRights) {
    EXPECT_EQ(route_chain({.user_offer = {.rights = RX_STAR}, .declared = R_STAR}),
              "AccessDenied at realm/provider: Declare ours.example.Echo");
    EXPECT_EQ(route_chain({.top_offer = {.rights = RX_STAR},
                           .realm_expose = {},
                           .provider_expose = {},
                           .declared = R_STAR}),
              "AccessDenied at realm/provider: Declare ours.example.Echo");
}

/// A declaration that states no rights narrows nothing, as a hop that states
/// none never does. The compiler refuses to write one for a directory; the
/// decoder lets it through.
TEST(RouteDirectories, ADeclarationThatStatesNoRightsNarrowsNothing) {
    EXPECT_EQ(route_chain({.user_offer = {.rights = RX_STAR}, .declared = ustl::NONE}),
              "realm/provider serves ours.example.Echo, path \"/export/data\", subdir \"\", "
              "rights r*");
}

// Rights and subdirs at an expose.

/// realm exposes r* what top offered it as rx*: the expose is the hop at
/// fault, and the error is at realm, the child the walk turned down into. r*
/// is all the use asks, which is why the offer's rx* is what it is held
/// against, not the use.
TEST(RouteDirectories, AnExposeNarrowerThanTheOfferBeforeItIsAccessDeniedAtTheChild) {
    EXPECT_EQ(route_chain({.top_offer = {.rights = RX_STAR},
                           .realm_expose = {.rights = R_STAR},
                           .declared = RX_STAR}),
              "AccessDenied at realm: Expose ours.example.RealmEcho");
}

/// The second expose of the walk, provider's, against realm's before it. The
/// name is the one provider knows the capability by.
TEST(RouteDirectories, AnExposeIsHeldAgainstTheExposeBeforeIt) {
    EXPECT_EQ(route_chain({.realm_expose = {.rights = RX_STAR},
                           .provider_expose = {.rights = R_STAR},
                           .declared = RX_STAR}),
              "AccessDenied at realm/provider: Expose ours.example.Echo");
}

/// The expose is the hop before the declaration when it stated rights: the
/// provider exposes rx* what it declares as r*, so the declaration is the
/// hop at fault, at the provider and not at the expose.
TEST(RouteDirectories, AnExposeWiderThanTheDeclarationIsAccessDeniedAtTheProvider) {
    EXPECT_EQ(route_chain({.provider_expose = {.rights = RX_STAR}, .declared = R_STAR}),
              "AccessDenied at realm/provider: Declare ours.example.Echo");
}

/// Stated rights only grow toward the source: an expose that states more than
/// the offer before it is routed.
TEST(RouteDirectories, AnExposeWiderThanTheOfferBeforeItIsRouted) {
    EXPECT_EQ(route_chain({.top_offer = {.rights = R_STAR},
                           .realm_expose = {.rights = RX_STAR},
                           .provider_expose = {.rights = RX_STAR},
                           .declared = RX_STAR}),
              "realm/provider serves ours.example.Echo, path \"/export/data\", subdir \"\", "
              "rights r*");
}

/// An expose that states none narrows nothing and is not the hop before the
/// next one: the offer's rx* still is.
TEST(RouteDirectories, AnExposeThatStatesNoRightsNarrowsNothing) {
    EXPECT_EQ(route_chain({.top_offer = {.rights = RX_STAR}, .declared = RX_STAR}),
              "realm/provider serves ours.example.Echo, path \"/export/data\", subdir \"\", "
              "rights r*");
    EXPECT_EQ(route_chain({.top_offer = {.rights = RX_STAR}, .declared = R_STAR}),
              "AccessDenied at realm/provider: Declare ours.example.Echo");
}

/// Every hop's subdir goes in front, the source's side first, and the expose
/// subdirs are among them: provider's first, then realm's, top's offer, user's
/// offer and leaf's own. The declared path stays a path of its own.
TEST(RouteDirectories, ExposeSubdirsJoinBetweenTheOffersAndTheDeclaration) {
    EXPECT_EQ(route_chain({
                  .use = {.rights = R_STAR, .subdir = "u"},
                  .user_offer = {.subdir = "o1"},
                  .top_offer = {.subdir = "o2"},
                  .realm_expose = {.subdir = "e1"},
                  .provider_expose = {.subdir = "e2"},
              }),
              "realm/provider serves ours.example.Echo, path \"/export/data\", "
              "subdir \"e2/e1/o2/o1/u\", rights r*");
}

// The length of the Open.

/// The root opens the declared path without its leading '/', joined with the
/// subdir. "/export/data" is "export/data", 11 bytes, so a subdir of 1012
/// bytes after the joining '/' is exactly MAX_PATH_LENGTH, and 1013 is one
/// past it. The leading '/' is not counted: it is not in the Open.
TEST(RouteDirectories, ThePathAndTheSubdirTogetherMustFitOneOpen) {
    constexpr auto OPEN_PATH = std::string_view("export/data").size();
    auto const fits = std::string(component::MAX_PATH_LENGTH - OPEN_PATH - 1, 'a');
    EXPECT_EQ(route_chain({.use = {.rights = R_STAR, .subdir = fits}}),
              std::format("realm/provider serves ours.example.Echo, path \"/export/data\", "
                          "subdir \"{}\", rights r*",
                          fits));

    auto const past = std::string(fits.size() + 1, 'a');
    EXPECT_EQ(route_chain({.use = {.rights = R_STAR, .subdir = past}}),
              "NotFound at realm/provider: Declare ours.example.Echo");
}

/// The path alone can be too long too, which the compiler refuses and the
/// decoder does not.
TEST(RouteDirectories, ADeclaredPathTooLongIsNotFoundAtTheProvider) {
    auto const fits = "/" + std::string(component::MAX_PATH_LENGTH, 'a');
    EXPECT_EQ(route_chain({.path = fits}),
              std::format("realm/provider serves ours.example.Echo, path \"{}\", subdir \"\", "
                          "rights r*",
                          fits));

    auto const past = "/" + std::string(component::MAX_PATH_LENGTH + 1, 'a');
    EXPECT_EQ(route_chain({.path = past}), "NotFound at realm/provider: Declare ours.example.Echo");
}

/// An expose whose subdir overflows the joined one is NotFound at the expose,
/// before the walk reaches the declaration, as an offer's is at the offer. The
/// use's 1000 bytes leave room for 23 more after the joining '/'.
TEST(RouteDirectories, AnExposesSubdirTooLongIsNotFoundAtTheChild) {
    auto const use = std::string(1000, 'a');
    EXPECT_EQ(route_chain({
                  .use = {.rights = R_STAR, .subdir = use},
                  .provider_expose = {.subdir = std::string_view("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")},
              }),
              "NotFound at realm/provider: Expose ours.example.Echo");
    EXPECT_EQ(route_chain({
                  .use = {.rights = R_STAR, .subdir = use},
                  .realm_expose = {.subdir = std::string_view("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")},
              }),
              "NotFound at realm: Expose ours.example.RealmEcho");

    // 1000 + '/' + 23 is a subdir of exactly MAX_PATH_LENGTH: the expose
    // passes, and it is the declaration that finds no room for its path.
    EXPECT_EQ(route_chain({
                  .use = {.rights = R_STAR, .subdir = use},
                  .provider_expose = {.subdir = std::string_view("bbbbbbbbbbbbbbbbbbbbbbb")},
              }),
              "NotFound at realm/provider: Declare ours.example.Echo");
}

/// The rights come first, as at every hop: a route that is denied and too long
/// is denied.
TEST(RouteDirectories, TheRightsOfTheDeclarationAreCheckedBeforeTheLength) {
    auto const past = std::string(component::MAX_PATH_LENGTH, 'a');
    EXPECT_EQ(route_chain({
                  .use = {.rights = R_STAR, .subdir = past},
                  .user_offer = {.rights = RX_STAR},
                  .declared = R_STAR,
              }),
              "AccessDenied at realm/provider: Declare ours.example.Echo");
}

// Kinds, programs and types.

/// The provider's capability of that name is a protocol: a directory use, with
/// every record before it a directory's, never reaches it.
TEST(RouteDirectories, ADirectoryUseNeverReachesAProtocolCapability) {
    auto tree = Tree("top.cm", [](Tree &tree) {
        make_directory_chain(tree, {});
        auto &provider = tree.bytes("provider.cm");
        patch(provider, tables_of(provider).capabilities + offsetof(format::Capability, kind),
              Kind::Protocol);
    });
    EXPECT_EQ(route_use(tree, "user/leaf", "echo"),
              "NotFound at realm/provider: Declare ours.example.Echo");
}

/// hollow.cm declares and exposes ours.example.Hollow and has no program to
/// serve it. Made a directory route, it is NotFound at hollow, Declare, even
/// where the declaration would also have been denied: the program comes first.
TEST(RouteDirectories, AProviderWithoutAProgramIsNotFound) {
    auto tree = Tree("top.cm", [](Tree &tree) {
        directory_use(tree, "consumer.cm", "ours.example.Hollow", {.rights = R_STAR});
        directory_offer(tree, "realm.cm", "ours.example.Hollow", {.rights = RX_STAR});
        directory_expose(tree, "hollow.cm", "ours.example.Hollow", {});
        directory_capability(tree, "hollow.cm", "ours.example.Hollow", R_STAR, "/export/data");
    });
    EXPECT_EQ(route_use(tree, "realm/consumer", "ours.example.Hollow"),
              "NotFound at realm/hollow: Declare ours.example.Hollow");
}

/// A directory has no type: the use states one and the capability another, and
/// the route is not BadType. (The protocol side is pinned by RouteTypes.)
TEST(RouteDirectories, BadTypeIsForProtocolsOnly) {
    auto tree = Tree("top.cm", [](Tree &tree) {
        make_directory_chain(tree, {});
        set_use_type(tree, "leaf.cm", "echo", "ours.example.Other");
        set_capability_type(tree, "provider.cm", "ours.example.Echo", "ours.example.Another");
    });
    EXPECT_EQ(route_use(tree, "user/leaf", "echo"),
              "realm/provider serves ours.example.Echo, path \"/export/data\", subdir \"\", "
              "rights r*");
}
} // namespace
