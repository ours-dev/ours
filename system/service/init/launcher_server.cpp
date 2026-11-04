/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "launcher_server.hpp"

#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <span>

#include <launch/launch.hpp>
#include <ort/debug.hpp>
#include <prot/process.hpp>

namespace init {
namespace {
    using ours::HandleType;
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;

    /// What a connection's slot waits for: a message, or the client gone.
    constexpr auto CONNECTION_SIGNALS = Signals::Readable | Signals::PeerClosed;

    // The client's handles and the launcher's own fill one processargs
    // message, in the order LAUNCHER_HANDLE_INFO lists the launcher's, which
    // start() below appends them in.
    static_assert(launch::ProcessLauncherState::MAX_HANDLES + launch::LAUNCHER_HANDLE_INFO.size() ==
                  ours::CHANNEL_MAX_MSG_HANDLES);
    static_assert(launch::LAUNCHER_HANDLE_INFO[0] == ours::handle_info(HandleType::VmaRoot, 0));
    static_assert(launch::LAUNCHER_HANDLE_INFO[1] == ours::handle_info(HandleType::VmoVdso, 0));
    static_assert(launch::LAUNCHER_HANDLE_INFO[2] == ours::handle_info(HandleType::ProcSelf, 0));

    /// Closes every handle of |handles|.
    auto close_all(std::span<Uandle const> const handles) -> void {
        for (auto const handle: handles) {
            // Each was this process's; nothing can be done about a close
            // that fails.
            auto _ = ours::handle_close(handle);
        }
    }
} // namespace

auto LauncherConnection::on_signals(Signals const observed) -> loop::Action {
    return m_server->handle(*this, observed);
}

auto LauncherConnection::on_rearm_failed(Status const cause) -> void {
    m_server->rearm_failed(*this, cause);
}

auto LauncherConnection::start(launch::Startup const &startup) -> ustl::Result<Uandle, Status> {
    return m_server->start(startup);
}

auto LauncherConnection::close(Uandle const handle) -> void {
    // A handle the state held and passes on to nobody; nothing to tell if
    // its close fails.
    auto _ = ours::handle_close(handle);
}

LauncherServer::LauncherServer(Storage &storage, loop::Loop &loop, Environment const &environment,
                               elfload::Diagnostics const diagnostics)
    : m_storage(&storage), m_loop(&loop), m_environment(environment), m_diagnostics(diagnostics) {}

auto LauncherServer::bind(Uandle const channel) -> Status {
    for (auto &connection: m_storage->connections) {
        if (connection.m_channel != Uandle::Invalid) {
            continue;
        }
        // A free entry's state is a new connection's: reset() ran when its
        // last connection ended, and a constinit entry never held anything.
        connection.m_server = this;
        connection.m_channel = channel;
        auto const added = m_loop->add(channel, CONNECTION_SIGNALS, connection);
        if (added.is_err()) {
            connection.m_channel = Uandle::Invalid;
            return added.unwrap_err();
        }
        return Status::Ok;
    }
    return Status::NoResource;
}

auto LauncherServer::release(LauncherConnection &connection) -> void {
    connection.m_state.reset();
    connection.m_channel = Uandle::Invalid;
}

auto LauncherServer::rearm_failed(LauncherConnection &connection, Status const cause) -> void {
    // What the client added and the state still holds is closed with the
    // entry, as on every path that ends the connection.
    release(connection);
    auto _ = ort::debug_line(
        {"init: the Launcher cannot wait on a client again: ", ours::to_string(cause)});
}

auto LauncherServer::handle(LauncherConnection &connection, Signals const observed)
    -> loop::Action {
    auto &state = connection.m_state;
    // The client went away with nothing left to read: whatever it added is
    // closed with the connection's state, which no one can launch from
    // any more.
    if (!(observed & Signals::Readable)) {
        release(connection);
        return loop::Action::Close;
    }

    // One message per on_signals(), then re-arm (I4).
    auto &storage = *m_storage;
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const read = ours::channel_read(
            connection.m_channel, ours::ChannelReadOptions::None, storage.request_bytes.data(),
            storage.request_handles.data(), static_cast<u32>(storage.request_bytes.size()),
            static_cast<u32>(storage.request_handles.size()), &num_bytes, &num_handles);
        read != Status::Ok) {
        // The buffers hold the largest message a channel carries, so this is
        // the channel failing, not the message.
        release(connection);
        return loop::Action::Close;
    }
    auto const message = wire::Message{
        .bytes = std::span{storage.request_bytes}.first(num_bytes),
        .handles = std::span{storage.request_handles}.first(num_handles),
    };

