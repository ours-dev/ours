/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FSIO_FSIO_HPP
#define FSIO_FSIO_HPP 1

#include <fsio/namespace.hpp>

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours_io/ours_io.hpp>
#include <ustl/result.hpp>

/// The calls a program makes through its namespace to reach its files and
/// protocols (component-framework design, "Events: a component opening a
/// file in /boot"). The namespace itself is <fsio/namespace.hpp>, and paths are
/// cleaned by <fsio/path.hpp>; this part makes system calls, so it exists in
/// user programs only.
///
/// Every call speaks ours.io (<ours_io/ours_io.hpp>) on a channel and blocks
/// the calling thread until the reply comes. A process has one thread, so
/// nothing here is locked, and one call is outstanding at a time.
///
/// There are no file descriptors: a file or a directory is the client end of
/// its connection, an ours::Uandle the caller owns and closes, with close()
/// or ours::handle_close().
namespace fsio {
/// The namespace the process was started with, once install() has run.
/// open() and connect() resolve paths in it.
[[nodiscard]]
auto installed_namespace() -> Namespace &;

/// Binds each NsDir startup handle at its name: for the i-th name of the
/// processargs message, the NsDir handle whose arg is i. The runtime runs it
/// before main (ort_extensions_init(), which fsio defines), so main can open
/// files at once; main calls it only to learn how that went.
///
/// The handles are taken from the runtime (ort::take_startup_handle()), so
/// nothing else can claim them after: the runtime's startup table hands each
/// handle out once, and the namespace is their one owner from then on.
///
/// A name with no handle is skipped. The first bind that fails stops the
/// install and is the answer, its handle closed, rather than ending the
/// process: the caller decides whether it can run without the rest. What was
/// bound before it stays bound, and later names are left unclaimed. Only the
/// first call binds; every later one answers what the first one did.
[[nodiscard]]
auto install() -> ours::Status;

/// Opens |path| in the installed namespace on |object|, the server end of a
/// channel the caller made, and returns without waiting.
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
auto open(std::string_view path, ours_io::Flags flags, ours::Uandle object) -> ours::Status;

/// open() on a new channel, returning the client end: the caller owns it.
[[nodiscard]]
auto open(std::string_view path, ours_io::Flags flags) -> ustl::Result<ours::Uandle, ours::Status>;

/// Opens |path| relative to the directory connection |dir| -- the |dir|
/// handle's own "." as the starting directory, the path never touching this
/// process's namespace. A path with a leading '/' or a ".." is BadPath: a
/// directory connection has no above, the namespace's mount points not
/// being reachable from here. The flags, the checks and the answer are the
/// namespace open's.
[[nodiscard]]
auto open(ours::Uandle dir, std::string_view path, ours_io::Flags flags)
    -> ustl::Result<ours::Uandle, ours::Status>;

/// Connects |server| to the protocol at |path|, such as
/// "/svc/ours.process.Launcher": open() with PROTOCOL_SERVICE alone, which
/// asks for the protocol itself rather than a node. |server| is consumed
/// whatever the answer, and from then on carries the protocol, not ours.io.
[[nodiscard]]
auto connect(std::string_view path, ours::Uandle server) -> ours::Status;

/// One namespace entry handed on: a path and a channel to the directory
/// bound there, which the caller owns.
struct ExportedEntry {
    /// The bound path: a view of the installed namespace's own copy, valid
    /// until the namespace next changes.
    std::string_view path;
    ours::Uandle directory;
};

/// Clones each entry of the installed namespace into |out|, in the order the
/// entries were bound, and returns how many it filled. Each clone is a new
/// connection to the same directory with the same rights, made with
/// Cloneable.Clone without waiting: Clone is one-way.
///
/// An entry whose server has closed it is left out: one marked closed
/// already (Namespace::mark_closed()), and one whose Clone finds the server
/// gone, which is then marked from its epitaph, as open() marks it. Failing
/// the whole export there would be wrong: the root closes a namespace entry
/// it could not route before the component runs (component-framework design
/// D8), so a component may hold one for good, and every spawn it made would
/// fail; a clone of such an entry could only carry the same refusal.
///
/// NoResource when |out| has fewer places than there are live entries;
/// otherwise the channel calls' own errors. On an error every clone made so
/// far is closed.
[[nodiscard]]
auto export_namespace(std::span<ExportedEntry> out) -> ustl::Result<usize, ours::Status>;

/// Starts the program at |path| in a new process under |job|, and returns
/// the process, which the caller owns. The child gets the caller's job and a
/// clone of its namespace, through the ours.process.Launcher in the installed
/// namespace's /svc (component-framework design D12, "Events: the shell
/// running a program").
///
/// In order:
///   1. opens |path| with READ_BYTES and EXECUTE, and takes its VMO with
///      READ, EXECUTE and PRIVATE_CLONE: a private copy-on-write view of the
///      file, which the process maps its code from;
///   2. connects to /svc/ours.process.Launcher;
///   3. sends AddArgs with |argv|, AddHandles with a duplicate of |job| as
///      JobDefault, and AddNames with a clone of every live namespace entry
///      (export_namespace()), when there is one;
///   4. sends Launch with the VMO, another duplicate of |job| and the name
///      argv[0], cut to ours::MAX_NAME_LEN - 1 bytes so that it keeps room
///      for the NUL a process name ends with, and waits for the answer.
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
/// ours_io::MAX_TRANSFER_SIZE, from the connection's seek offset, which
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

/// One File.Write on |file|: |data| at the file's seek offset, which moves
/// past what was written. Returns how many bytes were written, at most
/// ours_io::MAX_TRANSFER_SIZE. Errors as read(); AccessDenied from a server
/// the file was not opened writable on.
[[nodiscard]]
auto write(ours::Uandle file, std::span<u8 const> data) -> ustl::Result<usize, ours::Status>;

/// One Directory.ReadDirents on |directory|: as many whole entries as fit in
/// buffer.size() bytes, at most ours_io::MAX_BUF, from the directory's
/// cursor, which moves past them. Returns how many bytes were filled, 0 once
/// every entry has been read; ours_io::DirentReader reads them back.
/// Errors as read(); BufferTooSmall from the server when the next entry
/// alone does not fit.
[[nodiscard]]
auto read_dirents(ours::Uandle directory, std::span<u8> buffer)
    -> ustl::Result<usize, ours::Status>;

/// Directory.Unlink on the entry |path| names: the name goes, and with it
/// what it named. NotFound when nothing carries the name.
auto unlink(std::string_view path) -> ours::Status;

/// Directory.Unlink on |name| under the directory |dir| names -- the
/// handle form of unlink(path), for a caller already holding the parent:
/// the message goes on that connection, whose rights gate it as any other.
/// NotFound when nothing carries the name.
auto unlink_at(ours::Uandle dir, std::string_view name) -> ours::Status;

/// Sets this process's working directory to |path| -- absolute, or relative
/// to the working directory already set -- with |flags| as the working
/// directory's rights: a relative open later may ask no more than chdir
/// did, so chdir a directory with PERM_READABLE for reading and with
/// PERM_READABLE | PERM_WRITABLE for creating and writing. From a
/// successful chdir() on, open() accepts relative paths: "a/b" names a/b
/// under the working directory. A ".." that would climb above the working
/// directory is BadPath -- a directory handle has no above. The working
/// directory is per process and not inherited by spawned processes.
auto chdir(std::string_view path, ours_io::Flags flags) -> ours::Status;

/// Sets this process's working directory to |path| -- absolute, or relative
/// to the working directory already set. From a successful chdir() on,
/// open() accepts relative paths: "a/b" names a/b under the working
/// directory. A ".." that would climb above the working directory is
/// BadPath -- a directory handle has no above. The working directory is
/// per process and not inherited by spawned processes.
auto chdir(std::string_view path) -> ours::Status;

/// Lists the directory |path| into |out| as packed ours.io dirents -- the
/// whole listing in one call: opens |path|, reads entries until the
/// directory runs out, closes. The "." entry is left out; walk |out| with
/// ours_io::DirentReader. BufferTooSmall when |out| cannot hold the
/// listing; other errors as open() and read_dirents() answer them.
[[nodiscard]]
auto list(std::string_view path, std::span<u8> out) -> ustl::Result<usize, ours::Status>;

/// Sets this process's working directory to |path| -- absolute, or relative
/// to the working directory already set -- with |flags| as the working
/// directory's rights: a relative open later may ask no more than chdir
/// did, so chdir a directory with PERM_READABLE for reading and with
/// PERM_READABLE | PERM_WRITABLE for creating and writing. From a
/// successful chdir() on, open() accepts relative paths: "a/b" names a/b
/// under the working directory. A ".." that would climb above the working
/// directory is BadPath -- a directory handle has no above. The working
/// directory is per process and not inherited by spawned processes.
auto chdir(std::string_view path, ours_io::Flags flags) -> ours::Status;

/// File.GetBackingMemory on |file|: a VMO of the file's contents with the
/// rights |flags| ask for, which the caller owns. To run a program, ask for
/// READ, EXECUTE and PRIVATE_CLONE, as spawn() does; the server refuses EXECUTE
/// unless the file is executable and the connection holds EXECUTE. Errors as
/// read().
[[nodiscard]]
auto get_backing_memory(ours::Uandle file, ours_io::VmoFlags flags)
    -> ustl::Result<ours::Uandle, ours::Status>;

/// Node.Close on |node|, then closes |node| itself, whatever the answer, so
/// that the handle never outlives the call. Returns the server's status, or why
/// the call failed; either way the handle is gone. A caller that does not care
/// for the server's answer can just close the handle: the server then sees
/// PEER_CLOSED and frees the connection all the same.
[[nodiscard]]
auto close(ours::Uandle node) -> ours::Status;
} // namespace fsio

#endif // #ifndef FSIO_FSIO_HPP
