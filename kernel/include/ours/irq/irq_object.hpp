/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_OBJECT_HPP
#define OURS_IRQ_IRQ_OBJECT_HPP 1

#include <ours/irq/irq_chip.hpp>
#include <ours/irq/irq_observer.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>

#include <gktl/canary.hpp>

namespace ours::irq {
class IrqObject {
    using Self = IrqObject;

  public:
    static auto create(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name) -> Self *;

    auto init(IrqChip *chip) -> Status;
    auto attach(IrqObserver &observer) -> Status;
    auto detach(IrqObserver &observer) -> Status;
    auto notify() -> void;

    [[nodiscard]]
    auto is_shared() const -> bool {
        return !!(irqflags_ & IrqFlags::Shared);
    }

  private:
    IrqObject(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name);

    gktl::Canary canary_;
    sync::Mutex request_mutex_;
    IrqFlags irqflags_;
    IrqData irqdata_;
    IrqObserver::List observers_;
    [[maybe_unused]]
    char const *name_;
};
} // namespace ours::irq

#endif // OURS_IRQ_IRQ_OBJECT_HPP
