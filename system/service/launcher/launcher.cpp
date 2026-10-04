/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// launcher -- the one process that starts programs.
///
/// init starts it, with the user-space launcher library, and hands it what
/// starting a program takes: the BootFS, the vDSO's VMO, and VMEX, the right
/// to make a VMO executable. From then on every program is started here, at
/// a client's request (<launch/protocol.hpp>), and no other process needs
/// VMEX (.agents/tasks/launcher-service/DESIGN.md). Fuchsia's counterpart is
/// component_manager serving fuchsia.process.Launcher; there, clients bring
/// VMOs already executable, and the launcher holds no VMEX.
///
/// Startup handles:
///   - User0: the first connection, init's.
///   - VmoBootfs, SystemResource VMEX, VmoVdso, VmaRoot: what
///     launch::make_bootfs_launcher() needs.
///
/// One thread serves every connection, one request at a time, on one port:
/// each connection is watched for Readable and PeerClosed with a one-shot
/// object_wait_async, re-armed after every message, and closed when its peer
/// goes. The loop is this program's own until an event-loop library exists
/// (U10).

#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include <elfload/diagnostics.hpp>
#include <launch/bootfs.hpp>
#include <launch/launch.hpp>
#include <launch/protocol.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/process_args.hpp>
#include <ort/startup.hpp>

namespace {
using ours::Status;
using ours::Uandle;
namespace protocol = launch::protocol;

/// How many connections the launcher serves at once: init's, the shell's,
/// and room for the services init will start. A Connect past it is refused
/// by closing the channel it carried.
constexpr usize MAX_CONNECTIONS = 16;

auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
}

[[noreturn]]
auto halt() -> void {
    // `pause` keeps the loop a side effect the optimiser must keep; see init.
    for (;;) {
        __asm__ volatile("pause");
    }
}

auto print_launch_error(void *, elfload::Report const &report) -> void {
    if (report.severity != elfload::Severity::Error) {
        return;
    }
    std::string_view separator;
    std::string_view name;
    if (!report.name.empty()) {
        separator = ": ";
        name = {report.name.data(), report.name.size()};
    }
    auto _ = ort::debug_line(
        {"launcher: launch failed: ", {report.what.data(), report.what.size()}, separator, name});
}

auto close_all(std::span<Uandle const> const handles) -> void {
    for (auto const handle: handles) {
        auto _ = ours::handle_close(handle);
    }
}

/// The connections, by slot; a slot's index is the key its waits carry.
class Connections {
  public:
    explicit Connections(Uandle const port) : m_port(port) {}

    /// Starts serving |channel|. False, with |channel| closed, when every
    /// slot is taken or the wait cannot be armed.
    auto add(Uandle const channel) -> bool {
        for (usize slot = 0; slot < MAX_CONNECTIONS; ++slot) {
            if (m_slots[slot] != Uandle::Invalid) {
                continue;
            }
            m_slots[slot] = channel;
            if (arm(slot)) {
                return true;
            }
            m_slots[slot] = Uandle::Invalid;
            break;
        }
        auto _ = ours::handle_close(channel);
        return false;
    }

    /// Watches the connection in |slot| for its next message or its end.
    auto arm(usize const slot) -> bool {
        auto const status = ours::object_wait_async(
            m_slots[slot], m_port, slot, ours::Signals::Readable | ours::Signals::PeerClosed,
            ours::WaitAsyncOptions::None);
        if (status != Status::Ok) {
            report("launcher: watching a connection = ", status);
            return false;
        }
        return true;
    }

    auto drop(usize const slot) -> void {
        auto _ = ours::handle_close(m_slots[slot]);
        m_slots[slot] = Uandle::Invalid;
    }

    [[nodiscard]]
    auto at(usize const slot) const -> Uandle {
        return slot < MAX_CONNECTIONS ? m_slots[slot] : Uandle::Invalid;
    }

  private:
    Uandle m_port;
    std::array<Uandle, MAX_CONNECTIONS> m_slots{};
};

