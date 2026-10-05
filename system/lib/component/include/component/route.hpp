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
/// What route() needs of the root's record of one component instance: its
/// place in the tree, its manifest, and a way to resolve it on demand.
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
/// root. On dev these are `boot` (rx*), `ours.process.Launcher`, and the
/// ours.resource protocols `ours.resource.Debug`, `ours.resource.Irq` and
/// `ours.resource.Ioport`; the root holds the list.
struct Builtin final {
    Kind kind;
    std::string_view name;
    /// A directory's rights, such as `boot`'s rx*. None for a protocol.
    ustl::Option<Rights> rights;
};

/// Which declaration a failed walk was looking at when it stopped.
enum class Verb : u32 {
    /// An offer in the parent of the instance the walk had reached.
    Offer = 1,
    /// An expose in a child the walk was turning down into.
    Expose = 2,
    /// A capability declaration: a component's own `capabilities`, or a
    /// built-in, which the root declares itself.
    Declare = 3,
    /// The use itself, in the target's manifest: the first hop of every walk.
    /// Only its own subdir can fail it, by being longer than MAX_PATH_LENGTH,
    /// which the compiler refuses and the decoder does not.
    Use = 4,
};

/// The subdirectory a route accumulates on its way to the source, held inline
/// because the root has no heap. Empty means the source's root.
class Subdir final {
  public:
    constexpr Subdir() = default;

    /// Puts |head| in front, joined with one '/': "ours", then
    /// prepend("config"), is "config/ours". An empty |head| changes nothing,
    /// and on an empty Subdir the result is |head| itself. False, with the
    /// Subdir unchanged, when the result would be longer than MAX_PATH_LENGTH,
    /// which no Open could carry. |head| is not checked further: the compiler
    /// checked every subdir it wrote.
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
    /// AccessDenied when a hop holds fewer rights than the one before it;
    /// NotFound for every other way a route leads nowhere.
    ours::Status status;
    /// The instance whose declaration the walk stopped at; nullptr for the
    /// built-ins above the root.
    C *at;
    /// Which declaration it was.
    Verb verb;
    /// The name the walk was looking for there.
    std::string_view name;
};

/// route()'s non-template half: the lookups in one manifest and the hop
/// rules, which do not depend on the instance type, so that each program
/// carries one copy of them however many instance types it routes over.
namespace detail {
    /// The first built-in of |kind| named |name|.
    [[nodiscard]]
    auto find_builtin(std::span<Builtin const> builtins, Kind kind, std::string_view name)
        -> ustl::Option<Builtin>;

    /// The first of |decl|'s offers of |kind| to its child at |child| whose
    /// target_name is |name|.
    [[nodiscard]]
    auto find_offer(Decl const &decl, Kind kind, u32 child, std::string_view name)
        -> ustl::Option<Offer>;

    /// The first of |decl|'s exposes of |kind| to its parent whose target_name
    /// is |name|.
    [[nodiscard]]
    auto find_expose(Decl const &decl, Kind kind, std::string_view name) -> ustl::Option<Expose>;

    /// The first of |decl|'s own capabilities of |kind| named |name|.
    [[nodiscard]]
    auto find_capability(Decl const &decl, Kind kind, std::string_view name)
        -> ustl::Option<Capability>;

    /// What the walk carries from one hop to the next, and the two rules every
    /// hop obeys ("Applying a hop" in route()'s spec). It carries the rights
    /// of the last hop that stated any, and the subdir joined so far.
    class Hops final {
      public:
        constexpr Hops() = default;

        /// Applies one hop that states |rights| (none for a hop that states
        /// none) and |subdir| (empty for none). AccessDenied when |rights| do
        /// not contain the last stated ones, checked first; NotFound when the
        /// joined subdir would be longer than MAX_PATH_LENGTH. On a failure
        /// nothing changes, so the error is the hop's alone.
        [[nodiscard]]
        auto apply(ustl::Option<Rights> const &rights, std::string_view subdir) -> ours::Status;

        /// Every subdir applied so far, the latest hop's first.
        [[nodiscard]]
        auto subdir() const -> Subdir const & {
            return m_subdir;
        }

      private:
        ustl::Option<Rights> m_last;
        Subdir m_subdir;
    };
} // namespace detail

