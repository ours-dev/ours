/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <fsio/fsio.hpp>
#include <fsio/path.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <fuchsia_process/fuchsia_process.hpp>
#include <ort/startup.hpp>
#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <wire/wire.hpp>

namespace fsio {
namespace {
    using fuchsia_io::Flags;
    using ours::Status;
    using ours::Uandle;

    /// The process's namespace. constinit: nothing runs a user program's
    /// dynamic initialisers (//system/lib/ort, src/start.cpp).
    constinit Namespace installed;

    /// The txid of the next two-way request. One process-wide counter, as a
    /// process has one thread: a reply carries its request's txid back, so a
    /// reply left over from an earlier call, which failed before it read it,
    /// cannot be taken for the answer to this one.
    constinit u32 txid_counter = 1;

    auto next_txid() -> u32 {
        auto const txid = txid_counter;
        if (txid_counter == wire::MAX_TXID) {
            txid_counter = 1;
        } else {
            txid_counter += 1;
        }
        return txid;
    }

    /// One message read off a channel, its bytes and its handles, with room
    /// for the largest a channel carries: a fixed buffer, because a user
    /// program has no heap, and one that cannot be too small, so that a
    /// read never fails with BufferTooSmall and leaves a message behind.
    ///
    /// Owns the handles that came with it until take_handle() claims one,
    /// and closes the rest when it goes. A server that sent handles nobody
    /// asked for, or a malformed reply with handles in it, would otherwise
    /// leave them in this process's table for good.
    class Incoming final {
      public:
        Incoming() = default;
        Incoming(Incoming const &) = delete;
        auto operator=(Incoming const &) -> Incoming & = delete;

        ~Incoming() {
            for (auto index = usize{0}; index < m_num_handles; ++index) {
                if (m_handles[index] != Uandle::Invalid) {
                    // Nothing claimed it, and nothing can be done if the
                    // close fails: the handle is gone either way.
                    auto _ = ours::handle_close(m_handles[index]);
                }
            }
        }

        /// Reads the next message on |channel|, which must be readable.
        [[nodiscard]]
        auto read(Uandle const channel) -> Status {
            return ours::channel_read(channel, ours::ChannelReadOptions::None, m_bytes.data(),
                                      m_handles.data(), static_cast<u32>(m_bytes.size()),
                                      static_cast<u32>(m_handles.size()), &m_num_bytes,
                                      &m_num_handles);
        }

        [[nodiscard]]
        auto message() const -> wire::Message {
            return wire::Message{
                .bytes = std::span{m_bytes}.first(m_num_bytes),
                .handles = std::span{m_handles}.first(m_num_handles),
            };
        }

        /// Takes |handle|, which the message carried, out of the set closed
        /// at the end: the caller owns it from now on.
        auto take_handle(Uandle const handle) -> void {
            for (auto index = usize{0}; index < m_num_handles; ++index) {
                if (m_handles[index] == handle) {
                    m_handles[index] = Uandle::Invalid;
                    return;
                }
            }
        }

      private:
        std::array<u8, wire::MAX_MESSAGE_BYTES> m_bytes{};
        std::array<Uandle, wire::MAX_MESSAGE_HANDLES> m_handles{};
        u32 m_num_bytes = 0;
        u32 m_num_handles = 0;
    };

