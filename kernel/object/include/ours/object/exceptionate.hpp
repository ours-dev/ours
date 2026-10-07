/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_EXCEPTIONATE_HPP
#define OURS_OBJECT_EXCEPTIONATE_HPP 1

#include <ours/object/channel_dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>

#include <ustl/rc.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::object {
class ExceptionDispatcher;

/// The kernel's end of one exception channel, kept by the task the channel is
/// bound to -- a process, in slice 1 of .agents/tasks/exception-channels/.
/// Not a kernel object: user space holds the other end of the channel and
/// never names this.
///
/// Its lock is the order's next after the ThreadDispatcher lock and before
/// PeerHolder: a send runs with the faulting thread's lock held (the user's
/// call, 2026-10-04) and writes the channel under it.
class Exceptionate {
    using Self = Exceptionate;

  public:
    Exceptionate() = default;

    /// Shuts down, so that a channel still bound when its task goes without
    /// dying first tells its reader.
    ~Exceptionate();

    USTL_NO_MOVEABLE_AND_COPYABLE(Exceptionate);

    /// Binds |channel|, the kernel's end of a fresh channel; the caller hands
    /// the other end to user space. |thread_rights| and |process_rights| are
    /// what a handler on it gets on the handles it asks an exception for
    /// (ExceptionDispatcher::set_task_rights()); None withholds one. First
    /// come, first served:
    ///
    ///   - InvArgs when |channel| is empty;
    ///   - BadState after shutdown();
    ///   - AlreadyBound while a channel is bound and its reader is still
    ///     there. One whose reader has gone is replaced: a channel end never
    ///     reopens.
    ///
    /// On any refusal |channel| is dropped, and the other end sees
    /// PeerClosed.
    auto set_channel(KernelHandle<ChannelDispatcher> channel, Rights thread_rights,
                     Rights process_rights) -> Status;

    /// Unbinds the channel, if any, and refuses every later set_channel():
    /// the reader sees PeerClosed. Called as the task dies, and must not run
    /// under the task's own lock, since dropping the kernel end runs its
    /// on_zero_handles() (hard rule 1, userspace-interface.md 4.8).
    auto shutdown() -> void;

    /// Sends |exception| on the bound channel: an ExceptionInfo, and the
    /// exception's one handle, which no process is charged for.
    ///
    ///   - Ok: the message is in the channel, and the caller must now
    ///     wait_for_handle_close();
    ///   - PeerClosed: either no channel is bound, or its reader has gone;
    ///   - NoResource, NoMemory: the message could not be made or queued.
    ///
    /// Every answer but Ok means "try the next handler", and leaves
    /// |exception| ready for it.
    ///
    /// By value: ustl::Rc's constness is deep, so a const reference would
    /// give only const access to the exception, which a failed send resets.
    auto send(ustl::Arc<ExceptionDispatcher> exception) -> Status;

  private:
    /// A channel is bound and its reader is still there.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto has_valid_channel_locked() const -> bool;

    sync::Mutex m_lock;

    KernelHandle<ChannelDispatcher> m_channel TA_GUARDED(m_lock);

    /// The exceptionate's yes/no facts, as one word: every bit is written
    /// under m_lock.
    enum class Flags : u8 {
        None,
        /// shutdown() has run: the channel is gone, and set_channel() binds
        /// no other.
        Shutdown = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    Flags m_flags TA_GUARDED(m_lock) = Flags::None;

    /// See set_channel().
    Rights m_thread_rights TA_GUARDED(m_lock) = Rights::None;
    Rights m_process_rights TA_GUARDED(m_lock) = Rights::None;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_EXCEPTIONATE_HPP
