/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours_io/ours_io.hpp>

#include <algorithm>
#include <utility>

#include <cstring>

namespace ours_io {
namespace {
    using ours::Status;
    using ours::Uandle;
    using wire::Message;

    using Encoded = ustl::Result<Message, Status>;

    // Directory.Open.
    constexpr auto OPEN_FLAGS_OFFSET = usize{16};
    constexpr auto OPEN_OBJECT_OFFSET = usize{24};
    constexpr auto OPEN_PATH_SIZE_OFFSET = usize{28};
    constexpr auto OPEN_PATH_OFFSET = usize{32};

    // File.Write: the data's size, then the data, which ends the message.
    constexpr auto WRITE_DATA_OFFSET = usize{20};

    // The requests with a body of one u64, or of one u32 and a zero u32:
    // Clone, Read, GetBackingMemory and ReadDirents.
    constexpr auto BODY_OFFSET = usize{16};
    constexpr auto BODY_PAD_OFFSET = usize{20};
    constexpr auto SHORT_REQUEST_SIZE = usize{24};

    // The Ok replies: a size or a handle after the status, then the payload.
    constexpr auto REPLY_FIELD_OFFSET = usize{20};
    constexpr auto REPLY_PAYLOAD_OFFSET = usize{24};

    static_assert(wire::STATUS_OFFSET + sizeof(u32) == REPLY_FIELD_OFFSET);
    static_assert(REPLY_PAYLOAD_OFFSET + MAX_TRANSFER_SIZE == wire::MAX_MESSAGE_BYTES);
    static_assert(OPEN_PATH_OFFSET + MAX_PATH_LENGTH <= wire::MAX_MESSAGE_BYTES);

    /// The message's bytes as a string, for Open's path and a dirent's name.
    auto chars(std::span<u8 const> const bytes) -> std::string_view {
        return {reinterpret_cast<char const *>(bytes.data()), bytes.size()};
    }

    /// A request whose body is one u64: Read's count, ReadDirents' max_bytes.
    auto encode_u64_request(std::span<u8> const bytes, wire::Method const &method, u32 const txid,
                            u64 const value) -> Encoded {
        auto started = wire::start_request(bytes, SHORT_REQUEST_SIZE, method, txid);
        if (started.is_err()) {
            return ustl::err(started.unwrap_err());
        }
        auto const message = started.unwrap();
        wire::store(message, BODY_OFFSET, value);
        return ustl::ok(Message{.bytes = message, .handles = {}});
    }

    struct U64Request {
        u32 txid;
        u64 value;
    };

    auto decode_u64_request(Message const message, wire::Method const &method)
        -> ustl::Result<U64Request, Status> {
        auto header = wire::decode_request_header(message, method);
        if (header.is_err()) {
            return ustl::err(header.unwrap_err());
        }
        if (message.bytes.size() != SHORT_REQUEST_SIZE || !message.handles.empty()) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(U64Request{
            .txid = header->txid,
            .value = wire::load<u64>(message.bytes, BODY_OFFSET),
        });
    }

    /// An Ok reply that ends in a byte array, Read's or ReadDirents', or the
    /// status form when |status| is not Ok.
    auto encode_payload_reply(std::span<u8> const bytes, wire::Method const &method, u32 const txid,
                              Status const status, std::span<u8 const> const payload) -> Encoded {
        if (status != Status::Ok) {
            if (!payload.empty()) {
                return ustl::err(Status::InvArgs);
            }
            return wire::encode_status_reply(bytes, method, txid, status);
        }
        if (payload.size() > MAX_TRANSFER_SIZE) {
            return ustl::err(Status::InvArgs);
        }
        auto const size = REPLY_PAYLOAD_OFFSET + payload.size();
        if (bytes.size() < size) {
            return ustl::err(Status::BufferTooSmall);
        }
        // The payload first, then the header: start_ok_reply() zeroes the
        // message's first bytes, and a payload may lie anywhere in |bytes|,
        // in place at reply_payload() included. memmove, since it may
        // overlap where it goes. An empty span may have no data pointer, which
        // memmove must not be given even for zero bytes.
        if (!payload.empty()) {
            std::memmove(bytes.data() + REPLY_PAYLOAD_OFFSET, payload.data(), payload.size());
        }
        auto started = wire::start_ok_reply(bytes, REPLY_PAYLOAD_OFFSET, method, txid);
        if (started.is_err()) {
            return ustl::err(started.unwrap_err());
        }
        wire::store(bytes, REPLY_FIELD_OFFSET, static_cast<u32>(payload.size()));
        return ustl::ok(Message{.bytes = bytes.first(size), .handles = {}});
    }

    struct PayloadReply {
        Status status;
        std::span<u8 const> payload;
    };

