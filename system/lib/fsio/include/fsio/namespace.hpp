/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FSIO_NAMESPACE_HPP
#define FSIO_NAMESPACE_HPP 1

#include <fsio/path.hpp>

#include <array>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace fsio {
/// How many directories one namespace holds. A component's namespace has one
/// entry per directory it uses and one for /svc (component-framework design,
/// "Events: starting a component"), and a processargs message carries at most
/// 64 handles; fdio's tree has no limit, but it has a heap.
inline constexpr auto MAX_BINDINGS = usize{16};

/// How many bytes the bound paths take together, with nothing between them.
/// "/boot" and "/svc" take 9.
inline constexpr auto BOUND_PATHS_CAPACITY = usize{1024};

/// One binding of a Namespace, as Namespace::entry() hands it out.
struct Entry {
    /// The bound path, clean and absolute: a view of the namespace's own
    /// copy, valid until the namespace next changes.
    std::string_view path;
    /// The channel bound there, still the namespace's.
    ours::Uandle directory;
    /// Ok while its server keeps it open; once Namespace::mark_closed() has
    /// recorded that the server closed it, the status recorded.
    ours::Status closed;
};

/// What Namespace::resolve() found a path under: the directory bound at the
/// path's mount point, and the rest of the path below it, "." for the mount
/// point itself. |rest| points into the path that was resolved, and is what
/// an Open written on |directory| carries.
struct Resolved {
    ours::Uandle directory;
    std::string_view rest;
};

/// A process's namespace: absolute paths, each bound to a channel to a
/// directory served by someone else. Ours's counterpart of fdio's fdio_ns_t
/// (sdk/lib/fdio/namespace/local-filesystem.cc), with the same rules for what
/// may be bound where, kept as a flat table rather than fdio's tree of nodes,
/// since the rules below keep the two equivalent.
///
/// No two bound paths overlap: none lies under another. So a path lies under
/// at most one of them, and the longest bound prefix that fdio's walk finds
/// is the only one there is. fdio forbids the overlap in both directions:
///
///   - binding below a mount point would shadow part of that directory
///     ("Encountering a valid storage end at any point in the bind path
///     implies shadowing, which is not supported");
///   - binding above one would hide it (an "overlay").
///
/// Holds no memory but its own (MAX_BINDINGS entries and
/// BOUND_PATHS_CAPACITY bytes of paths), and makes no system call: it holds
/// the channels and never closes one, since unbind() hands the channel back.
/// So it builds on the host, where its tests run. No lock either: a process
/// has one thread (ort's startup-handle table makes the same assumption,
/// <ort/startup_handles.hpp>).
///
/// constexpr-constructible, because nothing runs a user program's dynamic
/// initialisers (//system/lib/ort, src/start.cpp): a namespace that is a
/// global has to be constinit.
class Namespace final {
  public:
    constexpr Namespace() = default;

    /// Binds |directory| at |path|, after cleaning it (clean_path()), as
    /// fdio_ns_bind() does. On Ok the namespace holds |directory|; on any
    /// failure the caller still does, and nothing changed. In the order they
    /// are checked:
    ///
    ///   BadPath        clean_path() refused |path|;
    ///   BadHandle      |directory| is Uandle::Invalid;
    ///   InvArgs        |path| is not absolute;
    ///   BadPath        one of its names is longer than
    ///                  fuchsia_io::MAX_NAME_LENGTH;
    ///   AlreadyExists  |path| is bound already; or other paths are bound
    ///                  below it, so fdio finds it as a node it made for
    ///                  them, unless |path| is "/";
    ///   NotSupported   |path| lies below a bound path (shadowing), or it is
    ///                  "/" and other paths are bound (an overlay);
    ///   NoResource     MAX_BINDINGS or BOUND_PATHS_CAPACITY would be
    ///                  exceeded, where fdio would fail to allocate.
    ///
    /// fdio walks the path one name at a time and reports what it meets
    /// first, so for a path with a name that is too long it may answer
    /// NotSupported before it reaches that name. Here every name is checked
    /// first, so that answer is always BadPath.
    [[nodiscard]]
    auto bind(std::string_view path, ours::Uandle directory) -> ours::Status;

    /// Removes the binding at |path|, cleaned first, and hands its channel
    /// back to the caller, who closes it; fdio_ns_unbind() closes it itself.
    /// Errors, in the order they are checked:
    ///
    ///   BadPath   clean_path() refused |path|;
    ///   InvArgs   |path| is not absolute;
    ///   BadPath   one of its names is longer than
    ///             fuchsia_io::MAX_NAME_LENGTH, or "/" is bound and |path| is
    ///             not "/": fdio's walk starts at a root that is a mount
    ///             point, which it refuses to look into;
    ///   NotFound  nothing is bound at exactly |path|, whether |path| lies
    ///             below a bound path, above one, or apart from all of them.
    [[nodiscard]]
    auto unbind(std::string_view path) -> ustl::Result<ours::Uandle, ours::Status>;