/// The strings of |run|, into |out|; false if there are more than it holds.
auto collect(ort::ProcessArgs::Strings const run,
             std::array<std::string_view, protocol::MAX_STRINGS> &out, usize *const count) -> bool {
    *count = 0;
    for (auto const string: run) {
        if (*count == out.size()) {
            return false;
        }
        out[(*count)++] = string;
    }
    return true;
}

/// Serves one Launch: |handles| are the job and then the program's startup
/// handles, all consumed. Answers the status to reply with, and the process
/// handle in |process| on Ok.
///
/// The child's processargs is encoded before the program is started, so that
/// nothing can fail between the start and the write but the write itself. A
/// program whose processargs never comes does not hang -- its runtime sees
/// the bootstrap channel's PeerClosed and exits with EXIT_START_FAILED -- but
/// it would have been started only to die, and the client told it started.
auto serve_launch(launch::BootfsLauncher const &launcher, std::span<u8 const> const message,
                  std::span<Uandle const> const handles, Uandle *const process) -> Status {
    protocol::Launch request;
    if (auto const status =
            protocol::decode_launch(message, static_cast<u32>(handles.size()), &request);
        status != Status::Ok) {
        close_all(handles);
        return status;
    }
    auto const job = handles[0];
    auto const startup = handles.subspan(1);

    // The child's processargs: what the client wrote, and VmaRoot after its
    // handles.
    std::array<u32, ours::CHANNEL_MAX_MSG_HANDLES> infos{};
    auto const num_infos = request.parsed.num_handles();
    for (u32 index = 0; index < num_infos; ++index) {
        infos[index] = request.parsed.handle_info(index);
    }
    infos[num_infos] = ours::handle_info(ours::HandleType::VmaRoot, 0);
    std::array<std::string_view, protocol::MAX_STRINGS> args{};
    std::array<std::string_view, protocol::MAX_STRINGS> environ{};
    std::array<std::string_view, protocol::MAX_STRINGS> names{};
    usize num_args = 0;
    usize num_environ = 0;
    usize num_names = 0;
    if (!collect(request.parsed.args(), args, &num_args) ||
        !collect(request.parsed.environ(), environ, &num_environ) ||
        !collect(request.parsed.names(), names, &num_names)) {
        close_all(handles);
        return Status::OutOfRange;
    }
    u8 child_message[ours::CHANNEL_MAX_MSG_BYTES];
    u32 length = 0;
    if (auto const status = ours::encode_process_args(
            child_message, {infos.data(), num_infos + 1}, {args.data(), num_args},
            {environ.data(), num_environ}, {names.data(), num_names}, &length);
        status != Status::Ok) {
        close_all(handles);
        return status;
    }

    Uandle here = Uandle::Invalid;
    Uandle there = Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
        status != Status::Ok) {
        close_all(handles);
        return status;
    }
    auto const started = launch::launch_from_bootfs(
        launcher, job, request.name, there, {.report = print_launch_error, .context = nullptr});
    auto _ = ours::handle_close(job);
    if (started.is_err()) {
        close_all(startup);
        auto _ = ours::handle_close(here);
        return started.unwrap_err();
    }

    std::array<Uandle, ours::CHANNEL_MAX_MSG_HANDLES> carried{};
    for (usize index = 0; index < startup.size(); ++index) {
        carried[index] = startup[index];
    }
    carried[startup.size()] = started.unwrap().root_vma;
    auto const written =
        ours::channel_write(here, ours::ChannelWriteOptions::None, child_message, length,
                            carried.data(), static_cast<u32>(startup.size() + 1));
    auto _ = ours::handle_close(here);
    if (written != Status::Ok) {
        // With |here| closed and no message on it, the program's runtime
        // sees PeerClosed and exits on its own (EXIT_START_FAILED). The
        // handle the client would have had goes with the failure.
        auto _ = ours::handle_close(started.unwrap().process);
        return written;
    }
    *process = started.unwrap().process;
    auto _ = ort::debug_line({"launcher: started ", request.name});
    return Status::Ok;
}