/// Walks |use|, one of |target|'s uses, to the capability's source.
///
/// P3 in .agents/tasks/component-framework/PLAN.md reserves this function for
/// the user to write by hand under a time limit (step 8). On the user's
/// instruction of 2026-10-04 the AI wrote it for the MVP instead; the exercise
/// stays open, against this spec and test/route_test.cpp, which pins it down.
/// Its caller is the root (steps 13 and 14): once per directory use when a
/// component starts (D8), and again on every Open of `/svc/<name>` (D11).
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
/// the capability's name as |at| knows it. It first applies the use itself as
/// a hop, at |target| with the verb Use and the name `use.source_name`. It
/// starts at (|target|, `use.source`, `use.source_name`); `use.source` is the
/// parent for every use the compiler writes (D3), and any other source the
/// decoder lets through is walked by the same rules. Every lookup below
/// matches the use's kind, and takes the first matching record in table
/// order. Then it repeats:
/// - **from is the parent, and |at| is the root** (`at.parent()` is nullptr):
///   find the built-in named |name|. None: NotFound at nullptr, Declare.
///   Otherwise apply the hop with the built-in's rights and no subdir, and the
///   route ends at that built-in.
/// - **from is the parent:** in the parent P's offers, find the one whose
///   target is the child `at.index()` and whose target_name is |name|. None:
///   NotFound at P, Offer. Otherwise apply the hop with the offer's rights and
///   subdir, and go on at P, from the offer's source, with its source_name.
/// - **from is the child c:** take C = `at.child(c)` and resolve it. If that
///   fails, whatever the status: NotFound at C, Expose, since to the component
///   that uses the capability a child that cannot be resolved is a route that
///   leads nowhere. In C's exposes, find the one whose target is the parent and
///   whose target_name is |name|. None: NotFound at C, Expose. If its source is
///   the parent, also NotFound at C, Expose: the compiler never writes one, and
///   refusing it is what keeps the walk from turning back up. Otherwise go on
///   at C, from the expose's source, with its source_name. An expose states no
///   rights and no subdir, so it is no hop for the rules below.
/// - **from is self:** if |at| has no program, or declares no capability named
///   |name|: NotFound at |at|, Declare. A capability with no program to serve
///   it leads nowhere. Otherwise the route ends at |at|, under the declared
///   name. A declaration states no rights and no subdir.
///
/// Applying a hop. A hop is the use itself, an offer, or a built-in; each may
/// state rights and a subdir. Its instance and verb, which an error names, are
/// |target| and Use for the use, the parent and Offer for an offer, and
/// nullptr and Declare for a built-in. Rights first, then the subdir:
/// - Rights. A hop that states none narrows nothing and is skipped. One that
///   does must contain every right stated by the last hop before it that
///   stated any, the first being the use: walking from the use toward the
///   source, the stated rights may only grow. Otherwise AccessDenied, at the
///   hop's instance, with its verb and |name|. A hop's rights are held
///   against the hop before, not against the use: if the use asks r*, the
///   parent offers rx* and the grandparent offered only r*, the parent
///   offered more than it had, and the walk answers AccessDenied at the
///   grandparent. The error names the hop that holds less, the declaration
///   that narrowed the route; naming the hop before it instead would answer
///   at the parent here, and at |target| when an offer is narrower than the
///   use.
/// - Subdir. Each hop's subdir goes in front of the one the walk carries
///   (Subdir::prepend()), the use's own being the first, so the source's side
///   comes first in the result. If the result would be too long: NotFound at
///   the hop's instance, with its verb and |name|: no Open could carry the
///   path, so the route leads nowhere. The use's own subdir fails this way only
///   when it is longer than MAX_PATH_LENGTH by itself, which the compiler
///   refuses and the decoder does not: NotFound at |target|, Use.
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
///   MAX_PATH_LENGTH; at |target|, Use, when the use's own subdir is.
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
/// - It terminates on every manifest the decoder accepts, because resolve()
///   fails once the root's fixed instance pool (D27) is full. Step 13's
///   resolve() must keep that true: one that always found room, by growing
///   the pool or by evicting a resolved instance, would take the bound away.
///   The walk is monotone: a step from the parent moves one level up, a step
///   from a child one level down, and once it has turned down it never turns
///   up again, since an expose from the parent is refused. The steps up are
///   at most depth(target). The steps down are not bounded by the manifests:
///   one that names itself as a child, which the decoder accepts, and exposes
///   a capability from that child under the same name, makes a tree with no
///   bottom. Each step down lands on a resolved instance one level deeper
///   than the last, so there are at most as many as the pool holds before a
///   resolve() fails and the walk answers NotFound, Expose.
/// - The strings it returns view a manifest's bytes or |builtins|, and stay
///   valid while those do.
///
/// Not route()'s: whether the provider has exited (D23: the caller answers
/// NotFound for one); connecting; and any security policy applied after
/// routing, which ours does not have.
template <Instance C>
[[nodiscard]]
auto route(C &target, Use const &use, std::span<Builtin const> builtins)
    -> ustl::Result<Route<C>, RouteError<C>>;