    /// The directory |path| lies under, and the rest of |path| below it, as
    /// fdio_ns_open3() finds them before it writes Open. In the order they
    /// are checked:
    ///
    ///   NotFound      |path| is not absolute (fdio's OpenRemote: "Require
    ///                 that we start at /");
    ///   BadPath       one of its names is longer than
    ///                 fuchsia_io::MAX_NAME_LENGTH;
    ///   NotSupported  no bound path is |path| or above it, but |path| is
    ///                 "/" or some are below it: |path| names one of the
    ///                 nodes fdio makes to hold mount points, the root among
    ///                 them, which has nothing to connect to ("Cannot connect
    ///                 to non-mount-points");
    ///   NotFound      no bound path is |path|, above it or below it;
    ///   any other     |path| lies under a binding whose server closed it,
    ///                 and this is the status mark_closed() recorded for it.
    ///
    /// fdio checks a name's length only where its walk reaches it: up to the
    /// first name below the mount point, and not past a name it cannot find.
    /// Here every name is checked, as in bind() and unbind(); a name below
    /// the mount point is one the server would refuse with BadPath anyway
    /// (io.fidl, Name).
    ///
    /// Takes a PathBuffer because the path must be clean: a ".." would
    /// otherwise climb out of a mount point on the server's side of the
    /// channel, and fuchsia.io refuses one anyway (io.fidl, Path).
    [[nodiscard]]
    auto resolve(PathBuffer const &path) const -> ustl::Result<Resolved, ours::Status>;

    /// Records that the server of the binding holding |directory| has closed
    /// it, and why: from now on resolve() answers |status| for every path
    /// under it, without a channel being touched. The binding keeps its
    /// channel, which unbind() still hands back.
    ///
    /// This is how an entry the root could not route reads (component-framework
    /// design D8). The root routes a directory use when it starts the
    /// component, and when the route fails it closes the entry's server end
    /// with an epitaph before the component runs. fsio::open() finds the
    /// channel closed, reads that epitaph once, and records its status here,
    /// since the epitaph is the channel's last message and cannot be read
    /// twice; every later open() under the entry answers the same status.
    /// fdio has no such state: its Open on a closed channel fails, and the
    /// reason is lost.
    ///
    /// InvArgs for |status| Ok, which would mark nothing; NotFound when no
    /// binding holds |directory|. The first status recorded stays: a binding
    /// closes once.
    [[nodiscard]]
    auto mark_closed(ours::Uandle directory, ours::Status status) -> ours::Status;

    /// How many paths are bound.
    [[nodiscard]]
    auto size() const -> usize {
        return m_count;
    }

    /// The binding at |index|, in the order the paths were bound: what
    /// fdio's export walks (fdio_namespace::Export(), local-filesystem.cc),
    /// for fsio::export_namespace() to clone. None for an index at or past
    /// size(). unbind() moves the bindings after the one it removes down by
    /// one, so an index names the same binding only until the next unbind().
    [[nodiscard]]
    auto entry(usize index) const -> ustl::Option<Entry>;

  private:
    /// One bound path: its channel, where its text lies in m_paths, and, once
    /// its server has closed it, the status resolve() answers under it (Ok
    /// while it is open).
    struct Binding {
        ours::Uandle directory = ours::Uandle::Invalid;
        u16 offset = 0;
        u16 size = 0;
        ours::Status closed = ours::Status::Ok;
    };
    static_assert(BOUND_PATHS_CAPACITY <= 0xFFFF, "a Binding's offset and size are u16");

    [[nodiscard]]
    auto path_of(Binding const &binding) const -> std::string_view;

    /// The binding at exactly |path|, or MAX_BINDINGS when there is none.
    [[nodiscard]]
    auto find(std::string_view path) const -> usize;

    /// The first m_count entries are in use, in the order they were bound.
    std::array<Binding, MAX_BINDINGS> m_bindings{};
    usize m_count = 0;

    /// The bound paths' text, back to back in the order of m_bindings, with
    /// no NUL: m_paths_size bytes are in use. unbind() closes the gap it
    /// leaves.
    std::array<char, BOUND_PATHS_CAPACITY> m_paths{};
    usize m_paths_size = 0;
};
} // namespace fsio

#endif // #ifndef FSIO_NAMESPACE_HPP
