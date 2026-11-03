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
/// Whoever registers a handler owns its registration -- a kernel driver keeps it
/// in a static, an interrupt object on a line embeds it -- and passes it to
/// irq::attach(). The irq layer never allocates one, so a failed attach has
/// nothing to free, and irq::detach() names the very registration to take off
/// the line rather than a line number that cannot tell two registrations apart.
///
/// The owner keeps the registration alive, and at the same address, until
/// irq::detach() has returned: the line links it into its list through
/// `m_hook`. The hook is a SafeLink one, so destroying a registration that is
/// still linked traps instead of leaving the line a dangling pointer, and
/// the class can be neither copied nor moved.
///
/// The owner also never attaches and detaches one registration at the same time
/// on two CPUs. The line's lock covers the line, not the registration: detach()
/// reads `m_line` to find the line before it holds that line's lock, and
/// attach() reads it under the lock of the line it is about to join, which
/// is not the lock of a line the registration may be leaving.
class Registration final {
    using Self = Registration;

  public:
    struct Callbacks {
        IrqHandler handle = nullptr;
        IrqRevocationHandler revoke = nullptr;
        IrqCompletionCheck ready = nullptr;
    };

    /// The callbacks share one caller-owned context. Revoke opts into domain
    /// removal; Ready is required for deferred work and is called under L1
    /// before completion may reopen the line.
    constexpr Registration(Callbacks const callbacks, void *const context,
                           RegistrationOptions const options) noexcept
        : m_callbacks(callbacks), m_context(context), m_options(options) {}

    Registration(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    [[nodiscard]]
    constexpr auto is_shared() const -> bool {
        return !!(m_options & RegistrationOptions::Shared);
    }

    /// Asks for the line for good: it takes only an empty line, and the line
    /// then refuses every other attach and refuses to let this one go.
    [[nodiscard]]
    constexpr auto is_permanent() const -> bool {
        return !!(m_options & RegistrationOptions::Permanent);
    }

    /// Asks the line to stay masked after an interrupt this registration answers
    /// WakeThread for, until irq::finalize_oneshot().
    [[nodiscard]]
    constexpr auto is_oneshot() const -> bool {
        return !!(m_options & RegistrationOptions::Oneshot);
    }

    /// The line this registration is attached to, or IrqId::Invalid when it
    /// is on none. An acquire load: a reader that sees INVALID after a
    /// removal also sees everything the removal did to the registration and its
    /// owner's state first, which is what lets irq::detach() answer NotFound
    /// for a registration a removal has finished with, without taking the line's
    /// lock.
    [[nodiscard]]
    auto line_id() const -> IrqId {
        return m_line.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    auto is_attached() const -> bool {
        return line_id() != IrqId::Invalid;
    }

  private:
    // Only the line may say which line a registration is on: Line::attach() and
    // detach() write `m_line` together with linking `m_hook`, so the two
    // cannot disagree.
    friend class Line;

    [[nodiscard]]
    auto on_event() const -> IrqReturn {
        return m_callbacks.handle(line_id(), m_context);
    }

    /// Whether the kernel may take this registration off its line.
    [[nodiscard]]
    auto is_revocable() const -> bool {
        return m_callbacks.revoke != nullptr;
    }

    /// Tells the owner the kernel is taking the registration off its line; the
    /// line calls it, with its lock held, after unlinking the registration.
    auto notify_revoked() const -> void {
        if (m_callbacks.revoke) {
            m_callbacks.revoke(line_id(), m_context);
        }
    }

    [[nodiscard]]
    constexpr auto is_valid() const -> bool {
        constexpr auto OPTIONS = RegistrationOptions::Shared | RegistrationOptions::Permanent |
                                 RegistrationOptions::Oneshot;
        if (!m_callbacks.handle || !!(m_options & ~OPTIONS)) {
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
    RegistrationOptions const m_options;
    /// Written under the lock of the line it names, and by a removal as the
    /// last thing it does to the registration (Line::revoke()); read without that
    /// lock by detach() to find the line.
    std::atomic<IrqId> m_line{IrqId::Invalid};
    ustl::collections::intrusive::ListMemberHook<> m_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_hook, ManagedOption);
    USTL_DECLARE_LIST(Self, List, ManagedOption);
};
} // namespace ours::irq

#endif // OURS_IRQ_REGISTRATION_HPP
