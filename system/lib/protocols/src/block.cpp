/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prot/block.hpp>

#include <cstring>

#include <ours/vdso.hpp>

namespace prot::block {
namespace {
    using ours::Status;
    using ours::Uandle;

    constexpr auto BODY_OFFSET = usize{16};
    constexpr auto STATUS_FORM_SIZE = usize{wire::STATUS_FORM_SIZE};
    // The Ok replies' first field, right after the status.
    constexpr auto REPLY_FIELD_OFFSET = usize{20};

    /// Block.GetInfo's reply: the block size, then the capacity, no trailing
    /// array.
    constexpr auto GET_INFO_REPLY_SIZE = usize{32};

    /// Block.Read's request: the block number, then the count.
    constexpr auto READ_REQUEST_SIZE = usize{28};

    /// Block.Read's reply: the status, then the data, which ends the message.
    constexpr auto READ_DATA_OFFSET = usize{20};

    /// Block.Write's request: the block number, then the count, then the
    /// blocks, which end the message.
    constexpr auto WRITE_DATA_OFFSET = usize{28};

    auto start_request(std::span<u8> bytes, wire::Method const &method, u32 const txid,
                       usize const size) -> ustl::Result<std::span<u8>, Status> {
        if (txid == 0 || txid > wire::MAX_TXID) {
            return ustl::err(Status::InvArgs);
        }
        return wire::start_request(bytes, size, method, txid);
    }
} // namespace

auto encode_close(std::span<u8> bytes, CloseRequest const &request)
    -> ustl::Result<wire::Message, Status> {
    auto started = start_request(bytes, CLOSE, request.txid, BODY_OFFSET);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    return ustl::ok(wire::Message{.bytes = started.unwrap(), .handles = {}});
}

auto decode_close(wire::Message message) -> ustl::Result<CloseRequest, Status> {
    auto header = wire::decode_request_header(message, CLOSE);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != BODY_OFFSET || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(CloseRequest{.txid = header->txid});
}

auto encode_close_reply(std::span<u8> bytes, u32 txid, CloseReply const &reply)
    -> ustl::Result<wire::Message, Status> {
    if (txid == 0 || txid > wire::MAX_TXID) {
        return ustl::err(Status::InvArgs);
    }
    // Close's reply is the status form, whatever the status: the connection
    // ends with the reply either way.
    return wire::encode_status_reply(bytes, CLOSE, txid, reply.status);
}

auto encode_get_info(std::span<u8> bytes, GetInfoRequest const &request)
    -> ustl::Result<wire::Message, Status> {
    auto started = start_request(bytes, GET_INFO, request.txid, BODY_OFFSET);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    return ustl::ok(wire::Message{.bytes = started.unwrap(), .handles = {}});
}

auto decode_get_info(wire::Message message) -> ustl::Result<GetInfoRequest, Status> {
    auto header = wire::decode_request_header(message, GET_INFO);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != BODY_OFFSET || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(GetInfoRequest{.txid = header->txid});
}

auto encode_read(std::span<u8> bytes, ReadRequest const &request)
    -> ustl::Result<wire::Message, Status> {
    if (request.count == 0 || request.count > MAX_READ_BLOCKS) {
        return ustl::err(Status::InvArgs);
    }
    auto started = start_request(bytes, READ, request.txid, READ_REQUEST_SIZE);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, BODY_OFFSET, request.lba);
    wire::store(message, BODY_OFFSET + 8, request.count);
    return ustl::ok(wire::Message{.bytes = message, .handles = {}});
}

auto decode_read(wire::Message message) -> ustl::Result<ReadRequest, Status> {
    auto header = wire::decode_request_header(message, READ);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != READ_REQUEST_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto const request = ReadRequest{
        .txid = header->txid,
        .lba = wire::load<u64>(message.bytes, BODY_OFFSET),
        .count = wire::load<u32>(message.bytes, BODY_OFFSET + 8),
    };
    if (request.count == 0 || request.count > MAX_READ_BLOCKS) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(request);
}

auto encode_write(std::span<u8> bytes, WriteRequest const &request)
    -> ustl::Result<wire::Message, Status> {
    if (request.count == 0 || request.count > MAX_READ_BLOCKS ||
        request.data.size() != request.count * BLOCK_SIZE) {
        return ustl::err(Status::InvArgs);
    }
    auto started =
        start_request(bytes, WRITE, request.txid, WRITE_DATA_OFFSET + request.data.size());
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, BODY_OFFSET, request.lba);
    wire::store(message, BODY_OFFSET + 8, request.count);
    std::memcpy(message.data() + WRITE_DATA_OFFSET, request.data.data(), request.data.size());
    return ustl::ok(wire::Message{.bytes = message, .handles = {}});
}