    // The state owns every handle of the message from here, and accounts for
    // each: added, handed to start(), or closed.
    auto const received = state.receive(message, storage.process_args);
    switch (received.outcome) {
        case launch::ProcessLauncherState::Outcome::Continue: {
            return loop::Action::Rearm;
        }
        case launch::ProcessLauncherState::Outcome::Close: {
            // Malformed, or no Launcher request: the connection goes without
            // an epitaph, since a peer that sends such a message is not
            // speaking the protocol. The state has closed what it held.
            release(connection);
            return loop::Action::Close;
        }
        case launch::ProcessLauncherState::Outcome::Reply: {
            break;
        }
    }

    auto const encoded = prot::process::encode_launch_reply(
        storage.reply_bytes, storage.reply_handles, received.txid, received.reply);
    if (encoded.is_err()) {
        // Cannot happen: the txid passed the decoder, and the buffers hold
        // either form. Were it to, the client would wait for good, so its
        // connection goes, and the process with it.
        if (received.reply.process != Uandle::Invalid) {
            auto _ = ours::handle_close(received.reply.process);
        }
        release(connection);
        return loop::Action::Close;
    }
    auto const reply = encoded.unwrap();
    // The process handle goes with the reply, whatever the write answers.
    if (auto const written =
            ours::channel_write(connection.m_channel, ours::ChannelWriteOptions::None,
                                reply.bytes.data(), static_cast<u32>(reply.bytes.size()),
                                reply.handles.data(), static_cast<u32>(reply.handles.size()));
        written != Status::Ok) {
        // I1: a reply that cannot be written leaves its client waiting for
        // good, so the connection goes with it.
        auto _ = ort::debug_line(
            {"init: the Launcher cannot answer a client: ", ours::to_string(written)});
        release(connection);
        return loop::Action::Close;
    }
    return loop::Action::Rearm;
}

auto LauncherServer::start(launch::Startup const &startup) -> ustl::Result<Uandle, Status> {
    // Every handle of |startup.handles| leaves on one of the ways out below:
    // written to the process, or closed.
    std::array<Uandle, ours::CHANNEL_MAX_MSG_HANDLES> handles{};
    auto count = startup.handles.size();
    std::ranges::copy(startup.handles, handles.begin());
    auto const fail = [&handles, count](Status const status) -> ustl::Result<Uandle, Status> {
        close_all(std::span{handles}.first(count));
        return ustl::err(status);
    };

    // The process's vDSO handle first, so that a failure here starts
    // nothing.
    auto vdso = Uandle::Invalid;
    if (auto const status =
            ours::handle_duplicate(m_environment.vdso, ours::Rights::SameRights, &vdso);
        status != Status::Ok) {
        return fail(status);
    }
    auto here = Uandle::Invalid;
    auto there = Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
        status != Status::Ok) {
        auto _ = ours::handle_close(vdso);
        return fail(status);
    }
    // The client's VMO, mapped with Execute into the root while it is read,
    // which refuses a VMO without the right before a byte of it is parsed
    // (D14, I6); and the client's job, which must grant ManageProcess.
    // Neither is consumed: the state closes both after this returns.
    auto const started = launch::start({
        .job = startup.job,
        .name = startup.name,
        .file = startup.executable,
        .vma = m_environment.vma,
        .vdso = m_environment.vdso,
        .vdso_file = m_environment.vdso_file,
        .clock_page = m_environment.clock_page,
        .bootstrap = there,
        .diagnostics = m_diagnostics,
    });
    if (started.is_err()) {
        auto _ = ours::handle_close(here);
        auto _ = ours::handle_close(vdso);
        return fail(started.unwrap_err());
    }

    // The process's handle to itself, a duplicate with every right of the
    // one start() returned, which goes to the client. A failure here leaves
    // the process waiting for its processargs; closing |here| ends it.
    auto self = Uandle::Invalid;
    if (auto const status =
            ours::handle_duplicate(started->process, ours::Rights::SameRights, &self);
        status != Status::Ok) {
        auto _ = ours::handle_close(here);
        auto _ = ours::handle_close(vdso);
        auto _ = ours::handle_close(started->root_vma);
        auto _ = ours::handle_close(started->process);
        return fail(status);
    }

    // The processargs, encoded by the state with LAUNCHER_HANDLE_INFO's
    // three infos last: VmaRoot, VmoVdso, then ProcSelf.
    handles[count] = started->root_vma;
    handles[count + 1] = vdso;
    handles[count + 2] = self;
    auto const written =
        ours::channel_write(here, ours::ChannelWriteOptions::None, startup.process_args.data(),
                            static_cast<u32>(startup.process_args.size()), handles.data(),
                            static_cast<u32>(count + launch::LAUNCHER_HANDLE_INFO.size()));
    // The write took the handles, whatever it answered.
    auto _ = ours::handle_close(here);
    if (written != Status::Ok) {
        // The runtime finds the bootstrap channel closed with nothing on it
        // and exits; nobody gets its process.
        auto _ = ours::handle_close(started->process);
        return ustl::err(written);
    }
    return ustl::ok(started->process);
}
} // namespace init
