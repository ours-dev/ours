/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/channel_dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/message_packet.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>

namespace ours {
using object::ChannelDispatcher;
using object::Handle;
using object::KernelHandle;
using object::MessagePacket;
using object::ProcessDispatcher;

auto sys_channel_create(ChannelCreateOptions const options,
                        user_copy::UserOutPtr<HandleValue> const out0,
                        user_copy::UserOutPtr<HandleValue> const out1) -> Status {
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

    auto handle0 = Handle::make(KernelHandle<object::Dispatcher>(ustl::move(end0)), rights);
    if (!handle0) {
        return Status::NoResource;
    }
    auto handle1 = Handle::make(KernelHandle<object::Dispatcher>(ustl::move(end1)), rights);
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

auto sys_channel_write(HandleValue const channel, ChannelWriteOptions const options,
                       user_copy::UserInPtr<u8 const> const bytes, u32 const num_bytes,
                       [[maybe_unused]] user_copy::UserInPtr<HandleValue const> const handles,
                       u32 const num_handles) -> Status {
    if (options != ChannelWriteOptions::None) {
        return Status::InvArgs;
    }
    // Handles in messages are P1: they need transfer accounting and the
    // "consumed whether or not the write succeeds" rule. Refused outright
    // rather than silently dropped, so no program can come to rely on it.
    if (num_handles != 0) {
        return Status::NotSupported;
    }
    if (num_bytes > CHANNEL_MAX_MSG_BYTES) {
        return Status::OutOfRange;
    }

    ustl::Arc<ChannelDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            channel, Rights::Write, &dispatcher);
        status != Status::Ok) {
        return status;
    }

    // The copy from user memory, before the channel's lock is taken.
    ustl::Box<MessagePacket> message;
    if (auto const status = MessagePacket::create(bytes, num_bytes, &message);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->write(ustl::move(message));
}

auto sys_channel_read(HandleValue const channel, ChannelReadOptions const options,
                      user_copy::UserOutPtr<u8> const bytes,
                      [[maybe_unused]] user_copy::UserOutPtr<HandleValue> const handles,
                      u32 const num_bytes, [[maybe_unused]] u32 const num_handles,
                      user_copy::UserOutPtr<u32> const actual_bytes,
                      user_copy::UserOutPtr<u32> const actual_handles) -> Status {
    if (options != ChannelReadOptions::None) {
        return Status::InvArgs;
    }

    ustl::Arc<ChannelDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            channel, Rights::Read, &dispatcher);
        status != Status::Ok) {
        return status;
    }

    u32 length = 0;
    ustl::Box<MessagePacket> message;
    auto const status = dispatcher->read(num_bytes, &length, &message);

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
            if (auto const copied = actual_handles.copy_to_user(u32{0}); copied != Status::Ok) {
                return copied;
            }
        }
    }
    if (status != Status::Ok) {
        return status;
    }

    // After the channel's lock was released inside read(). A fault here loses
    // the message, which has already left the queue -- Zircon's
    // sys_channel_read() makes the same trade: the alternative is copying to
    // user memory with a channel lock held.
    return message->copy_data_to(bytes);
}
} // namespace ours
