/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_DISPATCHER_HPP
#define OURS_IRQ_IRQ_DISPATCHER_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <ustl/views/span.hpp>

namespace ours::irq {
class IrqObject;

class IrqDispatcher {
  public:
    auto init(usize nr_irqs) -> Status;
    auto request_irq(VIrqNum irqnum, IrqHandler handler, IrqFlags flags, char const *name)
        -> Status;
    auto release_irq(VIrqNum virqnum) -> Status;
    auto handle_irq(VIrqNum virqnum) -> IrqReturn;

  private:
    auto irqnum_to_object(VIrqNum virqnum) -> IrqObject *;

    ustl::views::Span<IrqObject *> objects_;
};
} // namespace ours::irq

#endif // OURS_IRQ_IRQ_DISPATCHER_HPP