    /// Writes |request| on |channel| and reads the message that comes back
    /// into |reply|: the answer, an epitaph, or whatever the server wrote
    /// instead, for the caller to decode. The request's handles are consumed
    /// whatever the answer, as channel_write consumes them.
    ///
    /// PeerClosed when the channel closes with nothing left to read. A
    /// server that closes after an epitaph leaves the epitaph readable, so
    /// that it is what the caller finds.
    ///
    /// That holds even when the server closed before the request went out,
    /// which is the usual case after a failed Open: the server handles the
    /// Open as soon as it runs, so its epitaph is often queued here before
    /// the caller's first request is written. channel_write then answers
    /// PeerClosed, but the epitaph is still readable, so the write's
    /// PeerClosed is not the answer: the wait and the read below are, as
    /// Fuchsia's Rust client ignores a write's PEER_CLOSED (Client::send_raw)
    /// and then reads the epitaph (ClientInner::recv_all). Returning the
    /// write's status would make a failed Open answer NotFound or PeerClosed
    /// depending on which process the scheduler ran first.
    auto call(Uandle const channel, wire::Message const request, Incoming &reply) -> Status {
        if (auto const status =
                ours::channel_write(channel, ours::ChannelWriteOptions::None, request.bytes.data(),
                                    static_cast<u32>(request.bytes.size()), request.handles.data(),
                                    static_cast<u32>(request.handles.size()));
            status != Status::Ok && status != Status::PeerClosed) {
            return status;
        }
        auto observed = ours::Signals::None;
        if (auto const status =
                ours::object_wait_one(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
            status != Status::Ok) {
            return status;
        }
        if (!(observed & ours::Signals::Readable)) {
            return Status::PeerClosed;
        }
        return reply.read(channel);
    }

    /// When |message| is an epitaph, the status the call that read it
    /// answers; none for any other message, which the caller then decodes as
    /// the reply it expects.
    ///
    /// The header is asked first (wire::is_epitaph()), before any reply
    /// decoder sees the message: an epitaph also fails every reply's decoder,
    /// and only its ordinal tells "the server closed the connection and said
    /// why" from "the server sent a malformed reply". An epitaph that is
    /// itself malformed is InvArgs, as any malformed message; an Ok one
    /// closed the connection with nothing wrong, but the request still got
    /// no answer, so PeerClosed.
    auto epitaph_of(wire::Message const message) -> ustl::Option<Status> {
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
        if (header.is_err() || !wire::is_epitaph(header.unwrap())) {
            return ustl::NONE;
        }
        auto const epitaph = wire::decode_epitaph(message);
        if (epitaph.is_err()) {
            return ustl::some(Status::InvArgs);
        }
        if (epitaph.unwrap() == Status::Ok) {
            return ustl::some(Status::PeerClosed);
        }
        return ustl::some(epitaph.unwrap());
    }

    /// Why the server of |directory|, a namespace entry, closed it: the
    /// status of the epitaph it left there, read off the channel, or
    /// PeerClosed when it left none.
    ///
    /// Only the first message is looked at. The root writes nothing on an
    /// entry's server end but the epitaph that closes it (design D8), so
    /// that is the one there is; a reply left over from a call that gave up
    /// on this channel would hide it, and PeerClosed is then the answer.
    auto closed_status(Uandle const directory) -> Status {
        Incoming last;
        if (last.read(directory) != Status::Ok) {
            return Status::PeerClosed;
        }
        auto const epitaph = epitaph_of(last.message());
        if (epitaph.is_none()) {
            return Status::PeerClosed;
        }
        return *epitaph;
    }

    /// A request with a small body: every one but Open.
    using SmallRequest = std::array<u8, wire::STATUS_FORM_SIZE>;

    /// Where spawn() finds the Launcher: the protocol's name under /svc, as
    /// fdio_service_connect_by_name() finds it (sdk/lib/fdio/directory.cc).
    constexpr auto LAUNCHER_PATH = std::string_view{"/svc/fuchsia.process.Launcher"};
    static_assert(LAUNCHER_PATH.ends_with(fuchsia_process::LAUNCHER_NAME));

    /// One handle a call made or was given, closed when it goes out of scope
    /// unless it was handed on: the many ways out of spawn() each leave
    /// nothing behind. launch.cpp keeps a class of the same shape for the
    /// same reason.
    class Owned final {
      public:
        explicit Owned(Uandle const handle) : m_handle(handle) {}
        Owned(Owned const &) = delete;
        auto operator=(Owned const &) -> Owned & = delete;

        ~Owned() {
            if (m_handle != Uandle::Invalid) {
                // Nothing to tell anyone if the close fails: the handle was
                // this call's alone.
                auto _ = ours::handle_close(m_handle);
            }
        }

        [[nodiscard]]
        auto get() const -> Uandle {
            return m_handle;
        }

        /// Hands the handle on, to a write that consumes it; this no longer
        /// closes it.
        [[nodiscard]]
        auto release() -> Uandle {
            return std::exchange(m_handle, Uandle::Invalid);
        }

      private:
        Uandle m_handle;
    };

    /// A handle duplicated from |handle| with the same rights, owned.
    auto duplicate(Uandle const handle) -> ustl::Result<Uandle, Status> {
        auto copy = Uandle::Invalid;
        if (auto const status = ours::handle_duplicate(handle, ours::Rights::SameRights, &copy);
            status != Status::Ok) {
            return ustl::err(status);
        }
        return ustl::ok(copy);
    }

    /// Writes the one-way request |message| on |channel|, consuming its
    /// handles whatever the answer.
    ///
    /// PeerClosed is not an error here, for call()'s reason: a server that
    /// refused the connection, a route that led nowhere say, closed it with
    /// an epitaph, and the two-way request that follows reads that epitaph
    /// and answers its status, where the write could only say PeerClosed.
    auto send(Uandle const channel, wire::Message const message) -> Status {
        auto const status =
            ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                                static_cast<u32>(message.bytes.size()), message.handles.data(),
                                static_cast<u32>(message.handles.size()));
        if (status == Status::PeerClosed) {
            return Status::Ok;
        }
        return status;
    }

