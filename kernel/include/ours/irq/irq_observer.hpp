/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_OBSERVER_HPP
#define OURS_IRQ_IRQ_OBSERVER_HPP 1

#include <ours/irq/types.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::irq {
class IrqObject;

/// One handler hung on an interrupt line: the function to call and the
/// context it is called with.
///
/// Whoever registers a handler owns its observer -- a kernel driver keeps it
/// in a static, an interrupt object on a line embeds it -- and passes it to
/// irq::attach(). The irq layer never allocates one, so a failed attach has
/// nothing to free, and irq::detach() names the very observer to take off
/// the line rather than a line number that cannot tell two observers apart.
///
/// The owner keeps the observer alive, and at the same address, until
/// irq::detach() has returned: the line links it into its list through
/// `m_hook`. The hook is a SafeLink one, so destroying an observer that is
/// still linked traps instead of leaving the line a dangling pointer, and
/// the class can be neither copied nor moved.
///
/// The owner also never attaches and detaches one observer at the same time
/// on two CPUs. The line's lock covers the line, not the observer: detach()
/// reads `m_virqnum` to find the line before it holds that line's lock, and
/// attach() reads it under the lock of the line it is about to join, which
/// is not the lock of a line the observer may be leaving.
class IrqObserver final {
    using Self = IrqObserver;

  public:
    /// `context` is handed back to `handler` on every interrupt, untouched.
    constexpr IrqObserver(IrqHandler const handler, void *const context, IrqFlags const flags,
                          char const *const name) noexcept
        : m_handler(handler), m_context(context), m_flags(flags), m_name(name) {}

    IrqObserver(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    [[nodiscard]]
    auto on_event() const -> IrqReturn {
        return m_handler(m_virqnum, m_context);
    }

    [[nodiscard]]
    auto is_shared() const -> bool {
        return !!(m_flags & IrqFlags::Shared);
    }

    /// Asks for the line for good: it takes only an empty line, and the line
    /// then refuses every other attach and refuses to let this one go.
    [[nodiscard]]
    auto is_permanent() const -> bool {
        return !!(m_flags & IrqFlags::Permanent);
    }

    /// The line this observer is attached to, or INVALID_V_IRQ_NUM when it
    /// is on none.
    [[nodiscard]]
    auto virqnum() const -> VIrqNum {
        return m_virqnum;
    }

    [[nodiscard]]
    auto is_attached() const -> bool {
        return m_virqnum != INVALID_V_IRQ_NUM;
    }

  private:
    // Only the line may say which line an observer is on: IrqObject::attach()
    // and detach() write `m_virqnum` together with linking `m_hook`, so the two
    // cannot disagree.
    friend class IrqObject;

    IrqHandler m_handler;
    void *m_context;
    IrqFlags m_flags;
    [[maybe_unused]]
    char const *m_name;
    VIrqNum m_virqnum = INVALID_V_IRQ_NUM;
    ustl::collections::intrusive::ListMemberHook<> m_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_hook, ManagedOption);
    USTL_DECLARE_LIST(Self, List, ManagedOption);
};
} // namespace ours::irq

#endif // OURS_IRQ_IRQ_OBSERVER_HPP