/// Reads one message from the connection in |slot| and serves it.
auto serve_message(launch::BootfsLauncher const &launcher, Connections &connections,
                   usize const slot) -> void {
    auto const channel = connections.at(slot);
    u8 message[ours::CHANNEL_MAX_MSG_BYTES];
    Uandle handles[ours::CHANNEL_MAX_MSG_HANDLES];
    u32 actual_bytes = 0;
    u32 actual_handles = 0;
    if (auto const status =
            ours::channel_read(channel, ours::ChannelReadOptions::None, message, handles,
                               sizeof(message), std::size(handles), &actual_bytes, &actual_handles);
        status != Status::Ok) {
        report("launcher: reading a request = ", status);
        return;
    }
    auto const bytes = std::span<u8 const>{message, actual_bytes};
    auto const carried = std::span<Uandle const>{handles, actual_handles};

    auto const ordinal = protocol::ordinal_of(bytes);
    if (ordinal.is_some() && *ordinal == protocol::Ordinal::Connect) {
        if (actual_handles == 1 && actual_bytes == sizeof(protocol::Header)) {
            if (connections.add(handles[0])) {
                auto _ = ort::debug_line("launcher: a new connection");
            }
        } else {
            close_all(carried);
        }
        return;
    }

    // Anything else is answered as a Launch, a malformed one with InvArgs:
    // the client is waiting for a reply.
    auto process = Uandle::Invalid;
    auto const status = serve_launch(launcher, bytes, carried, &process);
    auto const reply = protocol::Reply{.ordinal = protocol::Ordinal::Launch, .status = status};
    auto const with_process = status == Status::Ok;
    if (auto const written =
            ours::channel_write(channel, ours::ChannelWriteOptions::None, &reply, sizeof(reply),
                                with_process ? &process : nullptr, with_process ? 1 : 0);
        written != Status::Ok) {
        report("launcher: replying = ", written);
    }
}
} // namespace

auto main(int, char **) -> int {
    auto const first = ort::take_startup_handle(ours::HandleType::User0, 0);
    auto const bootfs = ort::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    auto const vdso = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    if (first == Uandle::Invalid || bootfs == Uandle::Invalid || vmex == Uandle::Invalid ||
        vdso == Uandle::Invalid || vma == Uandle::Invalid) {
        // Exits rather than halting: a launcher started without its handles
        // was started by mistake -- by a typed `launcher`, say -- and its
        // starter is waiting for it to end.
        auto _ = ort::debug_line(
            "launcher: started without a connection, the BootFS, vmex, the vDSO or VmaRoot");
        return 1;
    }

    auto made = launch::make_bootfs_launcher(bootfs, vmex, vdso, vma,
                                             {.report = print_launch_error, .context = nullptr});
    if (made.is_err()) {
        report("launcher: setting up = ", made.unwrap_err());
        halt();
    }
    auto const launcher = made.unwrap();

    Uandle port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("launcher: port_create = ", status);
        halt();
    }
    Connections connections(port);
    if (!connections.add(first)) {
        halt();
    }
    auto _ = ort::debug_line("launcher: serving");

    for (;;) {
        ours::PortPacket packet{};
        if (auto const status = ours::port_wait(port, ours::INSTANT_INFINITE, &packet);
            status != Status::Ok) {
            report("launcher: port_wait = ", status);
            halt();
        }
        auto const slot = static_cast<usize>(packet.key);
        if (connections.at(slot) == Uandle::Invalid) {
            continue;
        }
        if (!!(packet.signal.observed & ours::Signals::Readable)) {
            serve_message(launcher, connections, slot);
            // Re-armed even if more messages wait: the wait is
            // level-triggered, so it queues again at once.
            if (!connections.arm(slot)) {
                connections.drop(slot);
            }
            continue;
        }
        // PeerClosed and nothing left to read: the client is gone.
        connections.drop(slot);
    }
}