    /// Closes the directory of every entry in |entries|: the clones an
    /// export made, which nothing took.
    auto close_entries(std::span<ExportedEntry const> const entries) -> void {
        for (auto const &entry: entries) {
            // Each was this call's own; a close that fails has no one to
            // tell.
            auto _ = ours::handle_close(entry.directory);
        }
    }

    /// The bytes of a Read or ReadDirents Ok reply: at most |limit| of them,
    /// copied into |buffer|. More than asked for is not an answer to the
    /// request: InvArgs.
    auto copy_payload(std::span<u8 const> const payload, usize const limit,
                      std::span<u8> const buffer) -> ustl::Result<usize, Status> {
        if (payload.size() > limit) {
            return ustl::err(Status::InvArgs);
        }
        std::ranges::copy(payload, buffer.begin());
        return ustl::ok(payload.size());
    }
} // namespace

auto installed_namespace() -> Namespace & {
    return installed;
}

auto install() -> Status {
    auto index = u32{0};
    for (auto const name: ort::process_args().names()) {
        // A handle_info's arg is 16 bits, so no handle names a later name.
        // A 4 KiB message holds fewer strings than that anyway.
        if (index > std::numeric_limits<u16>::max()) {
            break;
        }
        auto const directory =
            ort::take_startup_handle(ours::HandleType::NsDir, static_cast<u16>(index));
        index += 1;
        if (directory == Uandle::Invalid) {
            continue;
        }
        if (auto const status = installed.bind(name, directory); status != Status::Ok) {
            // The namespace did not take it, and nothing else will.
            auto _ = ours::handle_close(directory);
            return status;
        }
    }
    return Status::Ok;
}

auto open(std::string_view const path, Flags const flags, Uandle const object) -> Status {
    // Consumed on every path, as the write below consumes it: a caller never
    // has to work out whether it still owns |object|.
    auto const refuse = [object](Status const status) {
        auto _ = ours::handle_close(object);
        return status;
    };

    PathBuffer clean;
    if (auto const status = clean_path(path, clean); status != Status::Ok) {
        return refuse(status);
    }
    auto const resolved = installed.resolve(clean);
    if (resolved.is_err()) {
        return refuse(resolved.unwrap_err());
    }

    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, 1> handles{};
    auto const encoded = fuchsia_io::encode_open(bytes, handles,
                                                 fuchsia_io::OpenRequest{
                                                     .flags = flags,
                                                     .path = resolved->rest,
                                                     .object = object,
                                                 });
    if (encoded.is_err()) {
        return refuse(encoded.unwrap_err());
    }
    auto const message = encoded.unwrap();
    auto const written =
        ours::channel_write(resolved->directory, ours::ChannelWriteOptions::None,
                            message.bytes.data(), static_cast<u32>(message.bytes.size()),
                            message.handles.data(), static_cast<u32>(message.handles.size()));
    if (written != Status::PeerClosed) {
        return written;
    }
    // The entry's server has closed it. When the root could not route the
    // entry, it closed the server end with an epitaph before this process
    // ran (design D8), and the epitaph says why: AccessDenied for rights an
    // offer narrowed, NotFound for a capability nobody offers. It is the
    // channel's last message and can be read once, so its status is kept in
    // the binding, and every later open() under this entry answers it from
    // there, through resolve(), without touching the channel.
    auto const closed = closed_status(resolved->directory);
    // Cannot fail: the binding was found just now, and |closed| is never Ok.
    auto _ = installed.mark_closed(resolved->directory, closed);
    return closed;
}

auto open(std::string_view const path, Flags const flags) -> ustl::Result<Uandle, Status> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = open(path, flags, server); status != Status::Ok) {
        auto _ = ours::handle_close(client);
        return ustl::err(status);
    }
    return ustl::ok(client);
}