// The spec is the comment on route() above; the lookups and the hop rules are
// in detail, in src/route.cpp.
template <Instance C>
auto route(C &target, Use const &use, std::span<Builtin const> const builtins)
    -> ustl::Result<Route<C>, RouteError<C>> {
    auto const fail = [](ours::Status const status, C *const at, Verb const verb,
                         std::string_view const name) {
        return ustl::err(RouteError<C>{.status = status, .at = at, .verb = verb, .name = name});
    };
    auto hops = detail::Hops{};
    // The route carries the use's rights, never a hop's: the walk only proves
    // that every hop had at least that much.
    auto const found = [&use, &hops](C *const provider, std::string_view const name) {
        return ustl::ok(Route<C>{
            .provider = provider,
            .name = name,
            .subdir = hops.subdir(),
            .rights = use.rights,
        });
    };

    // The use is the first hop: its rights are the first any hop states, and
    // its subdir the first joined.
    if (auto const status = hops.apply(use.rights, use.subdir); status != ours::Status::Ok) {
        return fail(status, &target, Verb::Use, use.source_name);
    }

    auto *at = &target;
    auto from = use.source;
    auto name = use.source_name;
    // Up through the parents' offers while the capability comes from a
    // parent, then down through the children's exposes while it comes from a
    // child, until it comes from self or from a built-in. An expose from the
    // parent is refused below, so the walk never turns back up.
    while (from.kind != RefKind::Self) {
        if (from.kind == RefKind::Parent) {
            auto *const parent = at->parent();
            if (parent == nullptr) {
                // Above the root: the built-ins, the walk's last hop.
                auto const builtin = detail::find_builtin(builtins, use.kind, name);
                if (builtin.is_none()) {
                    return fail(ours::Status::NotFound, nullptr, Verb::Declare, name);
                }
                if (auto const status = hops.apply(builtin->rights, {});
                    status != ours::Status::Ok) {
                    return fail(status, nullptr, Verb::Declare, name);
                }
                return found(nullptr, builtin->name);
            }

            auto const offer = detail::find_offer(parent->decl(), use.kind, at->index(), name);
            if (offer.is_none()) {
                return fail(ours::Status::NotFound, parent, Verb::Offer, name);
            }
            if (auto const status = hops.apply(offer->rights, offer->subdir);
                status != ours::Status::Ok) {
                return fail(status, parent, Verb::Offer, name);
            }
            at = parent;
            from = offer->source;
            name = offer->source_name;
            continue;
        }

        // From the child at from.child, the one kind left: the decoder
        // refuses any RefKind but these three. Whatever resolve() says went
        // wrong, the route only says NotFound (route()'s spec, above).
        auto &child = at->child(from.child);
        if (child.resolve() != ours::Status::Ok) {
            return fail(ours::Status::NotFound, &child, Verb::Expose, name);
        }
        auto const expose = detail::find_expose(child.decl(), use.kind, name);
        if (expose.is_none() || expose->source.kind == RefKind::Parent) {
            return fail(ours::Status::NotFound, &child, Verb::Expose, name);
        }
        at = &child;
        from = expose->source;
        name = expose->source_name;
    }

    // From self: |at| serves it, if it has a program to serve anything.
    if (at->decl().program().is_none()) {
        return fail(ours::Status::NotFound, at, Verb::Declare, name);
    }
    auto const capability = detail::find_capability(at->decl(), use.kind, name);
    if (capability.is_none()) {
        return fail(ours::Status::NotFound, at, Verb::Declare, name);
    }
    return found(at, capability->name);
}
} // namespace component

#endif // #ifndef COMPONENT_ROUTE_HPP