auto decode_write(wire::Message message) -> ustl::Result<WriteRequest, Status> {
    auto header = wire::decode_request_header(message, WRITE);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < WRITE_DATA_OFFSET || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto request = WriteRequest{
        .txid = header->txid,
        .lba = wire::load<u64>(message.bytes, BODY_OFFSET),
        .count = wire::load<u32>(message.bytes, BODY_OFFSET + 8),
    };
    if (request.count == 0 || request.count > MAX_READ_BLOCKS ||
        message.bytes.size() != WRITE_DATA_OFFSET + request.count * BLOCK_SIZE) {
        return ustl::err(Status::InvArgs);
    }
    request.data = message.bytes.subspan(WRITE_DATA_OFFSET);
    return ustl::ok(request);
}

auto encode_get_info_reply(std::span<u8> bytes, u32 txid, GetInfoReply const &reply)
    -> ustl::Result<wire::Message, Status> {
    if (txid == 0 || txid > wire::MAX_TXID) {
        return ustl::err(Status::InvArgs);
    }
    if (reply.status != Status::Ok) {
        if (reply.block_size != 0 || reply.blocks != 0) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, GET_INFO, txid, reply.status);
    }
    auto started = wire::start_ok_reply(bytes, GET_INFO_REPLY_SIZE, GET_INFO, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, REPLY_FIELD_OFFSET, reply.block_size);
    wire::store(message, REPLY_FIELD_OFFSET + 4, reply.blocks);
    return ustl::ok(wire::Message{.bytes = message, .handles = {}});
}

auto decode_get_info_reply(wire::Message message, u32 txid) -> ustl::Result<GetInfoReply, Status> {
    auto status = wire::decode_reply_status(message, GET_INFO, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (status.unwrap() != Status::Ok) {
        if (message.bytes.size() != STATUS_FORM_SIZE || !message.handles.empty()) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(GetInfoReply{.status = status.unwrap()});
    }
    if (message.bytes.size() != GET_INFO_REPLY_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(GetInfoReply{
        .status = Status::Ok,
        .block_size = wire::load<u32>(message.bytes, REPLY_FIELD_OFFSET),
        .blocks = wire::load<u64>(message.bytes, REPLY_FIELD_OFFSET + 4),
    });
}

auto encode_read_reply(std::span<u8> bytes, u32 txid, ReadReply const &reply)
    -> ustl::Result<wire::Message, Status> {
    if (txid == 0 || txid > wire::MAX_TXID) {
        return ustl::err(Status::InvArgs);
    }
    if (reply.status != Status::Ok) {
        if (!reply.data.empty()) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, READ, txid, reply.status);
    }
    if (reply.data.size() == 0 || reply.data.size() % BLOCK_SIZE != 0 ||
        reply.data.size() > MAX_READ_BLOCKS * BLOCK_SIZE) {
        return ustl::err(Status::InvArgs);
    }
    auto started = wire::start_ok_reply(bytes, READ_DATA_OFFSET + reply.data.size(), READ, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    std::memcpy(message.data() + READ_DATA_OFFSET, reply.data.data(), reply.data.size());
    return ustl::ok(wire::Message{.bytes = message, .handles = {}});
}

auto decode_read_reply(wire::Message message, u32 txid) -> ustl::Result<ReadReply, Status> {
    auto status = wire::decode_reply_status(message, READ, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (status.unwrap() != Status::Ok) {
        if (message.bytes.size() != STATUS_FORM_SIZE || !message.handles.empty()) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(ReadReply{.status = status.unwrap()});
    }
    if (message.bytes.size() < READ_DATA_OFFSET || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto const data = message.bytes.subspan(READ_DATA_OFFSET);
    if (data.size() == 0 || data.size() % BLOCK_SIZE != 0) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(ReadReply{
        .status = Status::Ok,
        .data = data,
    });
}

auto encode_write_reply(std::span<u8> bytes, u32 txid, WriteReply const &reply)
    -> ustl::Result<wire::Message, Status> {
    if (txid == 0 || txid > wire::MAX_TXID) {
        return ustl::err(Status::InvArgs);
    }
    // Write's reply is the status form, whatever the status: nothing follows
    // it on Ok.
    return wire::encode_status_reply(bytes, WRITE, txid, reply.status);
}

auto decode_write_reply(wire::Message message, u32 txid) -> ustl::Result<WriteReply, Status> {
    auto status = wire::decode_reply_status(message, WRITE, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (message.bytes.size() != STATUS_FORM_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(WriteReply{.status = status.unwrap()});
}
} // namespace prot::block