auto connect(std::string_view const path, Uandle const server) -> Status {
    return open(path, Flags::ProtocolService, server);
}

auto export_namespace(std::span<ExportedEntry> const out) -> ustl::Result<usize, Status> {
    auto filled = usize{0};
    auto const fail = [&out, &filled](Status const status) -> ustl::Result<usize, Status> {
        close_entries(out.first(filled));
        return ustl::err(status);
    };
    for (auto index = usize{0}; index < installed.size(); ++index) {
        auto const entry = *installed.entry(index);
        if (entry.closed != Status::Ok) {
            // Its server closed it and said why (mark_closed()): there is
            // nothing to clone.
            continue;
        }
        if (filled == out.size()) {
            return fail(Status::NoResource);
        }
        auto client = Uandle::Invalid;
        auto server = Uandle::Invalid;
        if (auto const status =
                ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
            status != Status::Ok) {
            return fail(status);
        }
        Owned clone(client);
        std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
        std::array<Uandle, 1> handles{};
        auto const encoded =
            fuchsia_io::encode_clone(bytes, handles, fuchsia_io::CloneRequest{.request = server});
        if (encoded.is_err()) {
            auto _ = ours::handle_close(server);
            return fail(encoded.unwrap_err());
        }
        auto const message = encoded.unwrap();
        auto const written =
            ours::channel_write(entry.directory, ours::ChannelWriteOptions::None,
                                message.bytes.data(), static_cast<u32>(message.bytes.size()),
                                message.handles.data(), static_cast<u32>(message.handles.size()));
        if (written == Status::PeerClosed) {
            // The server is gone, and may have said why: the root closes an
            // entry it could not route with an epitaph (design D8). Marked as
            // open() marks it, and left out; the clone's server end went with
            // the write.
            auto _ = installed.mark_closed(entry.directory, closed_status(entry.directory));
            continue;
        }
        if (written != Status::Ok) {
            return fail(written);
        }
        out[filled] = ExportedEntry{.path = entry.path, .directory = clone.release()};
        filled += 1;
    }
    return ustl::ok(filled);
}

