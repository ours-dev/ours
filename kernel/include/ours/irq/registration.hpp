/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_IRQ_REGISTRATION_HPP
#define OURS_IRQ_REGISTRATION_HPP

#include <ours/irq/types.hpp>

#include <atomic>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::irq {
class Line;

/// One handler hung on an interrupt line: the function to call and the
/// context it is called with.
///
/// Whoever registers a handler owns its action -- a kernel driver keeps it
/// in a static, an interrupt object on a line embeds it -- and passes it to
/// irq::attach(). The irq layer never allocates one, so a failed attach has
/// nothing to free, and irq::detach() names the very action to take off
/// the line rather than a line number that cannot tell two actions apart.
///
/// The owner keeps the action alive, and at the same address, until
/// irq::detach() has returned: the line links it into its list through
/// `m_hook`. The hook is a SafeLink one, so destroying an action that is
/// still linked traps instead of leaving the line a dangling pointer, and
/// the class can be neither copied nor moved.
///
/// The owner also never attaches and detaches one action at the same time
/// on two CPUs. The line's lock covers the line, not the action: detach()
/// reads `m_virqnum` to find the line before it holds that line's lock, and
/// attach() reads it under the lock of the line it is about to join, which
/// is not the lock of a line the action may be leaving.
class Registration final {
    using Self = Registration;

  public:
    struct Callbacks {
        IrqHandler handle = nullptr;
        IrqCancel revoke = nullptr;
        IrqCompletionCheck ready = nullptr;
    };

    /// The callbacks share one caller-owned context. Revoke opts into domain
    /// removal; Ready is required for deferred work and is called under L1
    /// before completion may reopen the line.
    constexpr Registration(Callbacks const callbacks, void *const context, IrqFlags const flags,
                           char const *const name) noexcept
        : m_callbacks(callbacks), m_context(context), m_flags(flags), m_name(name) {}

    Registration(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    [[nodiscard]]
    auto on_event() const -> IrqReturn {
        return m_callbacks.handle(virqnum(), m_context);
    }

    [[nodiscard]]
    constexpr auto is_shared() const -> bool {
        return !!(m_flags & IrqFlags::Shared);
    }

    /// Asks for the line for good: it takes only an empty line, and the line
    /// then refuses every other attach and refuses to let this one go.
    [[nodiscard]]
    constexpr auto is_permanent() const -> bool {
        return !!(m_flags & IrqFlags::Permanent);
    }

    /// Asks the line to stay masked after an interrupt this action answers
    /// WakeThread for, until irq::finalize_oneshot().
    [[nodiscard]]
    constexpr auto is_oneshot() const -> bool {
        return !!(m_flags & IrqFlags::Oneshot);
    }

    /// Whether the kernel may take this action off its line.
    [[nodiscard]]
    auto is_revocable() const -> bool {
        return m_callbacks.revoke != nullptr;
    }

    /// Tells the owner the kernel is taking the action off its line; the
    /// line calls it, with its lock held, after unlinking the action.
    auto cancel() const -> void {
        if (m_callbacks.revoke) {
            m_callbacks.revoke(virqnum(), m_context);
        }
    }

    /// The line this action is attached to, or INVALID_V_IRQ_NUM when it
    /// is on none. An acquire load: a reader that sees INVALID after a
    /// removal also sees everything the removal did to the action and its
    /// owner's state first, which is what lets irq::detach() answer NotFound
    /// for an action a removal has finished with, without taking the line's
    /// lock.
    [[nodiscard]]
    auto virqnum() const -> VIrqNum {
        return m_virqnum.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    auto is_attached() const -> bool {
        return virqnum() != INVALID_V_IRQ_NUM;
    }

  private:
    // Only the line may say which line an action is on: Line::attach() and
    // detach() write `m_virqnum` together with linking `m_hook`, so the two
    // cannot disagree.
    friend class Line;

    [[nodiscard]]
    constexpr auto is_valid() const -> bool {
        constexpr auto OPTIONS = IrqFlags::Shared | IrqFlags::Permanent | IrqFlags::Oneshot;
        if (!m_callbacks.handle || !!(m_flags & ~OPTIONS)) {
            return false;
        }
        if (is_permanent() && (is_shared() || is_oneshot())) {
            return false;
        }
        if (is_oneshot() && !m_callbacks.ready) {
            return false;
        }
        return !(is_shared() && is_oneshot());
    }

    [[nodiscard]]
    auto ready_to_complete() const -> bool {
        return m_callbacks.ready != nullptr && m_callbacks.ready(m_context);
    }

    Callbacks const m_callbacks;
    void *const m_context;
    IrqFlags const m_flags;
    [[maybe_unused]]
    char const *const m_name;
    /// Written under the lock of the line it names, and by a removal as the
    /// last thing it does to the action (Line::revoke()); read without that
    /// lock by detach() to find the line.
    std::atomic<VIrqNum> m_virqnum{INVALID_V_IRQ_NUM};
    ustl::collections::intrusive::ListMemberHook<> m_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_hook, ManagedOption);
    USTL_DECLARE_LIST(Self, List, ManagedOption);
};
} // namespace ours::irq

#endif // OURS_IRQ_REGISTRATION_HPP
