/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FSIO_FSIO_HPP
#define FSIO_FSIO_HPP 1

#include <fsio/namespace.hpp>

#include <span>
#include <string_view>

#include <fuchsia_io/fuchsia_io.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

/// The calls a program makes through its namespace: ours's counterpart of the
/// part of fdio a component uses to reach its files and protocols
/// (component-framework design, "Events: a component opening a file in
/// /boot"). The namespace itself is <fsio/namespace.hpp>, and paths are
/// cleaned by <fsio/path.hpp>; this part makes system calls, so it exists in
/// user programs only.
///
/// Every call speaks the fuchsia.io subset (<fuchsia_io/fuchsia_io.hpp>) on
/// a channel and blocks the calling thread until the reply comes. A process
/// has one thread, so nothing here is locked, and one call is outstanding at
/// a time.
///
/// There are no file descriptors: a file or a directory is the client end of
/// its connection, an ours::Uandle the caller owns and closes, with close()
/// or ours::handle_close().
namespace fsio {
/// The namespace the process was started with, once install() has run:
/// fdio_ns_get_installed()'s. open() and connect() resolve paths in it.
[[nodiscard]]
auto installed_namespace() -> Namespace &;

/// Binds each NsDir startup handle at its name, as fdio does when it reads
/// processargs (__libc_extensions_init(), sdk/lib/fdio/unistd.cc): for the
/// i-th name of the processargs message, the NsDir handle whose arg is i.
/// fdio runs this before main; here main calls it.
///
/// The handles are taken from the runtime (ort::take_startup_handle()), so
/// nothing else can claim them after. fdio instead leaves them in its
/// startup array for "higher level code that may also need access to the
/// namespace", which ours's take-once table does not allow.
///
/// A name with no handle is skipped. The first bind that fails stops the
/// install and is the answer, its handle closed, as fdio_ns_bind() closes
/// the handle it could not bind; fdio panics there instead. What was bound
/// before it stays bound, and later names are left unclaimed. Calling it
/// again binds nothing more, since every handle was taken the first time.
[[nodiscard]]
auto install() -> ours::Status;

/// Opens |path| in the installed namespace on |object|, the server end of a
/// channel the caller made, and returns without waiting: fdio_open3().
///
/// The path is cleaned (clean_path()) and resolved (Namespace::resolve()),
/// and Directory.Open is written on the directory it lies under, carrying
/// the rest of the path and |flags| as they are. Open is one-way, so the
/// caller may write its first request on the other end at once; if the
/// Open fails, the server says so with an epitaph on |object|, and the first
/// call on that end answers the epitaph's status.
///
/// |object| is consumed whatever the answer. The answer is this side's
/// alone: clean_path()'s and resolve()'s refusals (NotFound for a relative
/// path, there being no working directory), or the write's.
///
/// One case reads the channel. When the write finds the directory's server
/// gone, the server may have left an epitaph saying why: the root closes a
/// namespace entry it could not route that way before the component runs
/// (component-framework design D8). open() reads it, answers its status
/// (AccessDenied for an offer that narrowed the rights, say), and records it
/// in the binding (Namespace::mark_closed()), since an epitaph can be read
/// only once; every later open() and connect() under the entry answers the
/// same status from there. Without an epitaph, or with an Ok one, the
/// answer is PeerClosed, recorded the same way.
[[nodiscard]]
auto open(std::string_view path, fuchsia_io::Flags flags, ours::Uandle object) -> ours::Status;

/// open() on a new channel, returning the client end: the caller owns it.
/// fdio_open3() with the endpoints made for the caller.
[[nodiscard]]
auto open(std::string_view path, fuchsia_io::Flags flags)
    -> ustl::Result<ours::Uandle, ours::Status>;

/// Connects |server| to the protocol at |path|, such as
/// "/svc/fuchsia.process.Launcher": open() with PROTOCOL_SERVICE alone, as
/// fdio_service_connect() opens it. |server| is consumed whatever the
/// answer, and from then on carries the protocol, not fuchsia.io.
[[nodiscard]]
auto connect(std::string_view path, ours::Uandle server) -> ours::Status;

/// One namespace entry handed on: a path and a channel to the directory
/// bound there, which the caller owns. An element of fdio's
/// fdio_flat_namespace_t.
struct ExportedEntry {
    /// The bound path: a view of the installed namespace's own copy, valid
    /// until the namespace next changes.
    std::string_view path;
    ours::Uandle directory;
};

/// Clones each entry of the installed namespace into |out|, in the order the
/// entries were bound, and returns how many it filled: fdio_ns_export_root()
/// (fdio_namespace::Export(), sdk/lib/fdio/namespace/local-filesystem.cc).
/// Each clone is a new connection to the same directory with the same
/// rights, made with fuchsia.unknown/Cloneable.Clone as zxio_clone() makes
/// it, without waiting: Clone is one-way.
///
/// An entry whose server has closed it is left out: one marked closed
/// already (Namespace::mark_closed()), and one whose Clone finds the server
/// gone, which is then marked from its epitaph, as open() marks it. fdio
/// fails the whole export there instead. Here the root closes a namespace
/// entry it could not route before the component runs (component-framework
/// design D8), so a component may hold one for good, and every spawn it made
/// would fail; a clone of such an entry could only carry the same refusal.
///
/// NoResource when |out| has fewer places than there are live entries;
/// otherwise the channel calls' own errors. On an error every clone made so
/// far is closed.
[[nodiscard]]
auto export_namespace(std::span<ExportedEntry> out) -> ustl::Result<usize, ours::Status>;

/// Starts the program at |path| in a new process under |job|, and returns
/// the process, which the caller owns: fdio_spawn() with
/// FDIO_SPAWN_CLONE_JOB | FDIO_SPAWN_CLONE_NAMESPACE (sdk/lib/fdio/spawn.cc),
/// through the fuchsia.process.Launcher in the installed namespace's /svc
/// (component-framework design D12, "Events: the shell running a program").
///
/// In fdio_spawn's order:
///   1. opens |path| with READ_BYTES and EXECUTE, and takes its VMO with
///      READ, EXECUTE and PRIVATE_CLONE, the flags fdio_get_vmo_exec() asks
///      for;
///   2. connects to /svc/fuchsia.process.Launcher;
///   3. sends AddArgs with |argv|, AddHandles with a duplicate of |job| as
///      JobDefault, and AddNames with a clone of every live namespace entry
///      (export_namespace()), when there is one;
///   4. sends Launch with the VMO, another duplicate of |job| and the name
///      argv[0], cut to ours::MAX_NAME_LEN - 1 bytes as fdio cuts it, and
///      waits for the answer.
///
/// |job| is not consumed. Nothing else is handed on: no environment, no
/// standard I/O and no loader service, none of which ours has.
///
/// Errors, besides the system calls':
///   - InvArgs for an empty |argv|, or arguments one AddArgs cannot carry;
///   - open()'s and get_backing_memory()'s: NotFound for a path nothing is
///     bound under or a file the directory does not have, AccessDenied for a
///     file that is not executable or a namespace entry without EXECUTE;
///   - export_namespace()'s;
///   - the Launcher's answer: AccessDenied for a VMO without Execute, InvArgs
///     for more than a processargs can carry, and launch::start()'s own;
///   - the route's, read from the epitaph the root closes the Launcher
///     connection with when /svc offers no Launcher: NotFound, say.
[[nodiscard]]
auto spawn(ours::Uandle job, std::string_view path, std::span<std::string_view const> argv)
    -> ustl::Result<ours::Uandle, ours::Status>;

/// One File.Read on |file|: up to buffer.size() bytes, at most
/// fuchsia_io::MAX_TRANSFER_SIZE, from the connection's seek offset, which
/// moves past them. Returns how many came, 0 at the end of the file; fewer
/// than asked for is not an error, so a caller that wants more calls again.
///
/// Errors: the server's status (AccessDenied without READ_BYTES, say); an
/// epitaph's status, when the connection was closed with one -- a failed
/// Open, for instance -- told apart from a reply by its header's ordinal
/// (wire::is_epitaph()) before the reply is decoded; PeerClosed when it was
/// closed without one, or with an Ok epitaph; InvArgs for a reply that is
/// not a well-formed answer to this request, or a malformed epitaph; and the
/// channel calls' own.
[[nodiscard]]
auto read(ours::Uandle file, std::span<u8> buffer) -> ustl::Result<usize, ours::Status>;

/// One Directory.ReadDirents on |directory|: as many whole entries as fit in
/// buffer.size() bytes, at most fuchsia_io::MAX_BUF, from the directory's
/// cursor, which moves past them. Returns how many bytes were filled, 0 once
/// every entry has been read; fuchsia_io::DirentReader reads them back.
/// Errors as read(); BufferTooSmall from the server when the next entry
/// alone does not fit.
[[nodiscard]]
auto read_dirents(ours::Uandle directory, std::span<u8> buffer)
    -> ustl::Result<usize, ours::Status>;

/// File.GetBackingMemory on |file|: a VMO of the file's contents with the
/// rights |flags| ask for, which the caller owns. To run a program, ask for
/// READ, EXECUTE and PRIVATE_CLONE, as fdio_get_vmo_exec() does through
/// zxio_vmo_get_exec() (sdk/lib/zxio/get_vmo.cc); the server refuses EXECUTE
/// unless the file is executable and the connection holds EXECUTE. Errors as
/// read().
[[nodiscard]]
auto get_backing_memory(ours::Uandle file, fuchsia_io::VmoFlags flags)
    -> ustl::Result<ours::Uandle, ours::Status>;

/// Node.Close on |node|, then closes |node| itself, whatever the answer, as
/// zxio_close() does. Returns the server's status, or why the call failed;
/// either way the handle is gone. A caller that does not care for the
/// server's answer can just close the handle: the server then sees
/// PEER_CLOSED and frees the connection all the same.
[[nodiscard]]
auto close(ours::Uandle node) -> ours::Status;
} // namespace fsio

#endif // #ifndef FSIO_FSIO_HPP