auto spawn(Uandle const job, std::string_view const path,
           std::span<std::string_view const> const argv) -> ustl::Result<Uandle, Status> {
    // fdio_spawn() names the process after argv[0], so there must be one.
    if (argv.empty()) {
        return ustl::err(Status::InvArgs);
    }

    // 1. The program, as fdio_spawn() takes it: opened with READ_BYTES and
    //    EXECUTE, then its VMO with READ, EXECUTE and PRIVATE_CLONE, which is
    //    what fdio_get_vmo_exec() asks for. The file's connection is not
    //    needed once the VMO is held; closing the handle is enough, as the
    //    server frees the connection on PEER_CLOSED.
    auto const file = open(path, Flags::PermReadBytes | Flags::PermExecute);
    if (file.is_err()) {
        return ustl::err(file.unwrap_err());
    }
    auto const vmo = get_backing_memory(file.unwrap(), fuchsia_io::VmoFlags::Read |
                                                           fuchsia_io::VmoFlags::Execute |
                                                           fuchsia_io::VmoFlags::PrivateClone);
    auto _ = ours::handle_close(file.unwrap());
    if (vmo.is_err()) {
        return ustl::err(vmo.unwrap_err());
    }
    Owned executable(vmo.unwrap());

    // 2. The Launcher, through /svc, as any protocol is reached.
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        status != Status::Ok) {
        return ustl::err(status);
    }
    Owned const launcher(client);
    if (auto const status = connect(LAUNCHER_PATH, server); status != Status::Ok) {
        return ustl::err(status);
    }

    // 3. What the process starts with, one Add* message each, in fdio's
    //    order: the arguments, the handles, the namespace.
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
    auto const args = fuchsia_process::encode_add_args(bytes, argv);
    if (args.is_err()) {
        return ustl::err(args.unwrap_err());
    }
    if (auto const status = send(launcher.get(), args.unwrap()); status != Status::Ok) {
        return ustl::err(status);
    }

    // The job, as JobDefault: FDIO_SPAWN_CLONE_JOB.
    auto const job_default = duplicate(job);
    if (job_default.is_err()) {
        return ustl::err(job_default.unwrap_err());
    }
    Owned job_handle(job_default.unwrap());
    auto const infos = std::array{fuchsia_process::HandleInfo{
        .handle = job_handle.get(),
        .id = ours::handle_info(ours::HandleType::JobDefault, 0),
    }};
    auto const added = fuchsia_process::encode_add_handles(bytes, handles, infos);
    if (added.is_err()) {
        return ustl::err(added.unwrap_err());
    }
    // The write consumes it, whatever it answers.
    auto _ = job_handle.release();
    if (auto const status = send(launcher.get(), added.unwrap()); status != Status::Ok) {
        return ustl::err(status);
    }

    // The namespace, cloned: FDIO_SPAWN_CLONE_NAMESPACE.
    std::array<ExportedEntry, MAX_BINDINGS> entries{};
    auto const exported = export_namespace(entries);
    if (exported.is_err()) {
        return ustl::err(exported.unwrap_err());
    }
    auto const count = exported.unwrap();
    if (count != 0) {
        std::array<fuchsia_process::NameInfo, MAX_BINDINGS> names{};
        for (auto index = usize{0}; index < count; ++index) {
            names[index] = {.path = entries[index].path, .directory = entries[index].directory};
        }
        auto const named =
            fuchsia_process::encode_add_names(bytes, handles, std::span{names}.first(count));
        if (named.is_err()) {
            close_entries(std::span{entries}.first(count));
            return ustl::err(named.unwrap_err());
        }
        if (auto const status = send(launcher.get(), named.unwrap()); status != Status::Ok) {
            return ustl::err(status);
        }
    }

    // 4. Launch, named as fdio names the process: argv[0], cut short of
    //    MAX_NAME_LEN as fdio_spawn() cuts it to ZX_MAX_NAME_LEN - 1.
    auto const launch_job = duplicate(job);
    if (launch_job.is_err()) {
        return ustl::err(launch_job.unwrap_err());
    }
    Owned launch_job_handle(launch_job.unwrap());
    auto const name = argv[0].substr(0, std::min(argv[0].size(), ours::MAX_NAME_LEN - 1));
    auto const txid = next_txid();
    auto const launch = fuchsia_process::encode_launch(
        bytes, handles,
        fuchsia_process::LaunchRequest{
            .txid = txid,
            .info = {.executable = executable.get(), .job = launch_job_handle.get(), .name = name},
        });
    if (launch.is_err()) {
        return ustl::err(launch.unwrap_err());
    }
    // The write in call() consumes both, whatever it answers.
    auto _ = executable.release();
    auto _ = launch_job_handle.release();
    Incoming reply;
    if (auto const status = call(launcher.get(), launch.unwrap(), reply); status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const closed = epitaph_of(reply.message()); closed.is_some()) {
        return ustl::err(*closed);
    }
    auto const decoded = fuchsia_process::decode_launch_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return ustl::err(Status::InvArgs);
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    reply.take_handle(decoded->process);
    return ustl::ok(decoded->process);
}

