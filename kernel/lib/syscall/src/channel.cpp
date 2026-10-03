/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/channel_dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/message_packet.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
using object::ChannelDispatcher;
using object::KernelHandle;
using object::MessagePacket;
using object::ProcessDispatcher;

auto channel_create(ChannelCreateOptions const options,
                    user_copy::UserOutPtr<ours::Uandle> const out0,
                    user_copy::UserOutPtr<ours::Uandle> const out1) -> Status {
    if (options != ChannelCreateOptions::None) {
        return Status::InvArgs;
    }

    KernelHandle<ChannelDispatcher> end0;
    KernelHandle<ChannelDispatcher> end1;
    Rights rights = Rights::None;
    if (auto const status = ChannelDispatcher::create(&end0, &end1, &rights);
        status != Status::Ok) {
        return status;
    }

    auto handle0 = object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(end0)), rights);
    if (!handle0) {
        return Status::NoResource;
    }
    auto handle1 = object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(end1)), rights);
    if (!handle1) {
        return Status::NoResource;
    }

    // Both values go out to the caller before either handle goes into the
    // table (design 4.1). A value is a pure function of the handle and the
    // table, so it is known before the handle is installed; if either copy
    // fails, both handles simply die here and nothing was ever visible --
    // there is no window in which a program could see, use or close a handle
    // this call then took back.
    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = out0.copy_to_user(table.value_of(*handle0)); status != Status::Ok) {
        return status;
    }
    if (auto const status = out1.copy_to_user(table.value_of(*handle1)); status != Status::Ok) {
        return status;
    }

    static_cast<void>(table.add(ustl::move(handle0)));
    static_cast<void>(table.add(ustl::move(handle1)));
    return Status::Ok;
}

auto channel_write(ours::Uandle const channel, ChannelWriteOptions const options,
                   user_copy::UserInPtr<u8 const> const bytes, u32 const num_bytes,
                   user_copy::UserInPtr<ours::Uandle const> const handles, u32 const num_handles)
    -> Status {
    // The handle values first, out of user memory and outside every lock:
    // every handle they name leaves the caller's table below whatever this
    // call ends up answering, and that needs the values. A count over the
    // limit or a buffer that faults consumes nothing -- no value reaches the
    // table: the count is refused before the copy, and a copy that faults
    // returns before remove_for_transfer() -- which is the one way out that
    // leaves the table as it was. Zircon reads an
    // over-limit array in chunks and consumes it (RemoveUserHandles()).
    if (num_handles > CHANNEL_MAX_MSG_HANDLES) {
        return Status::OutOfRange;
    }
    ours::Uandle values[CHANNEL_MAX_MSG_HANDLES];
    if (num_handles != 0) {
        if (auto const status = handles.copy_array_from_user(values, num_handles);
            status != Status::Ok) {
            return status;
        }
    }

    // The channel, before the handles leave: it may be one of them, and a
    // reference held here keeps it alive while the write refuses it.
    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Arc<ChannelDispatcher> dispatcher;
    auto const resolved = table.get_dispatcher(channel, Rights::Write, &dispatcher);

    // Consumed now, whatever comes next: every handle named is out of the
    // table and in |taken|. One that never reaches a packet is closed when
    // |taken| goes; one in a packet write() refuses is closed with the
    // packet as write() returns; both after every lock. Zircon's
    // channel_write() gets the same effect from a deferred
    // RemoveUserHandles().
    ustl::Box<object::Handle> taken[CHANNEL_MAX_MSG_HANDLES];
    auto const removed = table.remove_for_transfer(values, num_handles, dispatcher.as_ptr(), taken);

    // Then the answer, in the order of design 4.5: the channel handle first
    // (BadHandle > BadType > AccessDenied), then the arguments.
    if (resolved != Status::Ok) {
        return resolved;
    }
    if (options != ChannelWriteOptions::None) {
        return Status::InvArgs;
    }
    if (removed != Status::Ok) {
        return removed;
    }
    if (num_bytes > CHANNEL_MAX_MSG_BYTES) {
        return Status::OutOfRange;
    }

    // The copy from user memory, before the channel's lock is taken. The
    // handles move into the packet only if it is made.
    ustl::Box<MessagePacket> message;
    if (auto const status = MessagePacket::create(bytes, num_bytes, taken, num_handles, &message);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->write(ustl::move(message));
}

auto channel_read(ours::Uandle const channel, ChannelReadOptions const options,
                  user_copy::UserOutPtr<u8> const bytes,
                  user_copy::UserOutPtr<ours::Uandle> const handles, u32 const num_bytes,
                  u32 const num_handles, user_copy::UserOutPtr<u32> const actual_bytes,
                  user_copy::UserOutPtr<u32> const actual_handles) -> Status {
    // The handle first, then the arguments: BadHandle > BadType >
    // AccessDenied > InvArgs (design 4.5). A program that passes a closed
    // handle and a bad option learns about the handle, which is the mistake
    // it has to fix first.
    ustl::Arc<ChannelDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            channel, Rights::Read, &dispatcher);
        status != Status::Ok) {
        return status;
    }

    if (options != ChannelReadOptions::None) {
        return Status::InvArgs;
    }

    u32 length = 0;
    u32 count = 0;
    ustl::Box<MessagePacket> message;
    auto const status = dispatcher->read(num_bytes, num_handles, &length, &count, &message);

    // The sizes are reported both for a message delivered and for one that
    // did not fit -- the second is how a caller learns how big a buffer to
    // come back with. Both out-pointers are optional, as in Zircon.
    if (status == Status::Ok || status == Status::BufferTooSmall) {
        if (!actual_bytes.is_null()) {
            if (auto const copied = actual_bytes.copy_to_user(length); copied != Status::Ok) {
                return copied;
            }
        }
        if (!actual_handles.is_null()) {
            if (auto const copied = actual_handles.copy_to_user(count); copied != Status::Ok) {
                return copied;
            }
        }
    }
    if (status != Status::Ok) {
        return status;
    }

    // After the channel's lock was released inside read(). A fault here loses
    // the message, which has already left the queue -- Zircon's
    // channel_read() makes the same trade: the alternative is copying to
    // user memory with a channel lock held.
    if (auto const copied = message->copy_data_to(bytes); copied != Status::Ok) {
        return copied;
    }
    if (count == 0) {
        return Status::Ok;
    }

    // Then the handles: their values out first, and only once that copy has
    // worked are they installed. A value is a pure function of the handle and
    // this table, so it is known before the handle is in the table; if the
    // copy faults the handles die with |taken| and nothing was ever visible
    // -- the same order channel_create() publishes in, and Zircon's
    // msg_get_handles(), which copies the values out before it moves a
    // single handle into the table.
    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Box<object::Handle> taken[CHANNEL_MAX_MSG_HANDLES];
    message->take_handles(taken);
    ours::Uandle values[CHANNEL_MAX_MSG_HANDLES];
    for (u32 i = 0; i < count; ++i) {
        values[i] = table.value_of(*taken[i]);
    }
    if (auto const copied = handles.copy_array_to_user(values, count); copied != Status::Ok) {
        return copied;
    }
    for (u32 i = 0; i < count; ++i) {
        static_cast<void>(table.add(ustl::move(taken[i])));
    }
    return Status::Ok;
}
} // namespace ours::syscall
