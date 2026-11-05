/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/process_launcher.hpp>

#include <algorithm>

#include <ours/channel.hpp>

namespace launch {
namespace {
    using ours::HandleType;
    using ours::Status;
    using ours::Uandle;
    using prot::process::AddHandlesRequest;
    using prot::process::LaunchInfo;
    using prot::process::PackedStrings;

    /// The handle types a client may not give, because the launcher adds
    /// them itself or they would describe the launcher's own work. VmaRoot
    /// and the region of the loaded image are the new process's, which only
    /// the launcher has; a self handle or a stack VMO from a client would
    /// name some other process's.
    constexpr auto RESERVED_TYPES = std::array{
        HandleType::ProcSelf,  HandleType::ThreadSelf, HandleType::VmaRoot,
        HandleType::VmaLoaded, HandleType::VmoStack,   HandleType::VmoExecutable,
    };

    /// Whether AddHandles may give a handle with |id|. NsDir may not: a
    /// namespace entry needs its path, which only AddNames carries, and its
    /// argument must index the names this state keeps. Nor may an id with
    /// its reserved bits set, which no handle_info of <ours/process_args.hpp>
    /// has.
    auto may_add(u32 const id) -> bool {
        if ((id & ours::HANDLE_INFO_RESERVED) != 0) {
            return false;
        }
        auto const type = ours::handle_info_type(id);
        return type != HandleType::NsDir &&
               std::ranges::find(RESERVED_TYPES, type) == RESERVED_TYPES.end();
    }
} // namespace

auto ProcessLauncherState::receive(wire::Message const message, std::span<u8> const scratch)
    -> Received {
    // Which method it is, from the header alone; each decoder then checks
    // the header again against its own method, with the txid rule.
    if (auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        header.is_ok()) {
        auto const ordinal = header->ordinal;
        if (ordinal == prot::process::ADD_ARGS.ordinal) {
            if (auto const request = prot::process::decode_add_args(message); request.is_ok()) {
                add_args(request->args);
                return Received{.outcome = Outcome::Continue};
            }
        } else if (ordinal == prot::process::ADD_HANDLES.ordinal) {
            if (auto const request = prot::process::decode_add_handles(message); request.is_ok()) {
                add_handles(request.unwrap());
                return Received{.outcome = Outcome::Continue};
            }
        } else if (ordinal == prot::process::ADD_NAMES.ordinal) {
            if (auto const request = prot::process::decode_add_names(message); request.is_ok()) {
                add_names(request.unwrap());
                return Received{.outcome = Outcome::Continue};
            }
        } else if (ordinal == prot::process::LAUNCH.ordinal) {
            if (auto const request = prot::process::decode_launch(message); request.is_ok()) {
                return launch(request.unwrap(), scratch);
            }
        }
    }
    // Malformed, a reply, or an ordinal Launcher does not have, which a
    // closed protocol does not answer: the connection goes, and everything
    // it brought with it.
    close_all(message.handles);
    reset();
    return Received{.outcome = Outcome::Close};
}

auto ProcessLauncherState::reset() -> void {
    close_all(std::span{m_handles}.first(m_num_handles));
    m_num_handles = 0;
    m_num_names = 0;
    m_num_args = 0;
    m_strings_size = 0;
    m_marks = Marks::None;
}

auto ProcessLauncherState::add_args(PackedStrings const args) -> void {
    if (args.count > MAX_ARGS - m_num_args ||
        args.bytes.size() > MAX_STRING_BYTES - m_strings_size) {
        m_marks |= Marks::Overflow;
        return;
    }
    store_strings(args, m_args, m_num_args);
    m_num_args += args.count;
}

auto ProcessLauncherState::add_handles(AddHandlesRequest const &request) -> void {
    // All of the message or none of it: every handle is checked before any
    // is added, so a refused message leaves the state as it was.
    auto const count = request.handles.size();
    auto refused = false;
    for (usize index = 0; index < count; ++index) {
        if (!may_add(request.at(index).id)) {
            refused = true;
        }
    }
    if (refused) {
        m_marks |= Marks::Refused;
        close_all(request.handles);
        return;
    }
    if (count > MAX_HANDLES - m_num_handles) {
        m_marks |= Marks::Overflow;
        close_all(request.handles);
        return;
    }
    for (usize index = 0; index < count; ++index) {
        auto const entry = request.at(index);
        m_handles[m_num_handles] = entry.handle;
        m_handle_info[m_num_handles] = entry.id;
        ++m_num_handles;
    }
}

auto ProcessLauncherState::add_names(prot::process::AddNamesRequest const &request) -> void {
    auto const count = request.directories.size();
    if (count > MAX_HANDLES - m_num_handles ||
        request.paths.bytes.size() > MAX_STRING_BYTES - m_strings_size) {
        m_marks |= Marks::Overflow;
        close_all(request.directories);
        return;
    }
    // Every name brings a handle, so there is room for its piece too.
    store_strings(request.paths, m_names, m_num_names);
    for (auto const directory: request.directories) {
        // The argument of an NsDir is the index of its path among the
        // processargs' names, which lie in the order they came, counted
        // from 0 across every AddNames call.
        m_handles[m_num_handles] = directory;
        m_handle_info[m_num_handles] =
            ours::handle_info(HandleType::NsDir, static_cast<u16>(m_num_names));
        ++m_num_handles;
        ++m_num_names;
    }
}

auto ProcessLauncherState::launch(prot::process::LaunchRequest const &request,
                                  std::span<u8> const scratch) -> Received {
    auto started = start(request.info, scratch);
    // start() never consumes the job or the program: the launcher closes
    // them once the process is built.
    close_all(std::array{request.info.executable, request.info.job});
    // A new connection from here on, whatever the answer: everything left
    // was never handed to start().
    reset();
    if (started.is_err()) {
        return Received{
            .outcome = Outcome::Reply,
            .txid = request.txid,
            .reply = {.status = started.unwrap_err()},
        };
    }
    return Received{
        .outcome = Outcome::Reply,
        .txid = request.txid,
        .reply = {.status = Status::Ok, .process = started.unwrap()},
    };
}

auto ProcessLauncherState::start(LaunchInfo const &info, std::span<u8> const scratch)
    -> ustl::Result<Uandle, Status> {
    // A name that holds a NUL is InvArgs, as it could not become the C
    // string process_create takes.
    if (m_marks != Marks::None || info.name.find('\0') != std::string_view::npos) {
        return ustl::err(Status::InvArgs);
    }

    // encode_process_args() takes the strings as views and the handle_info
    // as one array, so they are laid out here, on the stack, from what the
    // state keeps more compactly.
    std::array<std::string_view, MAX_ARGS> args;
    for (usize index = 0; index < m_num_args; ++index) {
        args[index] = string_at(m_args[index]);
    }
    std::array<std::string_view, MAX_HANDLES> names;
    for (usize index = 0; index < m_num_names; ++index) {
        names[index] = string_at(m_names[index]);
    }
    std::array<u32, ours::CHANNEL_MAX_MSG_HANDLES> handle_info{};
    std::ranges::copy(std::span{m_handle_info}.first(m_num_handles), handle_info.begin());
    std::ranges::copy(LAUNCHER_HANDLE_INFO, handle_info.begin() + m_num_handles);

    u32 length = 0;
    auto const status = ours::encode_process_args(
        scratch, std::span{handle_info}.first(m_num_handles + LAUNCHER_HANDLE_INFO.size()),
        std::span{args}.first(m_num_args), {}, std::span{names}.first(m_num_names), &length);
    if (status == Status::OutOfRange) {
        // More than one message carries: the strings fit this state, but not
        // beside this many handle_info. InvArgs, as for any processargs too
        // large to send.
        return ustl::err(Status::InvArgs);
    }
    if (status != Status::Ok) {
        return ustl::err(status);
    }

    auto const startup = Startup{
        .job = info.job,
        .name = info.name,
        .executable = info.executable,
        .process_args = scratch.first(length),
        .handles = std::span{m_handles}.first(m_num_handles),
    };
    auto started = m_ops->start(startup);
    // start() took them, whatever it answered.
    m_num_handles = 0;
    return started;
}

auto ProcessLauncherState::store_strings(PackedStrings const strings, std::span<Piece> const pieces,
                                         usize count) -> void {
    auto const first = m_strings_size;
    // Through a span rather than memcpy, so that a hardened build checks the
    // caller's promise that they fit.
    std::ranges::transform(strings.bytes,
                           std::span{m_strings}.subspan(first, strings.bytes.size()).begin(),
                           [](u8 const byte) { return static_cast<char>(byte); });
    m_strings_size += strings.bytes.size();
    // The decoder checked that every string ends in a NUL inside the bytes.
    auto begin = first;
    for (auto at = first; at < m_strings_size; ++at) {
        if (m_strings[at] == '\0') {
            pieces[count] = Piece{
                .offset = static_cast<u16>(begin),
                .size = static_cast<u16>(at - begin),
            };
            ++count;
            begin = at + 1;
        }
    }
}

auto ProcessLauncherState::string_at(Piece const piece) const -> std::string_view {
    return {m_strings.data() + piece.offset, piece.size};
}

auto ProcessLauncherState::close_all(std::span<Uandle const> const handles) const -> void {
    for (auto const handle: handles) {
        if (handle != Uandle::Invalid) {
            m_ops->close(handle);
        }
    }
}
} // namespace launch