    auto decode_payload_reply(Message const message, wire::Method const &method, u32 const txid)
        -> ustl::Result<PayloadReply, Status> {
        auto status = wire::decode_reply_status(message, method, txid);
        if (status.is_err()) {
            return ustl::err(status.unwrap_err());
        }
        if (status.unwrap() != Status::Ok) {
            return ustl::ok(PayloadReply{.status = status.unwrap(), .payload = {}});
        }
        if (message.bytes.size() < REPLY_PAYLOAD_OFFSET || !message.handles.empty()) {
            return ustl::err(Status::InvArgs);
        }
        auto const size = wire::load<u32>(message.bytes, REPLY_FIELD_OFFSET);
        if (size > MAX_TRANSFER_SIZE || message.bytes.size() != REPLY_PAYLOAD_OFFSET + size) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(PayloadReply{
            .status = Status::Ok,
            .payload = message.bytes.subspan(REPLY_PAYLOAD_OFFSET, size),
        });
    }
} // namespace

auto encode_open(std::span<u8> const bytes, std::span<Uandle> const handles,
                 OpenRequest const &request) -> Encoded {
    if (request.path.size() > MAX_PATH_LENGTH) {
        return ustl::err(Status::InvArgs);
    }
    auto started = wire::start_request(bytes, OPEN_PATH_OFFSET + request.path.size(), OPEN, 0);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status =
            wire::encode_handle(message, OPEN_OBJECT_OFFSET, handles, 0, request.object);
        status != Status::Ok) {
        return ustl::err(status);
    }
    wire::store(message, OPEN_FLAGS_OFFSET, std::to_underlying(request.flags));
    wire::store(message, OPEN_PATH_SIZE_OFFSET, static_cast<u32>(request.path.size()));
    if (!request.path.empty()) {
        std::memcpy(message.data() + OPEN_PATH_OFFSET, request.path.data(), request.path.size());
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(1)});
}

auto decode_open(Message const message) -> ustl::Result<OpenRequest, Status> {
    if (auto header = wire::decode_request_header(message, OPEN); header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < OPEN_PATH_OFFSET || message.handles.size() != 1) {
        return ustl::err(Status::InvArgs);
    }
    auto const path_size = wire::load<u32>(message.bytes, OPEN_PATH_SIZE_OFFSET);
    if (path_size > MAX_PATH_LENGTH || message.bytes.size() != OPEN_PATH_OFFSET + path_size) {
        return ustl::err(Status::InvArgs);
    }
    auto object = wire::decode_handle(message, OPEN_OBJECT_OFFSET, 0);
    if (object.is_err()) {
        return ustl::err(object.unwrap_err());
    }
    return ustl::ok(OpenRequest{
        .flags = static_cast<Flags>(wire::load<u64>(message.bytes, OPEN_FLAGS_OFFSET)),
        .path = chars(message.bytes.subspan(OPEN_PATH_OFFSET, path_size)),
        .object = object.unwrap(),
    });
}

auto encode_close(std::span<u8> const bytes, CloseRequest const &request) -> Encoded {
    auto started = wire::start_request(bytes, wire::HEADER_SIZE, CLOSE, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    return ustl::ok(Message{.bytes = started.unwrap(), .handles = {}});
}

auto decode_close(Message const message) -> ustl::Result<CloseRequest, Status> {
    auto header = wire::decode_request_header(message, CLOSE);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != wire::HEADER_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(CloseRequest{.txid = header->txid});
}

auto encode_clone(std::span<u8> const bytes, std::span<Uandle> const handles,
                  CloneRequest const &request) -> Encoded {
    auto started = wire::start_request(bytes, SHORT_REQUEST_SIZE, CLONE, 0);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status = wire::encode_handle(message, BODY_OFFSET, handles, 0, request.request);
        status != Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(1)});
}

auto decode_clone(Message const message) -> ustl::Result<CloneRequest, Status> {
    if (auto header = wire::decode_request_header(message, CLONE); header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != SHORT_REQUEST_SIZE || message.handles.size() != 1 ||
        wire::load<u32>(message.bytes, BODY_PAD_OFFSET) != 0) {
        return ustl::err(Status::InvArgs);
    }
    auto request = wire::decode_handle(message, BODY_OFFSET, 0);
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    return ustl::ok(CloneRequest{.request = request.unwrap()});
}

auto encode_read(std::span<u8> const bytes, ReadRequest const &request) -> Encoded {
    return encode_u64_request(bytes, READ, request.txid, request.count);
}

auto decode_read(Message const message) -> ustl::Result<ReadRequest, Status> {
    auto decoded = decode_u64_request(message, READ);
    if (decoded.is_err()) {
        return ustl::err(decoded.unwrap_err());
    }
    return ustl::ok(ReadRequest{.txid = decoded->txid, .count = decoded->value});
}

