/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "boot_server.hpp"

#include <ours/rights.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include "programs.hpp"
#include <bootfs/format.hpp>
#include <bootfs/view.hpp>
#include <ort/debug.hpp>

namespace init {
namespace {
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;

    /// What a connection's slot waits for: a message, or the client gone.
    constexpr auto CONNECTION_SIGNALS = Signals::Readable | Signals::PeerClosed;

    auto report(std::string_view const what, std::string_view const name, Status const status)
        -> void {
        auto _ = ort::debug_line({what, name, ": ", ours::to_string(status)});
    }

    /// Closes every handle of |handles|: the ones a dropped message carried,
    /// which nothing took.
    auto close_all(std::span<Uandle const> const handles) -> void {
        for (auto const handle: handles) {
            // Each was this process's; nothing can be done about a close
            // that fails.
            auto _ = ours::handle_close(handle);
        }
    }

    /// Whether the BootFS name |name| lies under the top-level directory
    /// |directory|: "bin/shell" under "bin", and neither "bin" itself nor
    /// "binary/x".
    auto lies_under(std::string_view const name, std::string_view const directory) -> bool {
        return name.size() > directory.size() && name.starts_with(directory) &&
               name[directory.size()] == '/';
    }
} // namespace

BootServer::BootServer(Storage &storage, loop::Loop &loop)
    : m_storage(&storage)
    , m_loop(&loop)
    , m_tree(storage.nodes)
    , m_server(m_tree, storage.connections,
               vfs::Syscalls{.handle_duplicate = ours::handle_duplicate}) {}

auto BootServer::ingest(std::span<u8 const> const mapping, Uandle const bootfs, Uandle const vmex)
    -> ustl::Result<Ingested, Status> {
    auto const view = bootfs::View::from({mapping.data(), mapping.size()});
    if (view.is_err()) {
        return ustl::err(view.unwrap_err());
    }
    auto const root = m_tree.add_root();
    if (root.is_err()) {
        return ustl::err(root.unwrap_err());
    }
    m_root = root.unwrap();

    auto ingested = Ingested{.files = 0, .executable = 0};
    for (auto index = u32{0}; index < view->file_count(); ++index) {
        auto const entry = view->file(index).unwrap();
        auto const name = view->file_name(entry);

        // The directories on the way, made as they are first met. A name
        // the tree refuses -- empty, ".", "..", too long -- leaves the entry
        // out: nothing could open it anyway.
        auto parent = m_root;
        auto rest = name;
        auto status = Status::Ok;
        for (auto slash = rest.find('/'); slash != std::string_view::npos; slash = rest.find('/')) {
            auto const segment = rest.substr(0, slash);
            rest.remove_prefix(slash + 1);
            if (auto const found = m_tree.lookup(parent, segment); found.is_ok()) {
                parent = found.unwrap();
                continue;
            }
            auto const made = m_tree.add_directory(parent, segment);
            if (made.is_err()) {
                status = made.unwrap_err();
                break;
            }
            parent = made.unwrap();
        }
        if (status == Status::NoResource) {
            return ustl::err(status);
        }
        if (status != Status::Ok) {
            report("init: /boot leaves out ", name, status);
            continue;
        }

        // The file's own VMO: a slice of the BootFS over its bytes, which
        // View has checked start on a page and share no page with another
        // file. A slice of a read-only VMO is read-only too.
        auto slice = Uandle::Invalid;
        if (auto const sliced = ours::vmo_create_child(bootfs, ours::VmoChildOptions::Slice,
                                                       entry.data_offset, entry.data_size, &slice);
            sliced != Status::Ok) {
            report("init: /boot cannot slice ", name, sliced);
            continue;
        }
        // Under bin/, executable: the decision BootfsSvc makes by path
        // (bootfs.rs, the executable directories), with bin/ alone until
        // there are shared libraries (S3). The call consumes the slice and
        // drops Write, which a slice of the BootFS never had.
        auto const executable = lies_under(name, user_programs::DIRECTORY);
        auto vmo = slice;
        if (executable) {
            if (auto const replaced = ours::vmo_replace_as_executable(slice, vmex, &vmo);
                replaced != Status::Ok) {
                report("init: /boot cannot make executable ", name, replaced);
                continue;
            }
        }
        auto const added = m_tree.add_file(
            parent, rest,
            vfs::VmoFile{
                .vmo = vmo,
                .bytes = {view->file_data(entry).data(), view->file_data(entry).size()},
                .executable = executable,
            });
        if (added.is_err()) {
            auto _ = ours::handle_close(vmo);
            if (added.unwrap_err() == Status::NoResource) {
                return ustl::err(Status::NoResource);
            }
            report("init: /boot leaves out ", name, added.unwrap_err());
            continue;
        }
        ingested.files += 1;
        if (executable) {
            ingested.executable += 1;
        }
    }
    return ustl::ok(ingested);
}

auto BootServer::file(std::string_view const path) const -> ustl::Result<vfs::VmoFile, Status> {
    auto const found = m_tree.lookup(m_root, path);
    if (found.is_err()) {
        return ustl::err(found.unwrap_err());
    }
    auto const file = m_tree.file(found.unwrap());
    if (file.is_none()) {
        return ustl::err(Status::NotFile);
    }
    return ustl::ok(*file);
}

auto BootServer::serve(Uandle const channel, std::string_view const subdir,
                       fuchsia_io::Operations const rights) -> Status {
    auto node = m_root;
    if (!subdir.empty()) {
        auto const found = m_tree.lookup(m_root, subdir);
        if (found.is_err()) {
            return found.unwrap_err();
        }
        node = found.unwrap();
    }
    auto const bound = m_server.bind(channel, node, rights);
    if (bound.is_err()) {
        return bound.unwrap_err();
    }
    return watch(channel, bound.unwrap());
}

auto BootServer::watch(Uandle const channel, vfs::ConnectionId const connection) -> Status {
    auto &binding = m_storage->bindings[connection.index];
    binding = Binding{.server = this, .connection = connection};
    auto const added = m_loop->add(channel, CONNECTION_SIGNALS, &BootServer::on_signals, &binding);
    if (added.is_err()) {
        // The Loop did not take the channel, so the connection must not
        // outlive this answer either.
        m_server.release(connection);
        return added.unwrap_err();
    }
    return Status::Ok;
}

auto BootServer::write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}

