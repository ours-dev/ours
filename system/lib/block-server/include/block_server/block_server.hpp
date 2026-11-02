/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BLOCK_SERVER_BLOCK_SERVER_HPP
#define BLOCK_SERVER_BLOCK_SERVER_HPP 1

#include <array>
#include <span>

#include <loop/loop.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>
#include <ours_block/ours_block.hpp>
#include <server/server.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The block-driver library: what a block device driver subclasses, so that
/// its author writes the hardware and nothing else.
///
/// The objects:
///   - the `ProtocolServer` base, which owns the connection table, the
///     message pump, and the reply and epitaph helpers (server/server.hpp);
///   - the slot table, one `Slot` per in-flight hardware request: the parked
///     request's channel, generation, txid, op and extent, and the reply
///     bytes it will be answered from;
///   - the derived driver, which implements `geometry()` -- the answer to
///     GetInfo -- and `start(slot, data)`, which programs one request into
///     the hardware and returns once it is offered;
///   - the driver's completion path, which maps the hardware's own request
///     identifier (a virtio used-ring index, an NVMe command id) back to a
///     slot and calls `complete(slot, status, data)`.
///
/// The events, one READ on an interrupt-driven driver:
///   1. the message pump calls on_message(); the library decodes the Read,
///      takes a free slot, and calls start(slot, {}) -- the driver offers the
///      work to the device and returns;
///   2. on_message returns Rearm: the connection stays, and the next request
///      of this client (or any client) may arrive while the disk works;
///   3. the device completes; the driver's IRQ callback maps its hardware id
///      to the slot and calls complete(slot, Ok, data);
///   4. the library encodes the reply into the slot's own bytes, writes it
///      to the parked channel, and frees the slot.
///
/// A request that arrives when every slot is busy is answered with
/// `ShouldWait` and the connection is kept: the client retries. This is the
/// backpressure of a fixed tag depth, the answer a full hardware queue
/// gives.
///
/// One thread, as the Loop it runs on: on_message and complete both run to
/// completion, so the slot state needs no lock. A parked request's channel
/// is checked against the client table at completion -- a client that went
/// away, and a handle value the kernel has since reissued to a new
/// connection, are both dropped silently -- which is what the slot's
/// generation field buys.
///
/// The slot table is also where a future IO scheduler seats itself: a
/// queueing policy between slot allocation and start(), and a dispatch in
/// the completion path. Nothing here is paid for it in advance.
namespace block_server {

/// What the parked request asks the hardware for.
enum class Op : u8 {
    Read,
    Write,
};

/// The device's geometry, as GetInfo answers it.
struct Geometry {
    u32 block_size = 0;
    u64 blocks = 0;
};

/// A READ reply's largest size: the header, the status, and the most data
/// one request carries. The per-slot reply buffer is this long, since other
/// callbacks run while a request is parked and a shared reply buffer would
/// be overwritten.
inline constexpr auto REPLY_BYTES =
    usize{20 + ours_block::MAX_READ_BLOCKS * ours_block::BLOCK_SIZE};
static_assert(REPLY_BYTES <= wire::MAX_MESSAGE_BYTES);

/// One parked request: what `start()` reads and what `complete()` answers
/// from, kept for as long as the hardware works on it.
struct Slot {
    /// Invalid while the slot is free. At completion it is checked against
    /// the client table together with |generation|: a client that went away,
    /// and a handle value the kernel has since reissued to a new connection,
    /// both drop.
    ours::Uandle channel = ours::Uandle::Invalid;
    u32 generation = 0;
    u32 txid = 0;
    Op op = Op::Read;
    u64 lba = 0;
    u32 count = 0;
    std::array<u8, REPLY_BYTES> reply = {};
};

/// The block-driver library's server. |MAX_INFLIGHT| is how many requests
/// the hardware may work on at once -- the tag depth -- and is the driver's
/// to size to its device.
template <typename Derived, usize MaxClients, usize MaxInflight>
class BlockServer
    : public server::ProtocolServer<BlockServer<Derived, MaxClients, MaxInflight>, MaxClients> {
  public:
    using Self = BlockServer<Derived, MaxClients, MaxInflight>;
    using Base = server::ProtocolServer<Self, MaxClients>;

    /// The base's clients and buffers, and the slot table.
    struct Storage {
        typename Base::Storage base;
        std::array<Slot, MaxInflight> slots;
    };

    explicit BlockServer(Storage &storage, loop::Loop &loop)
        : Base(storage.base, loop), m_storage(&storage) {}

    /// The request parked in slot |index|, for start() to read.
    [[nodiscard]]
    auto slot(usize const index) const -> Slot const & {
        return m_storage->slots[index];
    }

    /// How many requests are parked, for a driver to size its device to.
    [[nodiscard]]
    auto in_flight() const -> usize {
        auto parked = usize{0};
        for (auto const &slot: m_storage->slots) {
            if (slot.channel != ours::Uandle::Invalid) {
                parked += 1;
            }
        }
        return parked;
    }

  protected:
    /// Writes the completion of the request parked in slot |index|: the
    /// reply is built into the slot's own bytes -- READ's |data| may view the
    /// driver's DMA mapping, channel_write copies -- and written to the
    /// parked channel. A parked channel that is gone (the client left, or
    /// the kernel reissued the handle value) drops the reply and frees the
    /// slot silently. Callable from any callback on the Loop thread.
    ///
    /// The driver's side of the contract: an Ok completion of a Read carries
    /// exactly |count| blocks -- the codec refuses any other size, and a
    /// reply that cannot be encoded is a driver bug, said on the console and
    /// answered by ending the connection rather than by an answer that
    /// promises bytes that do not exist.
    auto complete(usize const index, ours::Status const status, std::span<u8 const> const data = {})
        -> void {
        auto &slot = m_storage->slots[index];
        if (!still_connected(slot)) {
            free_slot(slot);
            return;
        }
        if (slot.op == Op::Read && status == ours::Status::Ok &&
            data.size() != usize{slot.count} * ours_block::BLOCK_SIZE) {
            // The driver's bug, said the one way a user-space library says
            // anything: the debug console.
            constexpr auto LINE = std::string_view{
                "block-server: the driver's completion of a Read does not carry the blocks "
                "its request asked for\n"};
            auto _ = ours::debug_write(LINE.data(), static_cast<u32>(LINE.size()));
            auto _ = Base::close_client(slot.channel);
            free_slot(slot);
            return;
        }
        auto const encoded = encode_reply(slot, status, data);
        if (encoded.is_err() || Base::reply(slot.channel, encoded.unwrap()) != ours::Status::Ok) {
            // A reply that cannot be written leaves its client waiting for
            // good, so the connection ends, as the base's reply() documents.
            auto _ = Base::close_client(slot.channel);
        }
        free_slot(slot);
    }

  private:
    auto derived() -> Derived * {
        return static_cast<Derived *>(this);
    }

    /// The CRTP hooks, which the driver implements: what GetInfo answers,
    /// and how one parked request becomes hardware work. |data| is the
    /// Write's bytes, to be placed where the device reads them from; it is
    /// empty for a Read. start() returns Ok once the work is offered; an
    /// error means the hardware refused before anything was in flight, and
    /// the library answers the status inline and frees the slot.
    auto geometry() -> Geometry {
        return derived()->geometry();
    }
    auto start(usize const index, std::span<u8 const> const data) -> ours::Status {
        return derived()->start(index, data);
    }

    /// Whether the parked channel still names the connection it parked on:
    /// the client table holds the entry with this channel and generation.
    [[nodiscard]]
    auto still_connected(Slot const &slot) const -> bool {
        for (auto const &client: m_storage->base.clients) {
            if (client.channel == slot.channel && client.generation == slot.generation) {
                return true;
            }
        }
        return false;
    }

    auto free_slot(Slot &slot) -> void {
        slot.channel = ours::Uandle::Invalid;
    }

    auto encode_reply(Slot &slot, ours::Status const status, std::span<u8 const> const data)
        -> ustl::Result<wire::Message, ours::Status> {
        return encode_form(slot.reply, slot.op, slot.txid, status, data);
    }

    /// Encodes |status| in the request's own reply form: a Read's reply may
    /// carry data, a Write's is the status only. The backpressure answer
    /// uses it, having no slot to encode from.
    static auto encode_form(std::span<u8> bytes, Op const op, u32 const txid,
                            ours::Status const status, std::span<u8 const> const data)
        -> ustl::Result<wire::Message, ours::Status> {
        if (op == Op::Read) {
            return ours_block::encode_read_reply(bytes, txid, {.status = status, .data = data});
        }
        return ours_block::encode_write_reply(bytes, txid, {.status = status});
    }

  public:
    /// The pump's dispatch, which the base calls. A decoder refusal or an
    /// unknown ordinal closes the connection without an epitaph, as
    /// ours.block's contract states.
    auto on_message(ours::Uandle const channel, wire::Header const header, wire::Message &&message)
        -> loop::Action {
        if (header.ordinal == ours_block::CLOSE.ordinal) {
            auto const request = ours_block::decode_close(message);
            if (request.is_err()) {
                Base::close_handles(message);
                return Base::close_client(channel);
            }
            auto const answer = ours_block::encode_close_reply(Base::reply_buffer(), request->txid,
                                                               {.status = ours::Status::Ok});
            if (answer.is_err() || Base::reply(channel, answer.unwrap()) != ours::Status::Ok) {
                return Base::close_client(channel);
            }
            return Base::close_client(channel);
        }
        if (header.ordinal == ours_block::GET_INFO.ordinal) {
            auto const request = ours_block::decode_get_info(message);
            if (request.is_err()) {
                Base::close_handles(message);
                return Base::close_client(channel);
            }
            auto const geo = geometry();
            auto const answer = ours_block::encode_get_info_reply(
                Base::reply_buffer(), request->txid,
                {.status = ours::Status::Ok, .block_size = geo.block_size, .blocks = geo.blocks});
            if (answer.is_err() || Base::reply(channel, answer.unwrap()) != ours::Status::Ok) {
                return Base::close_client(channel);
            }
            return loop::Action::Rearm;
        }
        if (header.ordinal == ours_block::READ.ordinal) {
            auto const request = ours_block::decode_read(message);
            if (request.is_err()) {
                Base::close_handles(message);
                return Base::close_client(channel);
            }
            return park(channel, Op::Read, request->txid, request->lba, request->count, {});
        }
        if (header.ordinal == ours_block::WRITE.ordinal) {
            auto const request = ours_block::decode_write(message);
            if (request.is_err()) {
                Base::close_handles(message);
                return Base::close_client(channel);
            }
            return park(channel, Op::Write, request->txid, request->lba, request->count,
                        request->data);
        }
        Base::close_handles(message);
        return Base::close_client(channel);
    }

  private:
    /// Parks the request in a free slot and hands it to the driver; answers
    /// the backpressure status when the tag depth is exhausted, and answers
    /// start()'s own error inline -- the connection is kept in both cases,
    /// as the inline model answered them.
    auto park(ours::Uandle const channel, Op const op, u32 const txid, u64 const lba,
              u32 const count, std::span<u8 const> const data) -> loop::Action {
        auto *const free = take_slot(channel);
        if (free == nullptr) {
            // The tag depth is exhausted: the answer is the request's own
            // reply form carrying ShouldWait, and the connection stays.
            auto const answer =
                encode_form(Base::reply_buffer(), op, txid, ours::Status::ShouldWait, {});
            if (answer.is_err() || Base::reply(channel, answer.unwrap()) != ours::Status::Ok) {
                return Base::close_client(channel);
            }
            return loop::Action::Rearm;
        }
        free->txid = txid;
        free->op = op;
        free->lba = lba;
        free->count = count;
        if (auto const status = start(index_of(*free), data); status != ours::Status::Ok) {
            // The hardware refused the work before it was offered: the reply
            // is the status, and the slot is free again.
            auto const answer = encode_reply(*free, status, {});
            free_slot(*free);
            if (answer.is_err() || Base::reply(channel, answer.unwrap()) != ours::Status::Ok) {
                return Base::close_client(channel);
            }
            return loop::Action::Rearm;
        }
        return loop::Action::Rearm;
    }

    auto take_slot(ours::Uandle const channel) -> Slot * {
        for (auto &slot: m_storage->slots) {
            if (slot.channel != ours::Uandle::Invalid) {
                continue;
            }
            // The generation comes from the client entry this channel is
            // connected on: the pair is what complete() checks against.
            for (auto const &client: m_storage->base.clients) {
                if (client.channel == channel) {
                    slot.generation = client.generation;
                    break;
                }
            }
            slot.channel = channel;
            return &slot;
        }
        return nullptr;
    }

    [[nodiscard]]
    auto index_of(Slot const &slot) const -> usize {
        return static_cast<usize>(&slot - m_storage->slots.data());
    }

    Storage *m_storage;
};
} // namespace block_server

#endif // #ifndef BLOCK_SERVER_BLOCK_SERVER_HPP
