/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prot/process.hpp>

#include <algorithm>
#include <limits>
#include <utility>

#include <cstring>

namespace prot::process {
namespace {
    using ours::Status;
    using ours::Uandle;
    using wire::Message;

    using Encoded = ustl::Result<Message, Status>;

    // AddArgs, AddHandles and AddNames: a count, a byte size (a zero u32 for
    // AddHandles, which has no bytes but its entries), then what they count.
    constexpr auto COUNT_OFFSET = usize{16};
    constexpr auto SIZE_OFFSET = usize{20};
    constexpr auto BODY_OFFSET = usize{24};

    // AddHandles: an entry is {u32 handle, u32 id}.
    constexpr auto ENTRY_SIZE = usize{8};
    constexpr auto ENTRY_ID_OFFSET = usize{4};

    // AddNames: one handle field per name, then the paths.
    constexpr auto DIRECTORY_SIZE = sizeof(u32);

    // Launch.
    constexpr auto LAUNCH_EXECUTABLE_OFFSET = usize{16};
    constexpr auto LAUNCH_JOB_OFFSET = usize{20};
    constexpr auto LAUNCH_NAME_SIZE_OFFSET = usize{24};
    constexpr auto LAUNCH_NAME_OFFSET = usize{28};

    // Launch's Ok reply: the process after the status.
    constexpr auto REPLY_PROCESS_OFFSET = usize{20};

    static_assert(wire::STATUS_OFFSET + sizeof(u32) == REPLY_PROCESS_OFFSET);
    static_assert(LAUNCH_NAME_OFFSET + MAX_NAME_LEN <= wire::MAX_MESSAGE_BYTES);

    /// No bound on one string's length but the message's.
    constexpr auto UNBOUNDED = std::numeric_limits<usize>::max();

    /// The bytes of |strings| packed: each one's bytes and its NUL. InvArgs
    /// for a string that holds a NUL, which would end it early on the other
    /// side, or one longer than |longest|, and for a total that no message
    /// could carry.
    template <typename T, typename Project>
    auto packed_size(std::span<T const> const strings, Project const project, usize const longest)
        -> ustl::Result<usize, Status> {
        auto size = usize{0};
        for (auto const &item: strings) {
            std::string_view const string = project(item);
            if (string.size() > longest || string.find('\0') != std::string_view::npos) {
                return ustl::err(Status::InvArgs);
            }
            size += string.size() + 1;
            if (size > wire::MAX_MESSAGE_BYTES) {
                return ustl::err(Status::InvArgs);
            }
        }
        return ustl::ok(size);
    }

    /// Writes |strings| packed at the front of |out|, which packed_size()
    /// has sized.
    template <typename T, typename Project>
    auto put_packed(std::span<u8> const out, std::span<T const> const strings,
                    Project const project) -> void {
        auto at = usize{0};
        for (auto const &item: strings) {
            std::string_view const string = project(item);
            if (!string.empty()) {
                std::memcpy(out.data() + at, string.data(), string.size());
            }
            at += string.size();
            out[at] = 0;
            ++at;
        }
    }

    /// Whether |bytes| is |count| packed strings, none longer than
    /// |longest|: exactly |count| NULs, the last byte one of them.
    auto is_packed(u32 const count, std::span<u8 const> const bytes, usize const longest) -> bool {
        auto strings = usize{0};
        auto length = usize{0};
        for (auto const byte: bytes) {
            if (byte != 0) {
                ++length;
                continue;
            }
            if (length > longest) {
                return false;
            }
            ++strings;
            length = 0;
        }
        // A string still open here ran off the end without its NUL.
        return length == 0 && strings == count;
    }

    /// The parts of AddArgs and AddNames that hold strings: the count and
    /// the size, checked against |message|, whose strings start at
    /// |strings_offset|. Every refusal is InvArgs.
    auto decode_packed(Message const message, usize const strings_offset, usize const longest)
        -> ustl::Result<PackedStrings, Status> {
        auto const count = wire::load<u32>(message.bytes, COUNT_OFFSET);
        auto const size = wire::load<u32>(message.bytes, SIZE_OFFSET);
        if (message.bytes.size() < strings_offset ||
            message.bytes.size() - strings_offset != size) {
            return ustl::err(Status::InvArgs);
        }
        auto const bytes = message.bytes.subspan(strings_offset);
        if (!is_packed(count, bytes, longest)) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(PackedStrings{.count = count, .bytes = bytes});
    }