auto BootServer::on_signals(void *const context, Signals const observed) -> loop::Action {
    auto const *const binding = static_cast<Binding const *>(context);
    return binding->server->handle(binding->connection, observed);
}

auto BootServer::handle(vfs::ConnectionId const connection, Signals const observed)
    -> loop::Action {
    auto const channel = m_server.channel(connection);
    if (channel.is_none()) {
        // Nothing released a connection without closing its slot in the same
        // callback, so this is a slot outliving its connection: close it.
        return loop::Action::Close;
    }
    // The client went away with nothing left to read (I3: the slot's own
    // callback closes its handle, by answering Close).
    if (!(observed & Signals::Readable)) {
        m_server.release(connection);
        return loop::Action::Close;
    }

    // One message per callback, then re-arm (I4): a client with more queued
    // waits behind everyone else.
    auto &storage = *m_storage;
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const read = ours::channel_read(
            *channel, ours::ChannelReadOptions::None, storage.request_bytes.data(),
            storage.request_handles.data(), static_cast<u32>(storage.request_bytes.size()),
            static_cast<u32>(storage.request_handles.size()), &num_bytes, &num_handles);
        read != Status::Ok) {
        // The buffers hold the largest message a channel carries, so this is
        // the channel failing, not the message: nothing more can be served
        // on it.
        m_server.release(connection);
        return loop::Action::Close;
    }
    auto const message = wire::Message{
        .bytes = std::span{storage.request_bytes}.first(num_bytes),
        .handles = std::span{storage.request_handles}.first(num_handles),
    };

    auto const outcome = m_server.dispatch(connection, message, storage.reply);
    switch (outcome.action) {
        case vfs::Action::Reply: {
            // I1: a write to a full channel fails that write alone. A reply
            // that cannot be written leaves its client waiting for good, so
            // the connection goes with it.
            if (auto const written = write(*channel, outcome.message); written != Status::Ok) {
                report("init: /boot cannot answer a client", "", written);
                m_server.release(connection);
                return loop::Action::Close;
            }
            return loop::Action::Rearm;
        }
        case vfs::Action::ReplyAndClose: {
            // Node.Close: answered, and the connection is already released.
            // The client may have gone already; it no longer matters.
            auto _ = write(*channel, outcome.message);
            return loop::Action::Close;
        }
        case vfs::Action::Drop: {
            // Malformed, or a method this connection does not serve: the
            // connection is gone, and with it every handle the message
            // brought, which nothing took.
            close_all(message.handles);
            return loop::Action::Close;
        }
        case vfs::Action::Bind: {
            if (auto const watched = watch(outcome.handle, outcome.connection);
                watched != Status::Ok) {
                // The Open is answered the way any other refused Open is.
                auto const epitaph = wire::encode_epitaph(storage.reply.bytes, watched);
                if (epitaph.is_ok()) {
                    auto _ = write(outcome.handle, epitaph.unwrap());
                }
                auto _ = ours::handle_close(outcome.handle);
            }
            return loop::Action::Rearm;
        }
        case vfs::Action::Connect: {
            // /boot holds no services, so nothing reaches here until /svc
            // has nodes of its own (step 14); the request's channel goes.
            auto _ = ours::handle_close(outcome.handle);
            return loop::Action::Rearm;
        }
        case vfs::Action::Refuse: {
            // The epitaph says why the Open or Clone failed. The client may
            // be gone already; the handle closes either way.
            auto _ = write(outcome.handle, outcome.message);
            auto _ = ours::handle_close(outcome.handle);
            return loop::Action::Rearm;
        }
    }
    return loop::Action::Rearm;
}
} // namespace init
