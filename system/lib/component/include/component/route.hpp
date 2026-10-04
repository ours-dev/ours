/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef COMPONENT_ROUTE_HPP
#define COMPONENT_ROUTE_HPP 1

#include <component/decl.hpp>
#include <component/format.hpp>

#include <array>
#include <concepts>
#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

/// The router: one use of a component, walked through its parent's offers and
/// its providers' exposes to the capability's source. The design is "The
/// routing walk" in docs/design/261004/component-framework.md (D3, D9, D11).
namespace component {
/// What route() needs of the root's record of one component instance: the
/// counterpart of Fuchsia's ComponentInstanceInterface
/// (src/sys/lib/routing/src/component_instance.rs:22), whose
/// try_get_parent() (:52) answers AboveRoot at the root (:330-332), as
/// parent() answers nullptr here, and whose lock_resolved_state() (:63)
/// resolves the instance first, as resolve() does.
///
/// The root defines the instance (step 13 of the plan), and the router's test
/// defines another over the fixtures' manifests, so the router is a template
/// over this concept rather than a class of its own.
///
/// - `parent()`: the parent, or nullptr for the root.
/// - `index()`: the instance's index in its parent's `decl().children()`, the
///   index an offer's target names (Ref::child). Meaningless for the root.
/// - `decl()`: the decoded manifest. Only for a resolved instance: the target,
///   every ancestor of a resolved instance (a child exists only once its
///   parent has resolved), and an instance whose resolve() answered Ok.
/// - `resolve()`: resolves the instance if it is not yet, reading and decoding
///   its manifest and creating its children Unresolved, and answers Ok, or why
///   it could not. Idempotent: Ok at once for a resolved instance.
/// - `child(index)`: the child at |index| of `decl().children()`, resolved or
///   not. Only on a resolved instance, with an index the decoder checked.
template <typename T>
concept Instance = requires(T &instance, T const &view, u32 const index) {
    { view.parent() } -> std::same_as<T *>;
    { view.index() } -> std::same_as<u32>;
    { view.decl() } -> std::same_as<Decl const &>;
    { instance.resolve() } -> std::same_as<ours::Status>;
    { instance.child(index) } -> std::same_as<T &>;
};

/// One capability the root provides itself: what `from: "parent"` means at the
/// root. The counterpart of a CapabilityDecl in component_manager's
/// `builtin_capabilities()` and `namespace_capabilities()`, which
/// find_builtin_source() searches (legacy_router.rs:541-569 at
/// `9e015f591c4^`). On dev these are `boot` (rx*), `fuchsia.process.Launcher`
/// and `fuchsia.kernel.DebugResource`; the root holds the list.
struct Builtin final {
    Kind kind;
    std::string_view name;
    /// A directory's rights, as `boot`'s rx* in
    /// src/sys/component_manager/configs/bootfs_config.json5:7-14. None for a
    /// protocol.
    ustl::Option<Rights> rights;
};

/// Which declaration a failed walk was looking at when it stopped.
enum class Verb : u32 {
    /// An offer in the parent of the instance the walk had reached.
    Offer = 1,
    /// An expose in a child the walk was turning down into.
    Expose = 2,
    /// A capability declaration: a component's own `capabilities`, or a
    /// built-in, which is component_manager's own declaration.
    Declare = 3,
};

/// The subdirectory a route accumulates on its way to the source: the
/// counterpart of Fuchsia's RelativePath (src/sys/lib/cm_types/src/lib.rs:853),
/// held inline because the root has no heap. Empty means the source's root.
class Subdir final {
  public:
    constexpr Subdir() = default;

    /// Puts |head| in front, joined with one '/': "ours", then prepend("config"),
    /// is "config/ours". An empty |head| changes nothing, and on an empty
    /// Subdir the result is |head| itself. False, with the Subdir unchanged,
    /// when the result would be longer than MAX_PATH_LENGTH, as
    /// RelativePath::extend() (cm_types/src/lib.rs:928-942) refuses. |head|
    /// is not checked further: the compiler checked every subdir it wrote.
    [[nodiscard]]
    auto prepend(std::string_view head) -> bool;

    [[nodiscard]]
    auto view() const -> std::string_view;

    [[nodiscard]]
    auto is_empty() const -> bool {
        return m_size == 0;
    }