auto read(Uandle const file, std::span<u8> const buffer) -> ustl::Result<usize, Status> {
    auto const count = std::min(buffer.size(), fuchsia_io::MAX_TRANSFER_SIZE);
    auto const txid = next_txid();
    SmallRequest bytes{};
    auto const request =
        fuchsia_io::encode_read(bytes, fuchsia_io::ReadRequest{.txid = txid, .count = count});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    Incoming reply;
    if (auto const status = call(file, request.unwrap(), reply); status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const closed = epitaph_of(reply.message()); closed.is_some()) {
        return ustl::err(*closed);
    }
    auto const decoded = fuchsia_io::decode_read_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return ustl::err(Status::InvArgs);
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    return copy_payload(decoded->data, count, buffer);
}

auto read_dirents(Uandle const directory, std::span<u8> const buffer)
    -> ustl::Result<usize, Status> {
    auto const max_bytes = std::min(buffer.size(), fuchsia_io::MAX_BUF);
    auto const txid = next_txid();
    SmallRequest bytes{};
    auto const request = fuchsia_io::encode_read_dirents(
        bytes, fuchsia_io::ReadDirentsRequest{.txid = txid, .max_bytes = max_bytes});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    Incoming reply;
    if (auto const status = call(directory, request.unwrap(), reply); status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const closed = epitaph_of(reply.message()); closed.is_some()) {
        return ustl::err(*closed);
    }
    auto const decoded = fuchsia_io::decode_read_dirents_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return ustl::err(Status::InvArgs);
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    return copy_payload(decoded->dirents, max_bytes, buffer);
}

auto get_backing_memory(Uandle const file, fuchsia_io::VmoFlags const flags)
    -> ustl::Result<Uandle, Status> {
    auto const txid = next_txid();
    SmallRequest bytes{};
    auto const request = fuchsia_io::encode_get_backing_memory(
        bytes, fuchsia_io::GetBackingMemoryRequest{.txid = txid, .flags = flags});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    Incoming reply;
    if (auto const status = call(file, request.unwrap(), reply); status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const closed = epitaph_of(reply.message()); closed.is_some()) {
        return ustl::err(*closed);
    }
    auto const decoded = fuchsia_io::decode_get_backing_memory_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return ustl::err(Status::InvArgs);
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    reply.take_handle(decoded->vmo);
    return ustl::ok(decoded->vmo);
}

auto close(Uandle const node) -> Status {
    auto const answer = [node]() -> Status {
        auto const txid = next_txid();
        SmallRequest bytes{};
        auto const request =
            fuchsia_io::encode_close(bytes, fuchsia_io::CloseRequest{.txid = txid});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        Incoming reply;
        if (auto const status = call(node, request.unwrap(), reply); status != Status::Ok) {
            return status;
        }
        if (auto const closed = epitaph_of(reply.message()); closed.is_some()) {
            return *closed;
        }
        auto const decoded = fuchsia_io::decode_close_reply(reply.message(), txid);
        if (decoded.is_err()) {
            return Status::InvArgs;
        }
        return decoded->status;
    }();
    // The connection is over whatever the server said, and the handle with
    // it; a failure to close a handle the caller gave up has no one to tell.
    auto _ = ours::handle_close(node);
    return answer;
}
} // namespace fsio
