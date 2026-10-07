/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/exception.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/exception_dispatcher.hpp>
#include <ours/object/exceptionate.hpp>
#include <ours/object/message_packet.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>

#include <span>

#include <user_copy/copy_allowed.hpp>

namespace ours::object {
Exceptionate::~Exceptionate() {
    shutdown();
}

auto Exceptionate::set_channel(KernelHandle<ChannelDispatcher> channel, Rights const thread_rights,
                               Rights const process_rights) -> Status {
    if (!channel.dispatcher()) {
        return Status::InvArgs;
    }

    Guard guard{m_lock};
    if (!!(m_flags & Flags::Shutdown)) {
        return Status::BadState;
    }
    if (has_valid_channel_locked()) {
        return Status::AlreadyBound;
    }

    // None, or one whose reader has gone for good. Dropping that one runs its
    // on_zero_handles(), which takes its PeerHolder: after this lock, in the
    // order.
    m_channel = ustl::move(channel);
    m_thread_rights = thread_rights;
    m_process_rights = process_rights;
    return Status::Ok;
}

auto Exceptionate::shutdown() -> void {
    Guard guard{m_lock};
    m_channel = KernelHandle<ChannelDispatcher>();
    m_flags |= Flags::Shutdown;
}

auto Exceptionate::has_valid_channel_locked() const -> bool {
    auto const &channel = m_channel.dispatcher();
    return channel && !(channel->signals() & Signals::PeerClosed);
}

auto Exceptionate::send(ustl::Arc<ExceptionDispatcher> exception) -> Status {
    DEBUG_ASSERT(exception);

    Guard guard{m_lock};
    // Named here, under the lock, rather than read inside the lambda below:
    // the thread-safety analysis checks a lambda as a function of its own,
    // which holds no lock, although this one runs under m_lock.
    auto &channel = m_channel.dispatcher();
    if (!channel) {
        return Status::PeerClosed;
    }

    // Copied byte for byte into a message user space reads, so it must have
    // no byte the kernel did not write: no implicit padding.
    static_assert(user_copy::CopyAllowed<ExceptionInfo>);
    auto const &thread = *exception->thread();
    auto const info = ExceptionInfo{
        .pid = thread.process().koid(),
        .tid = thread.koid(),
        .type = exception->exception_type(),
        .padding1 = {},
    };

    // Before the handle leaves the kernel: once the handler has it, it may ask
    // for the task handles at once. A failed send leaves them set, and the
    // next send overwrites them.
    exception->set_task_rights(m_thread_rights, m_process_rights);

    // The handle before the packet: MessagePacket::create() takes finished
    // handles. Charged to no process (the user's call, 2026-10-04): there is
    // at most one per blocked thread, and charging the faulting process
    // would let a process at its quota die of an exception it could not be
    // told about.
    ustl::Box<Handle> handles[1] = {
        Handle::make_uncharged(exception, ExceptionDispatcher::default_right()),
    };
    auto const status = [&] {
        if (!handles[0]) {
            return Status::NoResource;
        }
        ustl::Box<MessagePacket> packet;
        auto const bytes = std::span{reinterpret_cast<u8 const *>(&info), sizeof(info)};
        if (auto const made = MessagePacket::create(bytes, handles, 1, &packet);
            Status::Ok != made) {
            return made;
        }
        // A packet write() refuses is freed before it returns, and with it the
        // handle, whose on_zero_handles() then signals the exception.
        return channel->write(ustl::move(packet));
    }();

    if (Status::Ok != status) {
        // A handle create() refused is still here, and has to be gone before
        // the reset below: its close would signal again after it.
        handles[0] = ustl::Box<Handle>();
        exception->discard_handle_close();
    }
    if (Status::PeerClosed == status) {
        // The reader has gone; the next set_channel() may bind afresh.
        m_channel = KernelHandle<ChannelDispatcher>();
    }
    return status;
}
} // namespace ours::object