  private:
    /// The path ends at the end of the array and grows toward its start, so
    /// that a prepend copies only the new head.
    std::array<char, MAX_PATH_LENGTH> m_bytes{};
    u32 m_size = 0;
};

/// Where a use leads, as route() found it.
template <typename C>
struct Route final {
    /// The instance whose outgoing directory serves the capability, at
    /// `svc/<name>`; nullptr when the source is the built-in named |name| of
    /// the use's kind.
    C *provider;
    /// The capability's name at its source: the name the provider declared,
    /// or the built-in's. A view of that manifest's bytes or of the built-in's
    /// name.
    std::string_view name;
    /// The subdirectory of the source the connection is opened at: every
    /// hop's subdir, the source's side first.
    Subdir subdir;
    /// Exactly the use's rights; none for a protocol use.
    ustl::Option<Rights> rights;
};

/// Why a use leads nowhere, and where the walk stopped.
template <typename C>
struct RouteError final {
    /// NotFound or AccessDenied: Fuchsia's mapping of a routing error to a
    /// status (src/sys/lib/routing/src/error.rs:405-441).
    ours::Status status;
    /// The instance whose declaration the walk stopped at; nullptr for the
    /// built-ins above the root, Fuchsia's ExtendedMoniker::ComponentManager.
    C *at;
    /// Which declaration it was.
    Verb verb;
    /// The name the walk was looking for there.
    std::string_view name;
};

/// Walks |use|, one of |target|'s uses, to the capability's source.
///
/// Written by the user by hand under a time limit (P3 in
/// .agents/tasks/component-framework/PLAN.md, step 8); test/route_test.cpp
/// pins this spec down. Its caller is the root (steps 13 and 14): once per
/// directory use when a component starts (D8), and again on every Open of
/// `/svc/<name>` (D11).
///
/// Inputs
/// - |target|: a resolved instance, so `target.decl()` is valid.
/// - |use|: one of `target.decl().uses()`. Its strings view that manifest's
///   bytes.
/// - |builtins|: what `from: "parent"` means at the root (Builtin).
///
/// Output
/// - A Route: the provider instance and the name it declared, or nullptr and
///   the built-in's name; the joined subdir; the use's rights.
/// - Or a RouteError: the status, the instance whose declaration the walk
///   stopped at (nullptr for the built-ins), the verb and the name it was
///   looking for there.
///
/// The walk. Its state is an instance |at|, where the walk has reached, a Ref
/// |from|, where the capability comes from as seen from |at|, and a |name|,
/// the capability's name as |at| knows it. It starts at (|target|,
/// `use.source`, `use.source_name`); `use.source` is the parent for every use
/// the compiler writes (D3), and any other source the decoder lets through is
/// walked by the same rules. Every lookup below matches the use's kind, and
/// takes the first matching record in table order. Then it repeats:
/// - **from is the parent, and |at| is the root** (`at.parent()` is nullptr):
///   find the built-in named |name|. None: NotFound at nullptr, Declare.
///   Otherwise apply the hop with the built-in's rights and no subdir, and the
///   route ends at that built-in.
/// - **from is the parent:** in the parent P's offers, find the one whose
///   target is the child `at.index()` and whose target_name is |name|. None:
///   NotFound at P, Offer. Otherwise apply the hop with the offer's rights and
///   subdir, and go on at P, from the offer's source, with its source_name.
/// - **from is the child c:** take C = `at.child(c)` and resolve it. If that
///   fails, whatever the status: NotFound at C, Expose, as Fuchsia maps
///   ResolveFailed to NOT_FOUND (src/sys/lib/routing/src/error.rs:66-73). In
///   C's exposes, find the one whose target is the parent and whose
///   target_name is |name|. None: NotFound at C, Expose. If its source is the
///   parent, also NotFound at C, Expose: the compiler never writes one,
///   Fuchsia's ExposeSource cannot even say it
///   (src/sys/lib/cm_rust/src/lib.rs:1139-1146), and refusing it is what keeps
///   the walk from turning back up. Otherwise go on at C, from the expose's
///   source, with its source_name. An expose states no rights and no subdir,
///   so it is no hop for the rules below.
/// - **from is self:** if |at| has no program, or declares no capability named
///   |name|: NotFound at |at|, Declare. A capability with no program to serve
///   it leads nowhere, as Fuchsia answers InstanceNonExecutable, NOT_FOUND
///   (src/sys/component_manager/src/model/component/instance.rs:785-792;
///   lib/errors/src/lib.rs:230). Otherwise the route ends at |at|, under the
///   declared name. A declaration states no rights and no subdir.
///
/// Applying a hop. A hop is the use itself, an offer, or a built-in; each may
/// state rights and a subdir. Rights first, then the subdir:
/// - Rights. A hop that states none narrows nothing and is skipped. One that
///   does must contain every right stated by the last hop before it that
///   stated any, the first being the use: walking from the use toward the
///   source, the stated rights may only grow. Otherwise AccessDenied, at the
///   hop's instance (nullptr for a built-in), with its verb (Offer, or Declare
///   for a built-in) and |name|. A hop's rights are held against the hop
///   before, not against the use: if the use asks r*, the parent offers rx*
///   and the grandparent offered only r*, the parent offered more than it had,
///   and the walk answers AccessDenied at the grandparent.
/// - Subdir. Each hop's subdir goes in front of the one the walk carries
///   (Subdir::prepend()), the use's own being the first, so the source's side
///   comes first in the result. If the result would be too long: NotFound at
///   the hop's instance, with its verb and |name|, as Fuchsia's PathTooLong
///   maps to NOT_FOUND.
///
/// Errors. The first failure on the walk is the answer:
/// - NotFound, Offer, at the parent: the parent offers no capability of that
///   kind and name to this child.
/// - NotFound, Expose, at a child: it cannot be resolved, it exposes no
///   capability of that kind and name, or it exposes one from its parent.
/// - NotFound, Declare, at an instance: it declares no such capability, or has
///   no program to serve the one it declares.
/// - NotFound, Declare, at nullptr: the root's parent offers it, and there is
///   no built-in of that kind and name.
/// - NotFound, at the hop: the joined subdir would be longer than
///   MAX_PATH_LENGTH.
/// - AccessDenied, at the hop: its rights do not contain the previous hop's.
///
/// Invariants it must keep:
/// - The Route's rights are exactly the use's, never a hop's: the connection
///   carries what the component asked for, and the walk only checks that every
///   hop had at least that much.
/// - It calls only the Instance members, and `decl()` only on an instance that
///   is resolved: |target|, an ancestor of a resolved instance, or a child
///   whose resolve() answered Ok. It makes no system call and allocates
///   nothing, so the root can run it inside a Loop callback (D16).
/// - It terminates on every manifest the decoder accepts. A step from the
///   parent moves one level up; a step from a child moves one level down; and
///   once the walk has turned down it never turns up again, since no expose
///   leads to a parent. So it takes at most depth(target) steps up and as many
///   down as the tree is deep: at most twice the depth of the tree.
/// - The strings it returns view a manifest's bytes or |builtins|, and stay
///   valid while those do.
///
/// Not route()'s: whether the provider has exited (D23: the caller answers
/// NotFound for one, as Fuchsia's RouteSourceShutdown); connecting; and the
/// security policy Fuchsia applies after routing, which ours does not have.
///
/// Fuchsia's counterpart is the router of directories until 2025-07-24, at
/// `9e015f591c4^`: route_directory() (src/sys/lib/routing/src/lib.rs:930-980,
/// the walk from the use at :952-975) over route_from_use()
/// (legacy_router.rs:56). Its steps:
/// - from the parent: Use::route() (:652-698) and Offer::route_segment()
///   (:1084-1132), with find_matching_offers() (:1555-1574) matching the kind,
///   the target name and the target child; at the root, find_builtin_source()
///   (:541-569);
/// - into a child: change_directions() (:1144-1186) and
///   Expose::route_segment() (:1479-1506), with find_matching_exposes()
///   (:1576-1594) matching the kind, the target name and the parent target;
/// - from self: Offer::route_segment() (:1025-1064) and
///   Expose::route_segment() (:1445-1459), over find_component_source()
///   (:576-601).
///
/// The hop rules are DirectoryState::advance() and finalize() (lib.rs:833-867),
/// over WalkState::advance() and finalize() (walk_state.rs:45-98), whose
/// RightsWalker::validate_next() (rights.rs:93-104; rights.rs:19-33 at
/// `51f05104c13`) holds each hop against the one before, and
/// RelativePath::extend() for the subdir.
template <Instance C>
[[nodiscard]]
auto route(C &target, Use const &use, std::span<Builtin const> builtins)
    -> ustl::Result<Route<C>, RouteError<C>>;

// The spec is the comment on route() above. Until the user writes this body,
// route() finds nothing and answers NotSupported, which every test of it in
// test/route_test.cpp refuses.
template <Instance C>
auto route([[maybe_unused]] C &target, [[maybe_unused]] Use const &use,
           [[maybe_unused]] std::span<Builtin const> const builtins)
    -> ustl::Result<Route<C>, RouteError<C>> {
    // Written by the user (P3): see the spec above.
    return ustl::err(RouteError<C>{
        .status = ours::Status::NotSupported,
        .at = nullptr,
        .verb = Verb::Declare,
        .name = {},
    });
}
} // namespace component

#endif // #ifndef COMPONENT_ROUTE_HPP