auto encode_write(std::span<u8> const bytes, WriteRequest const &request) -> Encoded {
    if (request.data.size() > MAX_TRANSFER_SIZE) {
        return ustl::err(Status::InvArgs);
    }
    auto started =
        wire::start_request(bytes, WRITE_DATA_OFFSET + request.data.size(), WRITE, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, BODY_OFFSET, static_cast<u32>(request.data.size()));
    if (!request.data.empty()) {
        std::memcpy(message.data() + WRITE_DATA_OFFSET, request.data.data(), request.data.size());
    }
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_write(Message const message) -> ustl::Result<WriteRequest, Status> {
    auto header = wire::decode_request_header(message, WRITE);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < WRITE_DATA_OFFSET || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto const size = wire::load<u32>(message.bytes, BODY_OFFSET);
    // A message is exactly as long as its fields: the data ends it.
    if (size > MAX_TRANSFER_SIZE || message.bytes.size() != WRITE_DATA_OFFSET + size) {
        return ustl::err(Status::InvArgs);
    }
    auto const data = message.bytes.subspan(WRITE_DATA_OFFSET, size);
    return ustl::ok(WriteRequest{
        .txid = header->txid,
        .data = {data.data(), data.size()},
    });
}

auto encode_get_backing_memory(std::span<u8> const bytes, GetBackingMemoryRequest const &request)
    -> Encoded {
    if ((request.flags & ~VMO_FLAGS_KNOWN) != VmoFlags::None) {
        return ustl::err(Status::InvArgs);
    }
    auto started = wire::start_request(bytes, SHORT_REQUEST_SIZE, GET_BACKING_MEMORY, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, BODY_OFFSET, std::to_underlying(request.flags));
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_get_backing_memory(Message const message)
    -> ustl::Result<GetBackingMemoryRequest, Status> {
    auto header = wire::decode_request_header(message, GET_BACKING_MEMORY);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != SHORT_REQUEST_SIZE || !message.handles.empty() ||
        wire::load<u32>(message.bytes, BODY_PAD_OFFSET) != 0) {
        return ustl::err(Status::InvArgs);
    }
    auto const flags = static_cast<VmoFlags>(wire::load<u32>(message.bytes, BODY_OFFSET));
    if ((flags & ~VMO_FLAGS_KNOWN) != VmoFlags::None) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(GetBackingMemoryRequest{.txid = header->txid, .flags = flags});
}

auto encode_read_dirents(std::span<u8> const bytes, ReadDirentsRequest const &request) -> Encoded {
    return encode_u64_request(bytes, READ_DIRENTS, request.txid, request.max_bytes);
}

auto decode_read_dirents(Message const message) -> ustl::Result<ReadDirentsRequest, Status> {
    auto decoded = decode_u64_request(message, READ_DIRENTS);
    if (decoded.is_err()) {
        return ustl::err(decoded.unwrap_err());
    }
    return ustl::ok(ReadDirentsRequest{.txid = decoded->txid, .max_bytes = decoded->value});
}

auto encode_close_reply(std::span<u8> const bytes, u32 const txid, CloseReply const &reply)
    -> Encoded {
    return wire::encode_status_reply(bytes, CLOSE, txid, reply.status);
}

auto decode_close_reply(Message const message, u32 const txid) -> ustl::Result<CloseReply, Status> {
    auto status = wire::decode_reply_status(message, CLOSE, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    // Close returns nothing, so its Ok reply is the status form as well.
    if (message.bytes.size() != wire::STATUS_FORM_SIZE || !message.handles.empty() ||
        wire::load<u32>(message.bytes, REPLY_FIELD_OFFSET) != 0) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(CloseReply{.status = status.unwrap()});
}

auto encode_read_reply(std::span<u8> const bytes, u32 const txid, ReadReply const &reply)
    -> Encoded {
    return encode_payload_reply(bytes, READ, txid, reply.status, reply.data);
}

auto decode_read_reply(Message const message, u32 const txid) -> ustl::Result<ReadReply, Status> {
    auto decoded = decode_payload_reply(message, READ, txid);
    if (decoded.is_err()) {
        return ustl::err(decoded.unwrap_err());
    }
    return ustl::ok(ReadReply{.status = decoded->status, .data = decoded->payload});
}

auto encode_write_reply(std::span<u8> const bytes, u32 const txid, WriteReply const &reply)
    -> Encoded {
    if (reply.status != Status::Ok) {
        if (reply.actual != 0) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, WRITE, txid, reply.status);
    }
    // The status and the count fill the status form exactly: nothing after
    // the count.
    auto started = wire::start_ok_reply(bytes, wire::STATUS_FORM_SIZE, WRITE, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, REPLY_FIELD_OFFSET, reply.actual);
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_write_reply(Message const message, u32 const txid) -> ustl::Result<WriteReply, Status> {
    auto status = wire::decode_reply_status(message, WRITE, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    // The count sits where the status form's zero goes, so a failure is the
    // plain form: no handles, nothing past the count.
    if (message.bytes.size() != wire::STATUS_FORM_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto const actual = wire::load<u32>(message.bytes, REPLY_FIELD_OFFSET);
    if (status.unwrap() != Status::Ok && actual != 0) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(WriteReply{.status = status.unwrap(), .actual = actual});
}

auto encode_get_backing_memory_reply(std::span<u8> const bytes, std::span<Uandle> const handles,
                                     u32 const txid, GetBackingMemoryReply const &reply)
    -> Encoded {
    if (reply.status != Status::Ok) {
        if (reply.vmo != Uandle::Invalid) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, GET_BACKING_MEMORY, txid, reply.status);
    }
    auto started = wire::start_ok_reply(bytes, wire::STATUS_FORM_SIZE, GET_BACKING_MEMORY, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status = wire::encode_handle(message, REPLY_FIELD_OFFSET, handles, 0, reply.vmo);
        status != Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(1)});
}

auto decode_get_backing_memory_reply(Message const message, u32 const txid)
    -> ustl::Result<GetBackingMemoryReply, Status> {
    auto status = wire::decode_reply_status(message, GET_BACKING_MEMORY, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (status.unwrap() != Status::Ok) {
        return ustl::ok(GetBackingMemoryReply{.status = status.unwrap(), .vmo = Uandle::Invalid});
    }
    if (message.bytes.size() != wire::STATUS_FORM_SIZE || message.handles.size() != 1) {
        return ustl::err(Status::InvArgs);
    }
    auto vmo = wire::decode_handle(message, REPLY_FIELD_OFFSET, 0);
    if (vmo.is_err()) {
        return ustl::err(vmo.unwrap_err());
    }
    return ustl::ok(GetBackingMemoryReply{.status = Status::Ok, .vmo = vmo.unwrap()});
}

auto encode_read_dirents_reply(std::span<u8> const bytes, u32 const txid,
                               ReadDirentsReply const &reply) -> Encoded {
    return encode_payload_reply(bytes, READ_DIRENTS, txid, reply.status, reply.dirents);
}

auto decode_read_dirents_reply(Message const message, u32 const txid)
    -> ustl::Result<ReadDirentsReply, Status> {
    auto decoded = decode_payload_reply(message, READ_DIRENTS, txid);
    if (decoded.is_err()) {
        return ustl::err(decoded.unwrap_err());
    }
    return ustl::ok(ReadDirentsReply{.status = decoded->status, .dirents = decoded->payload});
}

auto reply_payload(std::span<u8> const bytes) -> std::span<u8> {
    if (bytes.size() <= REPLY_PAYLOAD_OFFSET) {
        return {};
    }
    return bytes.subspan(REPLY_PAYLOAD_OFFSET,
                         std::min(bytes.size() - REPLY_PAYLOAD_OFFSET, MAX_TRANSFER_SIZE));
}

auto DirentWriter::append(Dirent const &dirent) -> Status {
    if (dirent.name.empty() || dirent.name.size() > MAX_NAME_LENGTH) {
        return Status::InvArgs;
    }
    auto const size = DIRENT_HEADER_SIZE + dirent.name.size();
    if (size > m_buffer.size() - m_size) {
        return Status::BufferTooSmall;
    }
    auto const entry = m_buffer.subspan(m_size, size);
    wire::store(entry, 0, dirent.ino);
    entry[8] = static_cast<u8>(dirent.name.size());
    entry[9] = std::to_underlying(dirent.type);
    std::memcpy(entry.data() + DIRENT_HEADER_SIZE, dirent.name.data(), dirent.name.size());
    m_size += size;
    return Status::Ok;
}

auto DirentReader::next() -> ustl::Result<ustl::Option<Dirent>, Status> {
    if (m_rest.empty()) {
        return ustl::ok(ustl::Option<Dirent>{ustl::NONE});
    }
    if (m_rest.size() < DIRENT_HEADER_SIZE) {
        return ustl::err(Status::InvArgs);
    }
    auto const name_size = usize{m_rest[8]};
    if (name_size == 0 || m_rest.size() < DIRENT_HEADER_SIZE + name_size) {
        return ustl::err(Status::InvArgs);
    }
    auto const dirent = Dirent{
        .ino = wire::load<u64>(m_rest, 0),
        .type = static_cast<DirentType>(m_rest[9]),
        .name = chars(m_rest.subspan(DIRENT_HEADER_SIZE, name_size)),
    };
    m_rest = m_rest.subspan(DIRENT_HEADER_SIZE + name_size);
    return ustl::ok(ustl::some(dirent));
}
} // namespace ours_io