    auto chars(std::span<u8 const> const bytes) -> std::string_view {
        return {reinterpret_cast<char const *>(bytes.data()), bytes.size()};
    }
} // namespace

auto StringReader::next() -> ustl::Option<std::string_view> {
    auto const end = std::ranges::find(m_rest, u8{0});
    if (end == m_rest.end()) {
        m_rest = {};
        return ustl::NONE;
    }
    auto const length = static_cast<usize>(end - m_rest.begin());
    auto const string = chars(m_rest.first(length));
    m_rest = m_rest.subspan(length + 1);
    return ustl::some(string);
}

auto AddHandlesRequest::at(usize const index) const -> HandleInfo {
    return HandleInfo{
        .handle = handles[index],
        .id = wire::load<u32>(entries, (index * ENTRY_SIZE) + ENTRY_ID_OFFSET),
    };
}

auto encode_add_args(std::span<u8> const bytes, std::span<std::string_view const> const args)
    -> Encoded {
    auto const identity = [](std::string_view const string) { return string; };
    auto size = packed_size(args, identity, UNBOUNDED);
    if (size.is_err()) {
        return ustl::err(size.unwrap_err());
    }
    auto started = wire::start_request(bytes, BODY_OFFSET + size.unwrap(), ADD_ARGS, 0);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, COUNT_OFFSET, static_cast<u32>(args.size()));
    wire::store(message, SIZE_OFFSET, static_cast<u32>(size.unwrap()));
    put_packed(message.subspan(BODY_OFFSET), args, identity);
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_add_args(Message const message) -> ustl::Result<AddArgsRequest, Status> {
    if (auto header = wire::decode_request_header(message, ADD_ARGS); header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < BODY_OFFSET || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto args = decode_packed(message, BODY_OFFSET, UNBOUNDED);
    if (args.is_err()) {
        return ustl::err(args.unwrap_err());
    }
    return ustl::ok(AddArgsRequest{.args = args.unwrap()});
}

auto encode_add_handles(std::span<u8> const bytes, std::span<Uandle> const handles,
                        std::span<HandleInfo const> const infos) -> Encoded {
    if (infos.size() > wire::MAX_MESSAGE_HANDLES) {
        return ustl::err(Status::InvArgs);
    }
    auto started =
        wire::start_request(bytes, BODY_OFFSET + (infos.size() * ENTRY_SIZE), ADD_HANDLES, 0);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, COUNT_OFFSET, static_cast<u32>(infos.size()));
    for (usize index = 0; index < infos.size(); ++index) {
        auto const entry = BODY_OFFSET + (index * ENTRY_SIZE);
        if (auto const status =
                wire::encode_handle(message, entry, handles, index, infos[index].handle);
            status != Status::Ok) {
            return ustl::err(status);
        }
        wire::store(message, entry + ENTRY_ID_OFFSET, infos[index].id);
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(infos.size())});
}

auto decode_add_handles(Message const message) -> ustl::Result<AddHandlesRequest, Status> {
    if (auto header = wire::decode_request_header(message, ADD_HANDLES); header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < BODY_OFFSET ||
        wire::load<u32>(message.bytes, COUNT_OFFSET) != message.handles.size() ||
        message.handles.size() > wire::MAX_MESSAGE_HANDLES ||
        wire::load<u32>(message.bytes, SIZE_OFFSET) != 0 ||
        message.bytes.size() != BODY_OFFSET + (message.handles.size() * ENTRY_SIZE)) {
        return ustl::err(Status::InvArgs);
    }
    for (usize index = 0; index < message.handles.size(); ++index) {
        if (auto handle = wire::decode_handle(message, BODY_OFFSET + (index * ENTRY_SIZE), index);
            handle.is_err()) {
            return ustl::err(handle.unwrap_err());
        }
    }
    return ustl::ok(AddHandlesRequest{
        .handles = message.handles,
        .entries = message.bytes.subspan(BODY_OFFSET),
    });
}

auto encode_add_names(std::span<u8> const bytes, std::span<Uandle> const handles,
                      std::span<NameInfo const> const names) -> Encoded {
    if (names.size() > wire::MAX_MESSAGE_HANDLES) {
        return ustl::err(Status::InvArgs);
    }
    auto const path = [](NameInfo const &name) { return name.path; };
    auto size = packed_size(names, path, MAX_PATH_LENGTH);
    if (size.is_err()) {
        return ustl::err(size.unwrap_err());
    }
    auto const paths_offset = BODY_OFFSET + (names.size() * DIRECTORY_SIZE);
    auto started = wire::start_request(bytes, paths_offset + size.unwrap(), ADD_NAMES, 0);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, COUNT_OFFSET, static_cast<u32>(names.size()));
    wire::store(message, SIZE_OFFSET, static_cast<u32>(size.unwrap()));
    for (usize index = 0; index < names.size(); ++index) {
        if (auto const status = wire::encode_handle(message, BODY_OFFSET + (index * DIRECTORY_SIZE),
                                                    handles, index, names[index].directory);
            status != Status::Ok) {
            return ustl::err(status);
        }
    }
    put_packed(message.subspan(paths_offset), names, path);
    return ustl::ok(Message{.bytes = message, .handles = handles.first(names.size())});
}

