/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_OBSERVER_HPP
#define OURS_IRQ_IRQ_OBSERVER_HPP 1

#include <ours/irq/types.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::irq {
class IrqObserver {
    using Self = IrqObserver;

  public:
    IrqObserver(VIrqNum const virqnum, IrqFlags const flags, IrqHandler const handler,
                char const *const name) noexcept
        : virqnum_(virqnum), flags_(flags), handler_(handler), name_(name) {}

    [[nodiscard]]
    auto on_event() const -> IrqReturn {
        return handler_(virqnum_, nullptr);
    }

    [[nodiscard]]
    auto is_shared() const -> bool {
        return !!(flags_ & IrqFlags::Shared);
    }

  private:
    VIrqNum virqnum_;
    IrqFlags flags_;
    IrqHandler handler_;
    [[maybe_unused]]
    char const *name_;
    ustl::collections::intrusive::ListMemberHook<> managed_hook_;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, managed_hook_, ManagedOption);
    USTL_DECLARE_LIST(Self, List, ManagedOption);
};
} // namespace ours::irq

#endif // OURS_IRQ_IRQ_OBSERVER_HPP