auto decode_add_names(Message const message) -> ustl::Result<AddNamesRequest, Status> {
    if (auto header = wire::decode_request_header(message, ADD_NAMES); header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < BODY_OFFSET ||
        wire::load<u32>(message.bytes, COUNT_OFFSET) != message.handles.size() ||
        message.handles.size() > wire::MAX_MESSAGE_HANDLES) {
        return ustl::err(Status::InvArgs);
    }
    auto const paths_offset = BODY_OFFSET + (message.handles.size() * DIRECTORY_SIZE);
    auto paths = decode_packed(message, paths_offset, MAX_PATH_LENGTH);
    if (paths.is_err()) {
        return ustl::err(paths.unwrap_err());
    }
    for (usize index = 0; index < message.handles.size(); ++index) {
        if (auto handle =
                wire::decode_handle(message, BODY_OFFSET + (index * DIRECTORY_SIZE), index);
            handle.is_err()) {
            return ustl::err(handle.unwrap_err());
        }
    }
    return ustl::ok(AddNamesRequest{.paths = paths.unwrap(), .directories = message.handles});
}

auto encode_launch(std::span<u8> const bytes, std::span<Uandle> const handles,
                   LaunchRequest const &request) -> Encoded {
    auto const &info = request.info;
    if (info.name.size() > MAX_NAME_LEN) {
        return ustl::err(Status::InvArgs);
    }
    auto started =
        wire::start_request(bytes, LAUNCH_NAME_OFFSET + info.name.size(), LAUNCH, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status =
            wire::encode_handle(message, LAUNCH_EXECUTABLE_OFFSET, handles, 0, info.executable);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = wire::encode_handle(message, LAUNCH_JOB_OFFSET, handles, 1, info.job);
        status != Status::Ok) {
        return ustl::err(status);
    }
    wire::store(message, LAUNCH_NAME_SIZE_OFFSET, static_cast<u32>(info.name.size()));
    if (!info.name.empty()) {
        std::memcpy(message.data() + LAUNCH_NAME_OFFSET, info.name.data(), info.name.size());
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(2)});
}

auto decode_launch(Message const message) -> ustl::Result<LaunchRequest, Status> {
    auto header = wire::decode_request_header(message, LAUNCH);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < LAUNCH_NAME_OFFSET || message.handles.size() != 2) {
        return ustl::err(Status::InvArgs);
    }
    auto const name_size = wire::load<u32>(message.bytes, LAUNCH_NAME_SIZE_OFFSET);
    if (name_size > MAX_NAME_LEN || message.bytes.size() != LAUNCH_NAME_OFFSET + name_size) {
        return ustl::err(Status::InvArgs);
    }
    auto executable = wire::decode_handle(message, LAUNCH_EXECUTABLE_OFFSET, 0);
    if (executable.is_err()) {
        return ustl::err(executable.unwrap_err());
    }
    auto job = wire::decode_handle(message, LAUNCH_JOB_OFFSET, 1);
    if (job.is_err()) {
        return ustl::err(job.unwrap_err());
    }
    return ustl::ok(LaunchRequest{
        .txid = header->txid,
        .info =
            LaunchInfo{
                .executable = executable.unwrap(),
                .job = job.unwrap(),
                .name = chars(message.bytes.subspan(LAUNCH_NAME_OFFSET, name_size)),
            },
    });
}

auto encode_launch_reply(std::span<u8> const bytes, std::span<Uandle> const handles, u32 const txid,
                         LaunchReply const &reply) -> Encoded {
    if (reply.status != Status::Ok) {
        if (reply.process != Uandle::Invalid) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, LAUNCH, txid, reply.status);
    }
    auto started = wire::start_ok_reply(bytes, wire::STATUS_FORM_SIZE, LAUNCH, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status =
            wire::encode_handle(message, REPLY_PROCESS_OFFSET, handles, 0, reply.process);
        status != Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(1)});
}

auto decode_launch_reply(Message const message, u32 const txid)
    -> ustl::Result<LaunchReply, Status> {
    auto status = wire::decode_reply_status(message, LAUNCH, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (status.unwrap() != Status::Ok) {
        return ustl::ok(LaunchReply{.status = status.unwrap(), .process = Uandle::Invalid});
    }
    if (message.bytes.size() != wire::STATUS_FORM_SIZE || message.handles.size() != 1) {
        return ustl::err(Status::InvArgs);
    }
    auto process = wire::decode_handle(message, REPLY_PROCESS_OFFSET, 0);
    if (process.is_err()) {
        return ustl::err(process.unwrap_err());
    }
    return ustl::ok(LaunchReply{.status = Status::Ok, .process = process.unwrap()});
}
} // namespace prot::process
